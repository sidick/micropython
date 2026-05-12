"""Python-side facade for the `amiga` module.

The C extension is registered as `_amiga` (see ports/amiga/modamiga.c).
This frozen module wraps it: it re-exports every name `_amiga` provides
(`lib_open`, `lib_call`, `find_task`, `volumes`, the `MEMF_*` flags,
etc.) and adds the higher-level `Library` proxy that consumes the
`.fd`-derived signature table in `_amiga_fd.LIBRARIES`.

Typical use:

    import amiga

    with amiga.library("intuition.library", 37) as intuition:
        intuition.DisplayBeep(0)
"""

from _amiga import *  # noqa: F401,F403  (the underscore module is the source of truth)
import _amiga as _c
import _amiga_fd
import _amiga_tags


_VALID_REGS = frozenset((
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5",
))


def _parse_fd(text):
    """Pure-Python `.fd` parser — runtime counterpart to tools/amiga-fdgen.py.

    Returns a dict mapping function name to a `(lvo, regs_csv, "")`
    triple.  Functions inside `##private` sections still consume LVO
    slots but are dropped from the output.  Functions whose arg / reg
    counts disagree (the IEEE-double convention used in
    `mathieeedoub*_lib.fd`) or that target A6 (cia.resource's special
    base-register ABI) are skipped silently — they're rare and the
    trampoline can't handle them without per-function help anyway.
    """
    bias = None
    slot = 0
    public = True
    out = {}
    for raw in text.split("\n"):
        line = raw.strip()
        if not line or line[0] == "*":
            continue
        if line.startswith("##"):
            parts = line.split(None, 1)
            directive = parts[0]
            arg = parts[1].strip() if len(parts) > 1 else ""
            if directive == "##bias":
                try:
                    bias = int(arg)
                except ValueError:
                    continue
                slot = 0
            elif directive == "##public":
                public = True
            elif directive == "##private":
                public = False
            elif directive == "##end":
                break
            continue
        # Function line:  Name(args)(regs)
        lp1 = line.find("(")
        rp1 = line.find(")", lp1 + 1) if lp1 != -1 else -1
        lp2 = line.find("(", rp1 + 1) if rp1 != -1 else -1
        rp2 = line.find(")", lp2 + 1) if lp2 != -1 else -1
        if lp1 == -1 or rp1 == -1 or lp2 == -1 or rp2 == -1:
            continue
        if bias is None:
            continue
        name = line[:lp1].strip()
        args_csv = line[lp1 + 1:rp1]
        regs_csv = line[lp2 + 1:rp2]
        lvo = -(bias + slot * 6)
        slot += 1
        if not public:
            continue
        args = [a.strip() for a in args_csv.split(",") if a.strip()]
        regs = []
        for chunk in regs_csv.split(","):
            for r in chunk.split("/"):
                r = r.strip().lower()
                if r:
                    regs.append(r)
        if len(args) != len(regs) or any(r not in _VALID_REGS for r in regs):
            continue
        out[name] = (lvo, ",".join(regs), "")
    return out


# Search path for runtime .fd lookup.  PROGDIR: is automatically
# assigned by AmigaDOS to the directory of the running executable;
# LIBS: is the system-wide libraries assign.  Files are tried in the
# order listed, with both the bebbo `<name>_lib.fd` and bare
# `<name>.fd` conventions accepted.
_FD_SEARCH_PATH = ("PROGDIR:fd/", "LIBS:fd/")


def _load_fd(name):
    """Search the runtime .fd path for a library and return its parsed
    signature dict, or None if no match is found."""
    for suffix in (".library", ".device", ".resource"):
        if name.endswith(suffix):
            base = name[:-len(suffix)]
            break
    else:
        base = name
    for prefix in _FD_SEARCH_PATH:
        for suffix in ("_lib.fd", ".fd"):
            path = prefix + base + suffix
            try:
                f = open(path, "r")
            except OSError:
                continue
            try:
                text = f.read()
            finally:
                f.close()
            return _parse_fd(text)
    return None


def _normalize_signatures(sigs):
    """Accept a user-supplied signature dict in either 2-tuple
    `(lvo, regs_csv)` or 3-tuple `(lvo, regs_csv, meta)` form and
    normalize to the 3-tuple form used internally."""
    out = {}
    for fname, sig in sigs.items():
        if len(sig) == 2:
            out[fname] = (sig[0], sig[1], "")
        else:
            out[fname] = (sig[0], sig[1], sig[2])
    return out


class Library:
    """Open an AmigaOS library / device / resource and dispatch its
    functions by name using the frozen `.fd` signature table.

    The opened base is held on the instance and released on `close()`,
    on context-manager exit, or on GC.  Attribute access looks the
    function up in `_amiga_fd.LIBRARIES`, builds a thin closure that
    marshals positional args into the right registers, calls through
    `_amiga.lib_call`, and caches the closure on `self.__dict__` so
    subsequent reads skip `__getattr__`.
    """

    def __init__(self, name, version=0, signatures=None):
        # Resolve the signature table in priority order:
        #   1. explicit `signatures=` kwarg (caller override)
        #   2. frozen NDK-derived table in `_amiga_fd.LIBRARIES`
        #   3. runtime .fd search path (PROGDIR:fd/, LIBS:fd/)
        #   4. empty (calls will raise AttributeError per-function)
        self._name = name
        if signatures is not None:
            self._signatures = _normalize_signatures(signatures)
        else:
            sigs = _amiga_fd.LIBRARIES.get(name)
            if sigs is None:
                sigs = _load_fd(name) or {}
            self._signatures = sigs
        self._base = _c.lib_open(name, version)

    @property
    def base(self):
        """Raw library base, for callers who want to drop down to `lib_call` directly."""
        return self._base

    @property
    def name(self):
        return self._name

    def close(self):
        if self._base:
            _c.lib_close(self._base)
            self._base = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def __del__(self):
        # Best-effort close on GC; ignore failures since __del__ can't
        # surface them cleanly.
        try:
            if self._base:
                _c.lib_close(self._base)
                self._base = 0
        except Exception:
            pass

    def __repr__(self):
        state = "closed" if not self._base else hex(self._base)
        return "<Library %s @ %s>" % (self._name, state)

    def __getattr__(self, fnname):
        # __getattr__ runs only when the normal attribute lookup misses,
        # so once we cache the closure on self.__dict__ subsequent reads
        # bypass this method entirely.
        if fnname.startswith("_"):
            raise AttributeError(fnname)
        sig = self._signatures.get(fnname)
        if sig is None:
            raise AttributeError(
                "%s has no function %r in its .fd table" % (self._name, fnname)
            )
        lvo, regs_csv, _since = sig
        regs = regs_csv.split(",") if regs_csv else []
        nargs = len(regs)
        # Closure over the bound self so close() invalidates calls
        # transparently (rather than the closure clinging to a stale base).
        lib = self

        def call(*args):
            if len(args) != nargs:
                raise TypeError(
                    "%s.%s: expected %d arg(s), got %d"
                    % (lib._name, fnname, nargs, len(args))
                )
            if not lib._base:
                raise ValueError("%s: closed" % lib._name)
            # Anything that defines __int__ (e.g. TagList) is unwrapped to
            # its address transparently — this is what makes
            #   intuition.OpenWindowTagList(0, taglist) work.
            kw = {}
            for i in range(nargs):
                v = args[i]
                if not isinstance(v, int):
                    v = int(v)
                kw[regs[i]] = v
            return _c.lib_call(lib._base, lvo, **kw)

        # MicroPython closures don't support `__name__` assignment, so
        # the wrapper stays anonymous.  Cache it on the instance via
        # setattr so subsequent attribute reads bypass __getattr__.
        setattr(self, fnname, call)
        return call


def library(name, version=0, signatures=None):
    """Convenience factory equivalent to `Library(name, version, signatures=...)`."""
    return Library(name, version, signatures=signatures)


def library_from_signatures(name, version, signatures):
    """Open `name` and dispatch its functions using `signatures` directly,
    bypassing both `_amiga_fd.LIBRARIES` and the runtime .fd search path.

    Useful for libraries that ship no `.fd` file at all (e.g. proprietary
    third-party libs), or for overriding a baked-in signature without
    rebuilding the binary.  `signatures` is a dict mapping function name
    to `(lvo, regs_csv)` or `(lvo, regs_csv, metadata)`.
    """
    return Library(name, version, signatures=signatures)


class TagList:
    """A `TAG_DONE`-terminated array of AmigaOS TagItems.

    Built from a sequence of `(tag_id, value)` pairs:

        tags = TagList([(WA_Width, 640), (WA_Height, 400), (WA_Title, "hi")])
        amiga.lib_call(intuition_base, -610,
                       a0=newWindow_ptr, a1=tags.addr)

    Integer values go straight into the TagItem's `ti_Data` field;
    bytes / str values are copied into a per-string buffer that
    `TagList` owns, with the buffer's address stored as `ti_Data`.
    All allocations are released when the object is closed (`close()`,
    `__exit__`, or `__del__`).

    Passing a `TagList` directly to a `Library` proxy call works
    transparently — the proxy unwraps it via `__int__` (which returns
    the head address of the TagItem array).
    """

    # Each TagItem is two ULONGs (ti_Tag, ti_Data) = 8 bytes; the
    # array is terminated by a TAG_DONE-marker pair.
    _ITEM_SIZE = 8

    def __init__(self, items):
        items = list(items)
        n = len(items)
        self._main = _c.alloc_vec(
            (n + 1) * self._ITEM_SIZE,
            _c.MEMF_ANY | _c.MEMF_CLEAR,
        )
        self._buffers = []
        try:
            for i, pair in enumerate(items):
                tag, val = pair
                slot = self._main + i * self._ITEM_SIZE
                _c.poke_l(slot, int(tag))
                if isinstance(val, str):
                    payload = val.encode("ascii") + b"\x00"
                elif isinstance(val, (bytes, bytearray)):
                    payload = bytes(val)
                    if not payload.endswith(b"\x00"):
                        payload = payload + b"\x00"
                else:
                    payload = None
                if payload is None:
                    _c.poke_l(slot + 4, int(val))
                else:
                    buf = _c.alloc_vec(len(payload), _c.MEMF_ANY)
                    _c.poke_bytes(buf, payload)
                    _c.poke_l(slot + 4, buf)
                    self._buffers.append(buf)
            # TAG_DONE terminator (alloc_vec returned a CLEARed array
            # so the terminator slot is already zero, but be explicit).
            term = self._main + n * self._ITEM_SIZE
            _c.poke_l(term, 0)
            _c.poke_l(term + 4, 0)
        except BaseException:
            self.close()
            raise

    @property
    def addr(self):
        """Address of the head of the TagItem array."""
        return self._main

    def __int__(self):
        return self._main

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def close(self):
        if self._buffers:
            for b in self._buffers:
                _c.free_vec(b)
            self._buffers = []
        if self._main:
            _c.free_vec(self._main)
            self._main = 0

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self):
        state = "closed" if not self._main else hex(self._main)
        return "<TagList @ %s>" % state


def taglist(*pairs, **named):
    """Build a `TagList` from kwargs and/or positional pairs.

    Kwargs are looked up by name in `_amiga_tags.TAGS`; unknown names
    raise `KeyError`.  Positional args can be either a single iterable
    of `(tag, value)` pairs or an alternating sequence of `tag, value,
    tag, value, ...`.  Tag values may be ints, `bytes`, or `str`; the
    last two cause a buffer to be allocated and the pointer stored.

        with amiga.taglist(WA_Width=640, WA_Height=480, WA_Title="hi") as tl:
            intuition.OpenWindowTagList(0, tl)

        # Or with literal IDs (handy for tags not in _amiga_tags.TAGS):
        tl = amiga.taglist((0x80000063 + 0x03, 640), (0x80000063 + 0x04, 480))
    """
    items = []
    if not pairs:
        pass
    elif len(pairs) == 1 and not isinstance(pairs[0], int):
        # One arg, an iterable of (tag, value) pairs:
        #   taglist([(WA_Width, 640), (WA_Height, 480)])
        for pair in pairs[0]:
            items.append((pair[0], pair[1]))
    elif isinstance(pairs[0], int):
        # Alternating tag, value, tag, value, ...
        #   taglist(WA_Width, 640, WA_Height, 480)
        if len(pairs) % 2 != 0:
            raise ValueError(
                "taglist alternating positional args must be in (tag, value) pairs"
            )
        for i in range(0, len(pairs), 2):
            items.append((pairs[i], pairs[i + 1]))
    else:
        # Multiple positional (tag, value) tuples:
        #   taglist((WA_Width, 640), (WA_Height, 480))
        for pair in pairs:
            items.append((pair[0], pair[1]))
    for name, val in named.items():
        tag_id = _amiga_tags.TAGS.get(name)
        if tag_id is None:
            raise KeyError("unknown tag name: %r" % name)
        items.append((tag_id, val))
    return TagList(items)


# ---------- Phase 17 step 6: ctypes-lite struct accessors ----------
#
# `Struct(addr, layout)` wraps a C struct living at `addr` and gives
# Python-attribute access to its fields, dispatching through the
# `_amiga.peek_*` / `poke_*` primitives.  `layout` is a `dict`
# mapping field name to `(offset, type_code)`, where the type code is:
#
#   'B'   unsigned byte   (1)
#   'b'   signed byte     (1)
#   'H'   unsigned word   (2, big-endian native)
#   'h'   signed word     (2)
#   'L'   unsigned long   (4)
#   'l'   signed long     (4)
#   'P'   pointer         (4, alias for 'L')
#   'sN'  raw N-byte region — read returns bytes; assign accepts bytes
#   'S'   NUL-terminated string pointed at by a ULONG slot — read
#         dereferences and returns bytes up to the first NUL (cap 256);
#         not writable through the accessor.
#
# `ports/amiga/modules/_amiga_structs.py` ships hand-curated layouts
# for `Node`, `Task`, `Library`, `DateStamp`, `FileInfoBlock`, and
# `IntuiMessage`, with per-struct factory functions re-exported here
# (`amiga.Task(addr)`, `amiga.Library_struct(addr)`, ...).

import _amiga_structs as _structs


def _struct_read(addr, typ):
    if typ == "B":
        return _c.peek_b(addr)
    if typ == "b":
        v = _c.peek_b(addr)
        return v - 256 if v >= 128 else v
    if typ == "H":
        return _c.peek_w(addr)
    if typ == "h":
        v = _c.peek_w(addr)
        return v - 65536 if v >= 32768 else v
    if typ in ("L", "P"):
        return _c.peek_l(addr) & 0xffffffff
    if typ == "l":
        v = _c.peek_l(addr) & 0xffffffff
        return v if v < 0x80000000 else v - 0x100000000
    if typ == "S":
        ptr = _c.peek_l(addr) & 0xffffffff
        if ptr == 0:
            return None
        out = bytearray()
        i = 0
        while i < 256:
            b = _c.peek_b(ptr + i)
            if b == 0:
                break
            out.append(b)
            i += 1
        return bytes(out)
    if typ[0] == "s":
        n = int(typ[1:])
        return _c.peek_bytes(addr, n)
    raise ValueError("unknown struct type code %r" % typ)


def _struct_write(addr, typ, value):
    if typ == "B" or typ == "b":
        _c.poke_b(addr, value & 0xff)
    elif typ == "H" or typ == "h":
        _c.poke_w(addr, value & 0xffff)
    elif typ in ("L", "l", "P"):
        _c.poke_l(addr, value & 0xffffffff)
    elif typ[0] == "s":
        n = int(typ[1:])
        if isinstance(value, str):
            value = value.encode("ascii")
        if len(value) > n:
            raise ValueError("value too long for s%d slot" % n)
        # Pad with NULs to the full slot width.
        padded = bytes(value) + b"\x00" * (n - len(value))
        _c.poke_bytes(addr, padded)
    else:
        raise ValueError("type %r not writable through Struct" % typ)


class Struct:
    """Pythonic accessor for a C struct at a fixed address.

    `layout` is a dict mapping field name to `(offset, type_code)`.
    Attribute reads peek; assignments poke (where supported by the
    type).  Pass `addr=0` only as a placeholder — every read/write
    runs against the live address.

    Pre-curated layouts ship in `_amiga_structs`; re-export factories
    such as `amiga.Task(addr)` build the right `Struct` for you.
    """

    def __init__(self, addr, layout, name=None):
        object.__setattr__(self, "_addr", addr)
        object.__setattr__(self, "_layout", layout)
        object.__setattr__(self, "_name", name)

    @property
    def addr(self):
        return self._addr

    def __int__(self):
        return self._addr

    def __getattr__(self, fname):
        if fname.startswith("_"):
            raise AttributeError(fname)
        info = self._layout.get(fname)
        if info is None:
            raise AttributeError(fname)
        offset, typ = info
        return _struct_read(self._addr + offset, typ)

    def __setattr__(self, fname, value):
        if fname.startswith("_"):
            object.__setattr__(self, fname, value)
            return
        info = self._layout.get(fname)
        if info is None:
            raise AttributeError(fname)
        offset, typ = info
        _struct_write(self._addr + offset, typ, value)

    def __repr__(self):
        nm = self._name or "Struct"
        return "<%s @ 0x%x>" % (nm, self._addr)


def Node(addr):
    """Wrap a `struct Node` at `addr`."""
    return Struct(addr, _structs.NODE, name="Node")


def Task(addr):
    """Wrap a `struct Task` at `addr`."""
    return Struct(addr, _structs.TASK, name="Task")


def Library_struct(addr):
    """Wrap a `struct Library` at `addr`.

    Named with a trailing `_struct` to avoid the class-vs-struct collision
    with `amiga.Library` (which represents an *opened* library proxy).
    """
    return Struct(addr, _structs.LIBRARY, name="Library")


def DateStamp(addr):
    """Wrap a `struct DateStamp` (12-byte days/minutes/ticks triple)."""
    return Struct(addr, _structs.DATESTAMP, name="DateStamp")


def FileInfoBlock(addr):
    """Wrap a `struct FileInfoBlock` (260 bytes)."""
    return Struct(addr, _structs.FILEINFOBLOCK, name="FileInfoBlock")


def IntuiMessage(addr):
    """Wrap a `struct IntuiMessage` (52 bytes)."""
    return Struct(addr, _structs.INTUIMESSAGE, name="IntuiMessage")


# ---------- Phase 18 (inbound): RexxMessage facade ----------
#
# `_amiga.rexx_recv` returns the raw RexxMsg address (or None on
# timeout); we wrap it here so callers see a normal Python object with
# `.command` / `.reply(...)`.  Methods delegate to the C primitives.

# ARexx action-code high byte (rexx/storage.h).
RXCOMM  = 0x01000000
RXFUNC  = 0x02000000
RXCLOSE = 0x03000000
RXQUERY = 0x04000000

# Standard ARexx return-code levels.
RC_OK     = 0
RC_WARN   = 5
RC_ERROR  = 10
RC_FAIL   = 20


class RexxMessage:
    """A single inbound ARexx message, with a `.command` and a `.reply()`.

    Constructed automatically by `amiga.rexx_recv()`.  Each message
    *must* be replied to — leaving one un-replied blocks the sender
    forever.  The `serve()` helper handles this for you; a manual loop
    needs an explicit `msg.reply(...)` (or `msg.reply(rc=...)`) before
    going back to `recv()`.
    """

    __slots__ = ("_msg", "_replied")

    def __init__(self, msg_ptr):
        # msg_ptr is the raw struct RexxMsg* as a Python int.
        object.__setattr__(self, "_msg", msg_ptr)
        object.__setattr__(self, "_replied", False)

    @property
    def addr(self):
        return self._msg

    @property
    def command(self):
        """The ARG0 command string, as bytes."""
        return _c.rexx_command(self._msg)

    @property
    def action(self):
        """The 32-bit action code (`rm_Action`).  High byte is one of
        RXCOMM / RXFUNC / RXCLOSE / RXQUERY; low byte carries flags."""
        return _c.peek_l(self._msg + 28) & 0xffffffff

    def reply(self, result=None, rc=RC_OK, secondary=0):
        """Reply to the sender with `(rc, result)`.

        `result` (str / bytes) is wrapped as an ARexx argstring via
        `rexxsyslib.library` when `rc == 0`; non-zero `rc` puts
        `secondary` in `rm_Result2` as the error code slot.  Must be
        called exactly once per message.
        """
        if self._replied:
            raise RuntimeError("RexxMessage already replied")
        if isinstance(result, str):
            result = result.encode("latin-1")
        _c.rexx_reply(self._msg, rc, result, secondary)
        object.__setattr__(self, "_replied", True)

    def __del__(self):
        # An unread reply blocks the sender forever, so if the Python
        # script forgets we deliver a synthetic RC_FAIL on GC.  Better
        # than the alternative (a hung `rx`).
        if not self._replied:
            try:
                _c.rexx_reply(self._msg, RC_FAIL, None, 0)
            except Exception:
                pass

    def __repr__(self):
        replied = "replied" if self._replied else "open"
        return "<RexxMessage %s addr=0x%x>" % (replied, self._msg)


def rexx_open(stem="MICROPYTHON"):
    """Open a public ARexx port `<stem>.<N>` and return the assigned name."""
    return _c.rexx_open(stem)


def rexx_close():
    """Tear down the port and reply to any pending messages with rc=20."""
    _c.rexx_close()


def rexx_port_name():
    """Return the currently-assigned port name, or None if closed."""
    return _c.rexx_port_name()


def rexx_recv(timeout_ms=None):
    """Wait for the next incoming RexxMsg.

    Returns a `RexxMessage` instance, or `None` on timeout.  Raises
    `KeyboardInterrupt` if Ctrl+C fires during the wait.
    """
    ptr = _c.rexx_recv(timeout_ms=timeout_ms)
    if ptr is None:
        return None
    return RexxMessage(ptr)


def rexx(host, command, check=True):
    """Send `command` to another app's ARexx port `host` and block for the reply.

    `host` is the public port name (e.g. `"DOPUS.1"`, `"WORKBENCH"`).
    `command` is the ARexx command string; pass a `str` (latin-1
    encoded for you) or `bytes`.

    On a successful reply (`rc == 0`) the host's result string is
    returned as `bytes` (or empty `bytes` if the host returned `rc=0`
    with no result).

    If `rc != 0`, behaviour depends on `check`:
        * `check=True` (default) — raises `OSError` with the rc as
          errno and a short message.
        * `check=False` — returns a `(rc, result_or_None)` tuple
          instead.  `result` will be `None` since AmigaOS hosts
          conventionally put a secondary error code (not a string)
          in `rm_Result2` when `rc != 0`.

    Ctrl+C during the wait is *deferred* until after the host's reply
    arrives — abandoning the message mid-flight would have the host
    `PutMsg` into our freed reply port.  In practice ARexx hosts
    reply within milliseconds, so the user-visible delay is minimal.
    """
    if isinstance(command, str):
        command = command.encode("latin-1")
    rc, result = _c.rexx_send(host, command)
    if check and rc != 0:
        raise OSError(rc, "rexx %r at %s rc=%d" % (command, host, rc))
    if check:
        return result if result is not None else b""
    return (rc, result)


def rexx_serve(handler, timeout_ms=None):
    """Run a small dispatcher loop on the open port.

    Calls `handler(command_bytes)` for each incoming command and replies
    with the function's return value (any non-None value is converted
    via `str()`; `None` becomes a bare `rc=0` with no result string).
    A raised exception is delivered as `rc=10` and the exception's
    string form as the result.  Returns when `handler` raises
    `StopIteration` or when Ctrl+C interrupts the wait.
    """
    while True:
        try:
            msg = rexx_recv(timeout_ms=timeout_ms)
        except KeyboardInterrupt:
            return
        if msg is None:
            continue
        try:
            result = handler(msg.command)
        except StopIteration:
            msg.reply(rc=RC_OK)
            return
        except Exception as exc:
            msg.reply(str(exc), rc=RC_ERROR)
            continue
        if result is None:
            msg.reply(rc=RC_OK)
        else:
            msg.reply(str(result), rc=RC_OK)
