# Phase 39 step 1 -- wall-clock time surface.  Verifies time.time(),
# time.time_ns(), time.gmtime(), time.localtime(), and time.mktime()
# against the timer.device GetSysTime() backing.
#
# vamos doesn't emulate GetSysTime (returns "default" -- the buffer
# is left untouched), so the "is it a real recent wall-clock?"
# assertions are gated behind a runtime detection: if time.time() is
# below the 2020 floor we assume vamos and only verify the shape /
# contract surface (gmtime / mktime / time_ns return-type, epoch math)
# which doesn't depend on the system clock.

import time

# --- time.time() shape ---------------------------------------------------

t = time.time()
assert isinstance(t, float), type(t)

# Wall-clock is sane (real Amiga or Amiberry).
have_wallclock = t > 1577836800.0  # 2020-01-01

# --- time.time_ns() shape ------------------------------------------------

ns = time.time_ns()
assert isinstance(ns, int), type(ns)

# --- wall-clock sanity (skipped when GetSysTime isn't emulated) ---------

if have_wallclock:
    # Must be before 2046 (signed 32-bit Amiga tv_secs overflow from 1978).
    assert t < 2398291200.0, t
    # Monotonic-ish (system clock can't go backwards inside one task).
    t2 = time.time()
    assert t2 >= t, (t, t2)
    # ns and t agree to within ~1 second.
    assert 1577836800_000_000_000 < ns < 2398291200_000_000_000, ns
    ns_after = time.time_ns()
    assert abs(ns_after - int(t2 * 1e9)) < 2_000_000_000, (ns_after, t2)

# --- time.gmtime(0) is the Unix epoch -----------------------------------
# The port uses MICROPY_EPOCH_IS_1970, so timestamp 0 = 1970-01-01.

ep = time.gmtime(0)
assert isinstance(ep, tuple) and len(ep) == 8, ep
year, mon, mday, hour, minute, sec, wday, yday = ep
assert year == 1970, ep
assert mon == 1, ep
assert mday == 1, ep
assert hour == 0, ep
assert minute == 0, ep
assert sec == 0, ep
# 1970-01-01 was a Thursday. MicroPython's weekday is 0=Mon..6=Sun.
assert wday == 3, ep
assert yday == 1, ep

# --- time.gmtime(now) shape ---------------------------------------------

if have_wallclock:
    now = time.gmtime()
    assert isinstance(now, tuple) and len(now) == 8, now
    y, mo, d, h, mi, s, wd, yd = now
    # Same sanity window as time.time().
    assert 2020 <= y <= 2046, now
    assert 1 <= mo <= 12, now
    assert 1 <= d <= 31, now
    assert 0 <= h <= 23, now
    assert 0 <= mi <= 59, now
    assert 0 <= s <= 59, now
    assert 0 <= wd <= 6, now
    assert 1 <= yd <= 366, now

# localtime mirrors gmtime on this port (no TZ database) -- the values
# must match for the same instant.
gl = time.gmtime(1700000000)
ll = time.localtime(1700000000)
assert gl == ll, (gl, ll)

# --- time.mktime() round-trip ------------------------------------------

# 2024-01-02 03:04:05 UTC.
known_tuple = (2024, 1, 2, 3, 4, 5, 0, 0)
secs = time.mktime(known_tuple)
assert isinstance(secs, int), type(secs)
# 2024-01-02 03:04:05 UTC = 1704164645 seconds since 1970.
assert secs == 1704164645, secs

# Round-trip: mktime(gmtime(secs)) == secs.
g = time.gmtime(secs)
assert time.mktime(g) == secs, (g, secs)

# 9-tuple form is also accepted (CPython compat).
nine = (2024, 1, 2, 3, 4, 5, 0, 0, 0)
assert time.mktime(nine) == 1704164645, time.mktime(nine)

# --- time.gmtime epoch boundary -----------------------------------------
# 1978-01-01 = AmigaOS epoch = 252,460,800 seconds since 1970.
amiga_epoch = time.gmtime(252_460_800)
assert amiga_epoch[:6] == (1978, 1, 1, 0, 0, 0), amiga_epoch

print("OK")
