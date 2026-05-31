"""AmigaOS extensions to the C-side `os` module.

The C extension registered by `extmod/modos.c` is the source of truth
for the standard surface (`getcwd`, `chdir`, `listdir`, `mkdir`,
`remove`, `rename`, `rmdir`, `stat`, `statvfs`, `getenv`, `putenv`,
`unsetenv`).  This file extends it with:

* `chmod` / `getprotect` plus the `FIBF_*` protection-bit constants
  (sourced from `_osamiga`, the port-local C module).
* `makedirs` -- recursive mkdir, AmigaOS volume-aware.
* `walk`     -- recursive directory traversal generator.
* `os.path`  -- the AmigaOS-aware `_ospath` module re-exported.

AmigaDOS protection-bit caveat:  the four RWED bits are *inverted*
relative to Unix.  A *set* bit means "denied", a *clear* bit means
"allowed."  `chmod(path, 0)` therefore grants R, W, E, and D (nothing
denied).  The APSH bits (ARCHIVE / PURE / SCRIPT / HOLD) follow the
"set means yes" convention.  This is the same encoding the AmigaShell
`Protect` command uses.

`getprotect` returns the raw `fib_Protection` ULONG; mask it with the
`FIBF_*` constants to test individual bits.
"""

# Pull the C-side `os` surface into this frozen module's namespace.
# Without this, `mkdir` / `listdir` / `stat` etc. wouldn't be in scope
# when our makedirs / walk reference them. The `uos` alias is
# MicroPython's documented hook for forcing the extensible C built-in
# when a same-named frozen `.py` exists.
from uos import *  # noqa: F401, F403

# noqa rules: F401 (unused import re-export), F403 (star import).
from _osamiga import (   # noqa: F401
    chmod,
    getprotect,
    FIBF_READ,
    FIBF_WRITE,
    FIBF_EXECUTE,
    FIBF_DELETE,
    FIBF_ARCHIVE,
    FIBF_PURE,
    FIBF_SCRIPT,
    FIBF_HOLD,
)

import _ospath as path  # noqa: F401  -- re-exposed as os.path


def makedirs(name, exist_ok=False):
    """`mkdir -p` for AmigaOS paths.

    Splits at the first `:` (volume boundary) and at each `/`
    (component boundary).  Tolerates `EEXIST` at every level when
    `exist_ok=True`, matching CPython semantics.  The volume part
    itself isn't created -- if `Work:` doesn't exist, an `OSError`
    surfaces immediately.
    """
    # Strip a trailing separator so the last component participates
    # in the loop (otherwise an empty basename would be a no-op).
    if name.endswith("/"):
        name = name[:-1]
    colon = name.find(":")
    if colon == -1:
        # Pure relative path: walk the slashes from the cwd.
        prefix = ""
        rest = name
    else:
        # Up to and including ":" is the volume reference; the part
        # after is what we need to materialise.
        prefix = name[: colon + 1]
        rest = name[colon + 1 :]
    if not rest:
        return  # just a volume reference, nothing to create
    # Build progressively-longer paths and mkdir each in turn.
    parts = rest.split("/")
    accum = prefix
    for i, part in enumerate(parts):
        if accum and not (accum.endswith(":") or accum.endswith("/")):
            accum = accum + "/" + part
        else:
            accum = accum + part
        try:
            mkdir(accum)
        except OSError as e:
            if exist_ok and e.errno in (17, 18):  # EEXIST family
                continue
            raise


def walk(top, topdown=True):
    """Recursive directory traversal generator.

    Yields `(dirpath, dirnames, filenames)` triples; same shape as
    CPython's `os.walk` modulo the lack of an `onerror` / `followlinks`
    knob (AmigaOS doesn't have symlinks).  Joins paths with the
    AmigaOS-aware `os.path.join` so a `dirpath` of `"Work:"` doesn't
    get a spurious `/` when descended.
    """
    try:
        entries = listdir(top)
    except OSError:
        return
    dirs = []
    files = []
    for name in entries:
        full = path.join(top, name)
        try:
            mode = stat(full)[0]
        except OSError:
            files.append(name)
            continue
        # Directory bit: mode & 0o40000 (the standard CPython encoding
        # MicroPython's stat() honours).
        if mode & 0x4000:
            dirs.append(name)
        else:
            files.append(name)
    if topdown:
        yield top, dirs, files
    for d in dirs:
        # Caller may have mutated `dirs` (topdown convention); honour
        # whatever's left after the yield.
        sub = path.join(top, d)
        for triple in walk(sub, topdown):
            yield triple
    if not topdown:
        yield top, dirs, files
