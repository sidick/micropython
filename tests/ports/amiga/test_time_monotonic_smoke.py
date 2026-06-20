# Phase 39 -- time monotonic surface.
#
# Covers time.ticks_ms / ticks_us / ticks_cpu / ticks_add / ticks_diff
# and time.sleep_ms / sleep_us, all backed by timer.device ReadEClock()
# in ports/amiga/amiga_timer.c.
#
# vamos doesn't drive ReadEClock past the initial frequency probe -- the
# E-Clock counter doesn't advance during execution -- so the "did it
# really pass N ms?" assertions are gated behind a runtime detection.
# On Amiberry / real hardware the dynamic checks run.

import time

# --- module surface ------------------------------------------------------

for name in (
    "ticks_ms",
    "ticks_us",
    "ticks_cpu",
    "ticks_add",
    "ticks_diff",
    "sleep_ms",
    "sleep_us",
):
    assert hasattr(time, name), name

# --- return-type contract -----------------------------------------------

t_ms_1 = time.ticks_ms()
t_us_1 = time.ticks_us()
t_cpu_1 = time.ticks_cpu()
assert isinstance(t_ms_1, int), type(t_ms_1)
assert isinstance(t_us_1, int), type(t_us_1)
assert isinstance(t_cpu_1, int), type(t_cpu_1)
# Values are non-negative (ticks_* are unsigned-cast counters).
assert t_ms_1 >= 0
assert t_us_1 >= 0
assert t_cpu_1 >= 0

# --- ticks_diff handles arbitrary differences ---------------------------

# ticks_diff(a, b) computes a - b in the ticks_* wraparound domain. It
# must work on synthetic values without consulting the timer.
assert time.ticks_diff(100, 50) == 50
assert time.ticks_diff(50, 100) == -50
# Same input -> zero.
assert time.ticks_diff(t_ms_1, t_ms_1) == 0

# --- ticks_add wraps consistently with ticks_diff -----------------------

base = 1000
assert time.ticks_diff(time.ticks_add(base, 250), base) == 250
assert time.ticks_diff(time.ticks_add(base, -250), base) == -250

# --- sleep_ms(0) / sleep_us(0) must return promptly ---------------------

# Even on vamos these calls should return immediately; if they didn't the
# whole test would hang.
time.sleep_ms(0)
time.sleep_us(0)

# --- runtime detection: do ticks actually advance? ----------------------

# On Amiberry / real Amiga the E-Clock runs free, so ticks advance even
# without explicit sleeps. On vamos the E-Clock counter is static, so a
# tight Python loop sees no change. We use the latter to soft-skip the
# wall-time assertions instead of hard-failing them.
samples = [time.ticks_us() for _ in range(2000)]
have_eclock = samples[0] != samples[-1]

# --- dynamic checks (real timer) ----------------------------------------

if have_eclock:
    # sleep_ms(N) must sleep for at least roughly N ms. We allow a wide
    # tolerance window (50%-300% of requested) to accommodate cycle-counter
    # quirks and uneven host scheduling, but should never come back early.
    t0 = time.ticks_ms()
    time.sleep_ms(50)
    t1 = time.ticks_ms()
    elapsed_ms = time.ticks_diff(t1, t0)
    assert 25 <= elapsed_ms <= 500, "sleep_ms(50) elapsed=%d ms" % elapsed_ms

    # Same for sleep_us.
    u0 = time.ticks_us()
    time.sleep_us(5000)  # 5 ms
    u1 = time.ticks_us()
    elapsed_us = time.ticks_diff(u1, u0)
    assert 2500 <= elapsed_us <= 50000, "sleep_us(5000) elapsed=%d us" % elapsed_us

    # ticks_ms is monotonic across calls (within a single task).
    a = time.ticks_ms()
    b = time.ticks_ms()
    assert time.ticks_diff(b, a) >= 0, (a, b)

    # ticks_us has finer resolution than ticks_ms -- N back-to-back reads
    # should produce at least one distinct value.
    us_samples = [time.ticks_us() for _ in range(10)]
    assert len(set(us_samples)) > 1, us_samples

print("OK")
