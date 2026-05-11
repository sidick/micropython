# Standard variant — 68020, soft-float, all features.

AMIGA_CPU_FLAGS = -m68020 -msoft-float
AMIGA_FLOAT_WRAPS = -Wl,--wrap=__eqdf2,--wrap=__nedf2,--wrap=__ltdf2,--wrap=__ledf2,--wrap=__gtdf2,--wrap=__gedf2
