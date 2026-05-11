# 68040 variant — 68040 CPU with built-in FPU.
# Typical hardware: A3640, A4000/040, Blizzard 1240, CyberStorm Mk-II.
#
# Same float-wrap story as 68020fpu: gcc emits hardware FPU instructions
# directly, so the soft-float helper wraps are unnecessary. The libm
# pow/tgamma wraps in the main Makefile still apply.
#
# Note: also requires bebbo to have a 68040 multilib (or equivalent
# libgcc/clib2 built for -m68040). Stock bebbo 'make all' includes this.

AMIGA_CPU_FLAGS = -m68040
AMIGA_FLOAT_WRAPS =
