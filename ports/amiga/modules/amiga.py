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

    def __init__(self, name, version=0):
        # Look up the signature table first so an unsupported library
        # raises before we burn an OpenLibrary call.
        self._name = name
        self._signatures = _amiga_fd.LIBRARIES.get(name, {})
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


def library(name, version=0):
    """Convenience factory equivalent to `Library(name, version)`."""
    return Library(name, version)


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
