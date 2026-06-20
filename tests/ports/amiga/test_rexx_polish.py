# Phase 32 wiring smoke -- arg-shape + module-alias chain for the
# ARexx polish additions (rexx_exists / rexx_list / RexxClient).
# Runs under vamos with no real rexxsyslib.library, so we can't open
# a RexxClient or do an actual send; we just check the surface area
# the C module exposes and the Python facade wraps.

import amiga
import _amiga

# --- Step 1: one-shot helpers ---

# rexx_exists: pure FindPort, no rexxsyslib required. Vamos's port
# list is empty so any name returns False.
assert _amiga.rexx_exists("DOES_NOT_EXIST") is False

# rexx_list: walks SysBase->PortList. Vamos exposes no public ports
# so we get [].
lst = _amiga.rexx_list()
assert isinstance(lst, list)
for entry in lst:
    assert isinstance(entry, str)

# --- Step 2: persistent RexxClient ---

# Facade class is present.
assert hasattr(amiga, "RexxClient")
RC = amiga.RexxClient

# Expected method / property surface.
for attr in ("send", "close", "host", "__enter__", "__exit__", "__del__"):
    assert hasattr(RC, attr), attr

# Underlying C primitives are exposed (Python facade uses them).
for fn in ("rexx_client_open", "rexx_client_close", "rexx_client_send"):
    assert callable(getattr(_amiga, fn)), fn

# Pre-existing Phase 18 entries still wired (regression catch for the
# helper refactor in Step 2).
for fn in (
    "rexx_open",
    "rexx_close",
    "rexx_send",
    "rexx_recv",
    "rexx_reply",
    "rexx_command",
    "rexx_port_name",
):
    assert callable(getattr(_amiga, fn)), fn

print("OK")
