# ports/amiga/floatconv.c smoke test.
#
# Pins the libgcc int->double / libm wraps that the Amiga port replaces
# to work around bebbo 6.5 soft-float and libnix math bugs. A regression
# in any of these would surface as a silent numerical drift -- the
# point of the test is to make those drifts loud.
#
# Documented broken values (without the wraps):
#   (double)(unsigned int)0xFFFFFFFF       → 2281701375  (truth: 4294967295)
#   (double)(unsigned int)0x80000000       → 134217728   (truth: 2147483648)
#   (double)(int64_t)1000000000000         → 997986734080 (truth: 1e12)
#   pow(-1, NaN)                           → 1.0         (truth: NaN)
#   tgamma(-inf)                           → +inf        (truth: domain error)
#
# Variant scope: standard only. vamos's 68040 CPU emulation lacks
# faithful 68881 FPU support (project memory project_amiga_vamos_env),
# so running this test against the 68020fpu / 68040 binaries on vamos
# yields spurious failures. The soft-float-only standard variant
# exercises the entire wrap surface that floatconv.c implements;
# FPU-variant correctness lives on Amiberry / real hardware.

import math

# --- __floatunsidf: unsigned 32-bit -> double --------------------------

# Top-bit-set values are the ones bebbo's libgcc botched.
assert float(0xFFFFFFFF) == 4294967295.0
assert float(0x80000000) == 2147483648.0
assert float(0xC0000000) == 3221225472.0
# Sanity floor.
assert float(0) == 0.0
assert float(1) == 1.0

# --- __floatdidf / __floatundidf: 64-bit int -> double ----------------

# Plain decimal that bebbo got wrong (997986734080 vs the truth of 1e12).
assert float(1000000000000) == 1e12
assert float(-1000000000000) == -1e12

# Power-of-two boundary at the 32-bit -> 64-bit transition.
assert float(1 << 32) == 4294967296.0
assert float(1 << 40) == 1099511627776.0
# Largest int64 round-trips to a finite, sign-correct double.
# (Can't use 2**63-1 as the rhs because MICROPY_LONGINT_IMPL=LONGLONG
# raises OverflowError when 2**63 spills past int64 max during the
# intermediate computation.)
big = float(0x7FFFFFFFFFFFFFFF)
assert big > 0
assert math.isfinite(big)
assert big > 9.2e18 and big < 9.3e18, big

# --- __wrap_pow: NaN propagation --------------------------------------

# libnix's pow(-1, NaN) returns 1.0; CPython only short-circuits
# pow(1, NaN) -> 1.
nan = float("nan")
assert math.isnan(math.pow(-1.0, nan))
assert math.isnan(math.pow(2.0, nan))
assert math.pow(1.0, nan) == 1.0  # CPython-mandated exception
# pow(NaN, 0) -> 1 (also a CPython-mandated exception).
assert math.pow(nan, 0.0) == 1.0

# Normal pow() still works.
assert math.pow(2.0, 10.0) == 1024.0
assert math.pow(0.5, 2.0) == 0.25

# --- __wrap_tgamma: gamma(-inf) raises ---------------------------------

# libnix returns +inf for gamma(-inf); CPython raises math domain error.
neg_inf = -float("inf")
try:
    math.gamma(neg_inf)
    assert False, "expected math domain error for gamma(-inf)"
except (ValueError, OverflowError):
    pass

# Normal gamma values still compute. (libnix's gamma has roughly
# double-precision precision drift on large args, so we compare with
# a relative-error tolerance rather than ==.)
assert math.gamma(1.0) == 1.0
assert abs(math.gamma(5.0) - 24.0) < 1e-12  # 4!
assert abs(math.gamma(0.5) - math.sqrt(math.pi)) < 1e-9

# --- __fixdfsi: double -> int saturation on NaN/inf -------------------

# hash() on a float goes through bit-level extraction (objfloat.c) that
# gcc 6.5 lowers to __fixdfsi after optimisation. Without the wrap, the
# vamos MathIeeeDoubBas.IEEEDPFix raises a Python ValueError and the
# emulator aborts. Just calling hash(nan) must succeed.
assert isinstance(hash(nan), int)
assert isinstance(hash(float("inf")), int)
assert isinstance(hash(neg_inf), int)

# --- __wrap___*df2: double comparison wraps (standard variant only) ---

# On FPU variants the compiler emits hardware compares directly; on
# soft-float (standard) bebbo's __eqdf2/__nedf2/__ltdf2/__ledf2/
# __gtdf2/__gedf2 are wrong for some operands and get --wrap'd onto
# amiga_dbl_cmp. The observable contract is the same on both: IEEE 754
# NaN-aware comparison.

# NaN compared to anything (including itself) -> all relations false
# except !=.
assert (nan == nan) is False
assert (nan != nan) is True
assert (nan < 1.0) is False
assert (nan <= 1.0) is False
assert (nan > 1.0) is False
assert (nan >= 1.0) is False
assert (1.0 < nan) is False
assert (1.0 > nan) is False

# Inf comparisons behave normally.
assert float("inf") > 1e308
assert neg_inf < -1e308
assert float("inf") == float("inf")
assert neg_inf == neg_inf

# Regular cases sanity-check.
assert 1.0 < 2.0
assert 2.0 > 1.0
assert -1.5 <= -1.5
assert 3.14 != 2.72
assert 0.0 == -0.0  # IEEE 754: positive and negative zero compare equal

print("OK")
