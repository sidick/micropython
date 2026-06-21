# .mpy bytecode loading smoke test.
#
# The port enables MICROPY_PERSISTENT_CODE_LOAD so `import` searches for a
# .mpy alongside the .py (the reader is already linked in for frozen
# modules). This pins that the import machinery actually reaches the .mpy
# loader -- a garbage .mpy must raise ValueError ("invalid .mpy"), not
# ImportError (which is what you get when .mpy isn't searched at all).
# Native code inside a .mpy stays unsupported (LOAD_NATIVE=0); only
# bytecode .mpy load is claimed here.

import os, sys

_NAME = "__mpy_load_smoke"
_FILE = _NAME + ".mpy"


def _cleanup():
    try:
        os.remove(_FILE)
    except OSError:
        pass


# A .mpy whose magic byte isn't 'M' -- the loader must reject it.
with open(_FILE, "wb") as f:
    f.write(b"not a valid mpy file")

# Make the current directory importable.
if "" not in sys.path:
    sys.path.insert(0, "")

try:
    try:
        __import__(_NAME)
        raise AssertionError("garbage .mpy should not import")
    except ValueError:
        # Reached the .mpy loader and it rejected the bad magic: the
        # MICROPY_PERSISTENT_CODE_LOAD search path is live.
        pass
    except ImportError:
        raise AssertionError(".mpy not searched -- PERSISTENT_CODE_LOAD off?")
finally:
    _cleanup()

print("OK")
