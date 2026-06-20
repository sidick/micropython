# amiga.Library proxy smoke test.
#
# The Library proxy is the foundation of every Amiga-specific module --
# intuition, asl, icon, catalog, even the ARexx surfaces all build on
# top of _amiga.lib_open / lib_call / lib_close + the frozen
# _amiga_fd.LIBRARIES signature table. This test exercises the proxy
# itself against exec.library, which vamos emulates well enough that
# the full lookup -> register-marshal -> trampoline path runs end to
# end without needing real Amiga hardware.

import amiga
import _amiga

# --- module surface ------------------------------------------------------

assert hasattr(amiga, "Library")
assert hasattr(amiga, "library")  # convenience factory
assert hasattr(amiga, "library_from_signatures")

# --- open / call / close via the frozen .fd table ----------------------

e = amiga.Library("exec.library", 0)
assert e.base != 0, "exec.library failed to open"
assert e.name == "exec.library"

# Zero-arg function: ThisTask reads ExecBase->ThisTask, vamos emulates.
this_task = e.FindTask(0)  # FindTask(NULL) returns the current task
assert isinstance(this_task, int)
assert this_task != 0, "FindTask(0) should return non-NULL on a live task"

# One-arg function dispatched via D1: AvailMem(flags). The vamos shim
# returns a non-zero value for general / fast memory; we only check the
# trampoline marshalled the argument through (different `flags` values
# can return different numbers, and that's all we need to know).
avail_any = e.AvailMem(0)  # MEMF_ANY
avail_fast = e.AvailMem(4)  # MEMF_FAST
assert isinstance(avail_any, int)
assert isinstance(avail_fast, int)
# Either both > 0 (real machine / vamos with memory), or both 0 (some
# emulator stub) -- but never negative (the result is u_int_t cast).
assert avail_any >= 0 and avail_fast >= 0

# --- closure caching ---------------------------------------------------

# First attribute access goes through __getattr__ and stores a closure
# on the instance dict; subsequent reads must return the same object.
fn1 = e.FindTask
fn2 = e.FindTask
assert fn1 is fn2, "Library should cache the per-function closure"

# --- repr reflects open/closed state -----------------------------------

assert "exec.library" in repr(e)
assert "closed" not in repr(e)
e.close()
assert "closed" in repr(e)

# --- calling after close() raises ValueError ---------------------------

try:
    e.FindTask(0)
    assert False, "expected ValueError after close()"
except ValueError:
    pass

# Double-close is a no-op.
e.close()

# --- context-manager close ---------------------------------------------

with amiga.Library("exec.library") as e2:
    assert e2.base != 0
    base_addr = e2.base
    _ = e2.FindTask(0)
assert e2.base == 0, "context manager exit should close the library"
# Closures bound to the closed instance must now raise.
try:
    e2.FindTask(0)
    assert False, "expected ValueError after context-manager exit"
except ValueError:
    pass

# --- wrong arg count raises TypeError ---------------------------------

with amiga.library("exec.library") as e3:
    try:
        e3.FindTask()  # FindTask takes one arg
        assert False, "expected TypeError on missing arg"
    except TypeError:
        pass
    try:
        e3.FindTask(0, 0)  # extra arg
        assert False, "expected TypeError on extra arg"
    except TypeError:
        pass

# --- unknown function raises AttributeError ---------------------------

with amiga.library("exec.library") as e4:
    try:
        e4.ThisFunctionDoesNotExist
        assert False, "expected AttributeError for unknown function"
    except AttributeError as err:
        # Error message should mention the library + missing function.
        msg = str(err)
        assert "exec.library" in msg, msg
        assert "ThisFunctionDoesNotExist" in msg, msg

# Names starting with _ are reserved (Python protocol attrs) and never
# go through the .fd lookup -- otherwise an attribute miss on something
# like _private would try to OpenLibrary it.
with amiga.library("exec.library") as e5:
    try:
        e5._private_attr
        assert False, "expected AttributeError for _-prefixed name"
    except AttributeError:
        pass

# --- explicit signatures= override bypasses the frozen table ----------

# Hand-rolled FindTask signature -- proves the override path works
# without depending on _amiga_fd having seen the library.
custom = {"FindTask": (-294, "a1", "")}
with amiga.Library("exec.library", 0, signatures=custom) as cl:
    t = cl.FindTask(0)
    assert isinstance(t, int) and t != 0
    # The proxy must reject calls to functions that aren't in the
    # custom table even if they exist in the frozen one.
    try:
        cl.AvailMem(0)
        assert False, "expected AttributeError -- AvailMem not in custom sigs"
    except AttributeError:
        pass

# library_from_signatures is the same constructor under a different name.
cl2 = amiga.library_from_signatures("exec.library", 0, custom)
assert isinstance(cl2, amiga.Library)
cl2.close()

# --- int-coercion via __int__ -----------------------------------------


# Anything implementing __int__ is unwrapped to its address before
# being placed in the register dict. This is what lets TagList objects
# pass straight through; we verify the mechanism with a stand-in class.
class IntLike:
    def __init__(self, v):
        self._v = v

    def __int__(self):
        return self._v


with amiga.library("exec.library") as e6:
    # FindTask(IntLike(0)) should work the same as FindTask(0).
    t = e6.FindTask(IntLike(0))
    assert isinstance(t, int) and t != 0

# --- direct _amiga.lib_open / lib_call / lib_close round-trip --------

# The proxy is a thin wrapper -- the underlying primitives must also
# work standalone (and are used by anyone bypassing the .fd machinery).
b = _amiga.lib_open("exec.library", 0)
assert isinstance(b, int) and b != 0
# Call FindTask directly: LVO -294, takes a1.
r = _amiga.lib_call(b, -294, a1=0)
assert isinstance(r, int) and r != 0
_amiga.lib_close(b)

# --- Library.base / Library.name accessors ----------------------------

with amiga.library("exec.library") as e7:
    assert e7.name == "exec.library"
    assert isinstance(e7.base, int) and e7.base != 0
    # After close the base goes to zero.
assert e7.base == 0
assert e7.name == "exec.library"  # name persists past close

print("OK")
