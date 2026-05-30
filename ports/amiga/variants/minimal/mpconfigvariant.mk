# minimal variant — stock unaccelerated A1200 (68EC020, 2 MB Chip RAM, no FPU).
# Trims the heap and strips heavy modules to leave headroom on a machine
# that has no Fast RAM at all.

AMIGA_CPU_FLAGS = -m68020 -msoft-float
AMIGA_FLOAT_WRAPS = -Wl,--wrap=__eqdf2,--wrap=__nedf2,--wrap=__ltdf2,--wrap=__ledf2,--wrap=__gtdf2,--wrap=__gedf2

# No bsdsocket → no TLS. AmiSSL is also ~5 KB of stubs we don't want
# in the tight build.
MICROPY_PY_AMIGA_SSL = 0
