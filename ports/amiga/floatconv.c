// Override bebbo gcc 6.5's broken integer-to-double soft-float
// conversion routines from libgcc.
//
// Confirmed broken on bebbo 6.5b 68k -msoft-float:
//   (double)(unsigned int)0xFFFFFFFF       → 2281701375  (expected 4294967295)
//   (double)(unsigned int)0x80000000       → 134217728   (expected 2147483648)
//   (double)(uint64_t)0x8AC7230489E7FFFF   → 1.353e+18   (expected 9.999e+18)
//   (double)(int64_t)1000000000000         → 997986734080 (expected 1e12)
//
// The 32-bit signed conversion (__floatsidf), and double arithmetic, are
// fine -- so we build the wider conversions from those primitives.
//
// Affected MicroPython paths (anything that converts a >31-bit int to
// double):
//   - py/parsenum.c: float("9"*51 + "e-39") → wildly wrong float
//   - py/objint*.c:  hash, repr, comparison of large ints
//   - extmod/modjson, modstruct: 'q'/'Q' codes via mp_obj_new_int_from_*

#include <stdint.h>

double __floatunsidf(unsigned int x) {
    if (x <= 0x7FFFFFFFu) {
        return (double)(int)x;
    }
    // High bit set: shift right by 1, convert as signed, then double it
    // and add back the low bit.
    return (double)(int)(x >> 1) * 2.0 + (double)(int)(x & 1u);
}

// clib2's __fixdfsi calls into MathIeeeDoubBasBase. Under vamos, that
// library's IEEEDPFix raises a Python ValueError for NaN input rather
// than returning a sentinel, so the entire emulator process aborts.
// Real AmigaOS hardware would return 0x80000000 ("indefinite") for NaN
// in the same routine. Replace the conversion with a software version
// that returns INT_MAX/INT_MIN for inf/overflow and 0 for NaN -- gcc
// 6.5 emits __fixdfsi from the bit-level mp_float_hash() body in
// py/objfloat.c after optimisation, so this also stops hash(nan) from
// crashing.
int __fixdfsi(double x) {
    union { double d; uint64_t i; } u = {x};
    int sign = (int)(u.i >> 63);
    int raw_exp = (int)((u.i >> 52) & 0x7FF);
    int unbiased = raw_exp - 1023;

    if (raw_exp == 0x7FF) {
        // Inf or NaN. NaN: return 0; Inf: saturate.
        if (u.i & 0xFFFFFFFFFFFFFULL) return 0;
        return sign ? (int)0x80000000 : 0x7FFFFFFF;
    }
    if (unbiased < 0) {
        return 0;
    }
    if (unbiased >= 31) {
        return sign ? (int)0x80000000 : 0x7FFFFFFF;
    }
    uint64_t mant = (u.i & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
    int result;
    if (unbiased >= 52) {
        result = (int)(mant << (unbiased - 52));
    } else {
        result = (int)(mant >> (52 - unbiased));
    }
    return sign ? -result : result;
}

// Same fix for unsigned double-to-int. (clib2 puts each in its own
// object so we can simply redefine.) NaN returns 0; values out of
// uint32 range saturate to UINT_MAX.
unsigned int __fixunsdfsi(double x) {
    union { double d; uint64_t i; } u = {x};
    int sign = (int)(u.i >> 63);
    int raw_exp = (int)((u.i >> 52) & 0x7FF);
    int unbiased = raw_exp - 1023;

    if (sign) return 0;
    if (raw_exp == 0x7FF) {
        return (u.i & 0xFFFFFFFFFFFFFULL) ? 0u : 0xFFFFFFFFu;
    }
    if (unbiased < 0) return 0;
    if (unbiased >= 32) return 0xFFFFFFFFu;
    uint64_t mant = (u.i & 0xFFFFFFFFFFFFFULL) | (1ULL << 52);
    if (unbiased >= 52) {
        return (unsigned int)(mant << (unbiased - 52));
    }
    return (unsigned int)(mant >> (52 - unbiased));
}

double __floatundidf(unsigned long long x) {
    unsigned int hi = (unsigned int)(x >> 32);
    unsigned int lo = (unsigned int)x;
    return __floatunsidf(hi) * 4294967296.0 + __floatunsidf(lo);
}

double __floatdidf(long long x) {
    if (x >= 0) {
        return __floatundidf((unsigned long long)x);
    }
    // -LLONG_MIN doesn't fit in long long, so handle that edge case via
    // the bit pattern: -2^63 == (uint64_t)0x8000000000000000.
    return -__floatundidf((unsigned long long) - x);
}

// Float (single-precision) variants. MicroPython on this port uses
// double, but extmod/extmod.mk pulls in code that may reference these.

float __floatunsisf(unsigned int x) {
    return (float)__floatunsidf(x);
}

float __floatundisf(unsigned long long x) {
    return (float)__floatundidf(x);
}

float __floatdisf(long long x) {
    return (float)__floatdidf(x);
}

// ---------------------------------------------------------------------------
// Override clib2's broken double-comparison helpers.
//
// Confirmed broken on bebbo 6.5b 68k -msoft-float (clib2):
//   nan == nan  → 1   (expected 0; IEEE 754 unordered)
//   nan != nan  → 0   (expected 1)
//   nan == inf  → 1   (expected 0)
//   nan <= 1    → 1   (expected 0)
//   nan >= 1    → 1   (expected 0)
// (`<` and `>` happen to come out right; we override them anyway for
// consistency rather than mixing our isnan-aware handling with clib2's.)
//
// The libgcc-style contract for these helpers is:
//   __eqdf2 / __nedf2: 0 if equal, non-zero otherwise. NaN unordered → non-zero.
//   __ltdf2: <0 if a<b, 0 if a==b, >0 if a>b. NaN → return 1 so `<0` is false.
//   __ledf2: <=0 if a<=b, >0 if a>b.            NaN → return 1.
//   __gtdf2: >0 if a>b, 0 if a==b, <0 if a<b.   NaN → return -1.
//   __gedf2: >=0 if a>=b, <0 if a<b.            NaN → return -1.
//
// MicroPython exercises every one of these (e.g. `nan == nan` -> False is
// load-bearing for math.isclose, set/dict invariants on NaN keys, etc.)
// so all four broken cases need to be replaced.

static inline int amiga_dbl_isnan(double x) {
    union { double d; uint64_t i; } u = {x};
    return (u.i & 0x7FFFFFFFFFFFFFFFULL) > 0x7FF0000000000000ULL;
}

// Return -1 if a<b, 0 if a==b, 1 if a>b. Caller must have already ruled
// out NaN. Doubles are sign-magnitude in their bit representation, so the
// comparison can be done on the bits directly with a sign flip for negatives.
static int amiga_dbl_cmp(double a, double b) {
    union { double d; uint64_t i; } ua = {a}, ub = {b};
    // +0 and -0 must compare equal.
    if (((ua.i | ub.i) << 1) == 0) {
        return 0;
    }
    int sa = (int)(ua.i >> 63);
    int sb = (int)(ub.i >> 63);
    if (sa != sb) {
        return sa ? -1 : 1;
    }
    if (ua.i == ub.i) {
        return 0;
    }
    int positive_cmp = (ua.i < ub.i) ? -1 : 1;
    return sa ? -positive_cmp : positive_cmp;
}

// Use --wrap rather than direct overrides because clib2 packs every
// comparison helper plus __muldf3, __floatsidf, ... into a single
// __eqdf2.o, so simply defining __eqdf2 here would draw the whole
// object in and trigger multi-def errors on the rest. The Makefile
// passes -Wl,--wrap=__eqdf2,--wrap=__nedf2,... so calls to those
// names are redirected to __wrap___<name> below at link time.

int __wrap___eqdf2(double a, double b) {
    if (amiga_dbl_isnan(a) || amiga_dbl_isnan(b)) return 1;
    return amiga_dbl_cmp(a, b);
}

int __wrap___nedf2(double a, double b) {
    return __wrap___eqdf2(a, b);
}

int __wrap___ltdf2(double a, double b) {
    if (amiga_dbl_isnan(a) || amiga_dbl_isnan(b)) return 1;
    return amiga_dbl_cmp(a, b);
}

int __wrap___ledf2(double a, double b) {
    if (amiga_dbl_isnan(a) || amiga_dbl_isnan(b)) return 1;
    return amiga_dbl_cmp(a, b);
}

int __wrap___gtdf2(double a, double b) {
    if (amiga_dbl_isnan(a) || amiga_dbl_isnan(b)) return -1;
    return amiga_dbl_cmp(a, b);
}

int __wrap___gedf2(double a, double b) {
    if (amiga_dbl_isnan(a) || amiga_dbl_isnan(b)) return -1;
    return amiga_dbl_cmp(a, b);
}

// pow(-1, NaN) returns 1.0 in libnix's pow() but NaN in CPython.
// CPython only treats pow(1, NaN) -> 1 as a special case; pow(-1, NaN)
// must propagate the NaN. The wrap covers that case and defers to the
// libm pow() for everything else.
extern double __real_pow(double x, double y);
double __wrap_pow(double x, double y) {
    if (amiga_dbl_isnan(y) && x != 1.0) {
        return y;
    }
    return __real_pow(x, y);
}

// tgamma(-inf) returns +inf in libnix; CPython raises ValueError (the
// math module catches a NaN return as a domain error, but libnix
// returns +inf which the wrapper treats as a valid result). Force a
// NaN return for that input so py/modmath.c picks it up.
static inline int amiga_dbl_isneginf(double x) {
    union { double d; uint64_t i; } u = {x};
    return u.i == 0xFFF0000000000000ULL;
}
extern double __real_tgamma(double x);
double __wrap_tgamma(double x) {
    if (amiga_dbl_isneginf(x)) {
        union { double d; uint64_t i; } u;
        u.i = 0x7FF8000000000000ULL;  // quiet NaN
        return u.d;
    }
    return __real_tgamma(x);
}
