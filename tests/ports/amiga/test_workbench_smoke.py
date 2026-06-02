# _amiga Workbench-launch surface smoke test.
#
# When AmigaOS starts micropython from the Workbench (by double-clicking
# its icon or dropping a script onto it), the bebbo crt0 fills
# `_WBenchMsg` with the WBStartup pointer and main.c stashes it for
# later. The three accessors here surface that state to Python:
#
#   _amiga.launched_from_workbench() -> bool
#   _amiga.wb_selected_files()       -> tuple of full path strings
#   _amiga.tooltype(name)            -> str or None
#
# vamos always invokes us via the Shell, so launched_from_workbench()
# is False and the other two return empty / None. The live "actually
# launched from Workbench with selected icons" path is exercised on
# Amiberry; here we just pin the contract.

import _amiga

# --- module surface ------------------------------------------------------

assert hasattr(_amiga, "launched_from_workbench")
assert hasattr(_amiga, "wb_selected_files")
assert hasattr(_amiga, "tooltype")

# --- launched_from_workbench: bool -------------------------------------

flag = _amiga.launched_from_workbench()
assert isinstance(flag, bool), type(flag)
# Under vamos it's always False (no WBenchMsg). On a real Workbench
# launch it would be True. Both are acceptable -- the contract is just
# "is a real bool".

# --- wb_selected_files: tuple of strings -------------------------------

files = _amiga.wb_selected_files()
assert isinstance(files, (list, tuple)), type(files)
for f in files:
    assert isinstance(f, str), type(f)
# Shell launch -> no selected files.
if not flag:
    assert len(files) == 0, files

# --- tooltype: name -> str | None --------------------------------------

# Asking for a tooltype that can't exist on a Shell launch must return
# None rather than blowing up.
assert _amiga.tooltype("DEFINITELY_NOT_A_TOOLTYPE_xyz") is None

# A non-string name should raise.
try:
    _amiga.tooltype(42)
    assert False, "expected TypeError"
except TypeError:
    pass

print("OK")
