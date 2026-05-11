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
            kw = {regs[i]: args[i] for i in range(nargs)}
            return _c.lib_call(lib._base, lvo, **kw)

        # MicroPython closures don't support `__name__` assignment, so
        # the wrapper stays anonymous.  Cache it on the instance via
        # setattr so subsequent attribute reads bypass __getattr__.
        setattr(self, fnname, call)
        return call


def library(name, version=0):
    """Convenience factory equivalent to `Library(name, version)`."""
    return Library(name, version)
