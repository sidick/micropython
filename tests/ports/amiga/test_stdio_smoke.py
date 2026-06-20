# sys.stdin / stdout / stderr surface smoke test.
#
# ports/amiga/sysstdio.c provides the three default stdio streams the
# embedded REPL uses. They wrap the mphal stdio HAL (write -> AmigaDOS
# Output(), read -> Input()) and present a TextIOWrapper-style
# interface to Python code.

import sys
import io

# --- module surface ------------------------------------------------------

for name in ("stdin", "stdout", "stderr"):
    obj = getattr(sys, name)
    assert obj is not None, name

# fileno() returns POSIX-style fd numbers.
assert sys.stdin.fileno() == 0
assert sys.stdout.fileno() == 1
assert sys.stderr.fileno() == 2

# --- write side ---------------------------------------------------------

# sys.stdout / sys.stderr accept write() of any byte/str payload.
# Returning the byte count is the standard Python contract.
n = sys.stdout.write("stdout write OK\n")
assert isinstance(n, int) and n > 0, n

n = sys.stderr.write("stderr write OK\n")
assert isinstance(n, int) and n > 0, n

# Empty write is a no-op that returns 0.
assert sys.stdout.write("") == 0

# --- direction guard: read on a write-only stream raises EPERM ---------

for stream, name in [(sys.stdout, "stdout"), (sys.stderr, "stderr")]:
    try:
        stream.read(1)
        assert False, "expected EPERM on " + name + ".read()"
    except OSError as e:
        # Errno 1 = EPERM in the port's modtable.
        assert e.errno == 1, (name, e)

# --- direction guard: write on a read-only stream raises EPERM ---------

try:
    sys.stdin.write("x")
    assert False, "expected EPERM on stdin.write()"
except OSError as e:
    assert e.errno == 1, e

# --- stream-protocol methods all present -------------------------------

# Each stream is a TextIOWrapper -- it must expose the regular stream
# methods even if some of them error out (which is what the direction
# guards above confirm).
for stream in (sys.stdin, sys.stdout, sys.stderr):
    for name in ("read", "readinto", "readline", "readlines", "write", "fileno", "close"):
        assert hasattr(stream, name), name

# --- sys module is read-only -------------------------------------------

# MicroPython's sys module doesn't expose __dict__ for writing, so
# `sys.stdout = ...` raises AttributeError. Document the contract --
# code that wants to capture output has to use a wrapper class that
# overrides write at the C level, not re-bind sys.stdout in Python.
buf = io.StringIO()
try:
    sys.stdout = buf
    assert False, "expected AttributeError"
except AttributeError:
    pass

print("OK")
