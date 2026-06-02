# _amiga.execute surface smoke test.
#
# `_amiga.execute(command)` wraps dos.library's SystemTagList(). Under
# vamos there's no real Shell to spawn binaries into, so the actual
# binary launch fails with "failed loading binary" (return code -1
# for non-empty commands; 10 / ERROR for empty input). This test pins
# the surface and the contract that the return value is always an
# AmigaDOS-style integer return code -- the live "actually ran the
# command" path lives on Amiberry / real hardware.

import _amiga

# --- module surface ------------------------------------------------------

assert hasattr(_amiga, "execute")

# --- return-type contract -----------------------------------------------

rc = _amiga.execute("")
assert isinstance(rc, int), type(rc)

# Bogus command name. On vamos the binary load fails and returns -1.
# On a live Amiga with no such command in the search path, AmigaDOS
# returns 10 (ERROR) instead. Accept either.
rc = _amiga.execute("definitely_no_such_command_amiga_phase39_xyz")
assert isinstance(rc, int), type(rc)
assert rc != 0, "bogus command must not silently report success"

# --- argument typing ----------------------------------------------------

# A non-string argument should raise.
try:
    _amiga.execute(42)
    assert False, "expected TypeError on non-string command"
except TypeError:
    pass

print("OK")
