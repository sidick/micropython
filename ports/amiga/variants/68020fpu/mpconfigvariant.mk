# 68020fpu variant — 68020 (or 68030) with 68881/68882 FPU coprocessor.
# Typical hardware: A2620/A2630 accelerator, A3000, accelerated A1200/A2000.
#
# AMIGA_FLOAT_WRAPS is intentionally empty: with -m68881 the compiler emits
# FPU compare and conversion instructions directly, so the libgcc
# soft-float helpers (__eqdf2 etc.) that floatconv.c wraps are never called.
# The libm pow/tgamma wraps in the main Makefile still apply — those are
# math-library bugs unrelated to FPU codegen.
#
# Note: requires the bebbo toolchain to have FPU-multilib variants of
# libgcc/clib2 built. If linking fails with "cannot find -lgcc" or similar,
# rebuild bebbo with the FPU multilibs enabled (see bebbo's amiga-gcc docs).

AMIGA_CPU_FLAGS = -m68020 -m68881
AMIGA_FLOAT_WRAPS =
