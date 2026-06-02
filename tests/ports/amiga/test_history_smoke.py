# REPL history surface smoke test.
#
# ports/amiga/amiga_history.c persists the REPL line-edit history to
# ENV:MicroPython.history (override via the MICROPYHISTORY env var)
# across runs. The on-disk save/load is run from main() at startup
# and shutdown -- this test only exercises the in-memory ring buffer
# exposed via _amiga.readline_history / readline_push_history.

import _amiga

# --- module surface ------------------------------------------------------

assert hasattr(_amiga, "readline_history")
assert hasattr(_amiga, "readline_push_history")

# --- shape: history is a tuple of strings -------------------------------

initial = _amiga.readline_history()
assert isinstance(initial, tuple), type(initial)
for entry in initial:
    assert isinstance(entry, str), type(entry)

# --- push order is most-recent-first -----------------------------------

# Each pushed line ends up at index 0.
_amiga.readline_push_history("first push")
_amiga.readline_push_history("second push")
hist = _amiga.readline_history()
assert hist[0] == "second push", hist
assert hist[1] == "first push", hist

# --- consecutive duplicate is suppressed ------------------------------

before = _amiga.readline_history()
_amiga.readline_push_history(before[0])  # push the current top again
after = _amiga.readline_history()
# Length should not increase (the immediate-dup guard keeps the list
# flat when the user hits Enter twice on the same line).
assert len(after) == len(before), (len(before), len(after))

# --- empty string is dropped ------------------------------------------

before = _amiga.readline_history()
_amiga.readline_push_history("")
after = _amiga.readline_history()
assert len(after) == len(before)

# --- long line survives ------------------------------------------------

long_line = "x" * 100
_amiga.readline_push_history(long_line)
assert _amiga.readline_history()[0] == long_line

# --- ring buffer caps at MICROPY_READLINE_HISTORY_SIZE (32) -----------

# Push enough unique lines to fill and overflow the ring.
RING = 32
for i in range(RING + 10):
    _amiga.readline_push_history("ring_line_%03d" % i)

hist = _amiga.readline_history()
# Cap is exactly RING entries -- the oldest get evicted.
assert len(hist) == RING, (len(hist), RING)

# Top entry is the last one pushed.
assert hist[0] == "ring_line_%03d" % (RING + 10 - 1), hist[0]

# Bottom entry is the (RING+10-RING) = 10th push, i.e. ring_line_010.
assert hist[-1] == "ring_line_%03d" % (RING + 10 - RING), hist[-1]

# Everything in between is monotonic decreasing.
for i in range(RING - 1):
    a = int(hist[i].split("_")[-1])
    b = int(hist[i + 1].split("_")[-1])
    assert a == b + 1, (a, b)

# --- type guard on push() ----------------------------------------------

try:
    _amiga.readline_push_history(42)  # not a string
    assert False, "expected TypeError"
except TypeError:
    pass

print("OK")
