// 68040 variant: 68040 CPU with built-in FPU.
// Typical hardware: A3640, A4000/040, Blizzard 1240, CyberStorm Mk-II.

#define MICROPY_HW_MCU_NAME                 "68040"

// 68040-class machines almost always have 4+ MB of Fast RAM; default to
// 1 MB heap. Users on memory-constrained configurations can still
// override with -X heap=... once Phase 14 lands.
#define MICROPY_HEAP_SIZE                   (1024 * 1024)
