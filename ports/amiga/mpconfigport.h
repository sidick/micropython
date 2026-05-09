#include <stdint.h>

// Use setjmp-based NLR — no 68k native NLR implementation exists yet.
// (MicroPython would fall back to this automatically for an unknown arch,
// but being explicit avoids any future mis-detection.)
#define MICROPY_NLR_SETJMP                  (1)

// Start with core features; expand as the port matures.
#define MICROPY_CONFIG_ROM_LEVEL            MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES

// Heap supplied as a static buffer in main.c (no NDK AllocVec yet).
// Increase once larger Fast RAM configurations are confirmed working.
#define MICROPY_HEAP_SIZE                   (256 * 1024)

#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT      (1)

#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT      (16)

// Error reporting: normal verbosity to aid early debugging.
#define MICROPY_ERROR_REPORTING             MICROPY_ERROR_REPORTING_NORMAL

// sys module — enable basics, skip things that need POSIX.
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_SYS_PATH                 (1)
#define MICROPY_PY_SYS_ARGV                 (0)
#define MICROPY_PY_SYS_MODULES              (1)

// No threads — AmigaOS uses cooperative multitasking.
#define MICROPY_PY_THREAD                   (0)

// No asyncio until threads/event loop are sorted.
#define MICROPY_PY_ASYNCIO                  (0)

// No float for now; enable MICROPY_FLOAT_IMPL_DOUBLE here once confirmed
// that soft-float performance is acceptable.
// #define MICROPY_FLOAT_IMPL               MICROPY_FLOAT_IMPL_DOUBLE

// Platform string visible as sys.platform
#define MICROPY_PY_SYS_PLATFORM             "amiga"

// Board / MCU identity strings
#define MICROPY_HW_BOARD_NAME               "Amiga"
#define MICROPY_HW_MCU_NAME                 "68020"

// Type definitions for a 32-bit big-endian machine.
// (Endianness itself is auto-detected from GCC's __BYTE_ORDER__.)
typedef int             mp_int_t;
typedef unsigned int    mp_uint_t;
typedef long            mp_off_t;

// alloca is available from GCC built-ins / newlib
#include <alloca.h>

#define MP_STATE_PORT MP_STATE_VM
