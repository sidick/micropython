#include <stdint.h>

// Use setjmp-based NLR — no 68k native NLR implementation exists yet.
// (MicroPython would fall back to this automatically for an unknown arch,
// but being explicit avoids any future mis-detection.)
#define MICROPY_NLR_SETJMP                  (1)

#define MICROPY_CONFIG_ROM_LEVEL            MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES

// Heap supplied as a static buffer in main.c (no NDK AllocVec yet).
// Increase once larger Fast RAM configurations are confirmed working.
#define MICROPY_HEAP_SIZE                   (256 * 1024)

#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)
#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT      (1)

#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT      (16)

// Error reporting: normal verbosity to aid early debugging.
#define MICROPY_ERROR_REPORTING             MICROPY_ERROR_REPORTING_NORMAL

// 68k has no hardware FPU; soft-float via -msoft-float in CFLAGS.
#define MICROPY_FLOAT_IMPL                  MICROPY_FLOAT_IMPL_DOUBLE

// sys module
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_SYS_PATH                 (1)
#define MICROPY_PY_SYS_ARGV                 (1)

// POSIX-style exit codes from pyexec_file/pyexec_vstr: success returns 0,
// uncaught exception returns 1, sys.exit(N) returns N. Without this, the
// embedded REPL convention applies and pyexec returns 1 on normal success.
#define MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)
#define MICROPY_PY_SYS_MODULES              (1)
// sys.stdin/stdout/stderr stream objects backed by the mphal stdio HAL.
// Implementation lives in ports/amiga/sysstdio.c.
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (0)

// No threads — AmigaOS uses cooperative multitasking.
#define MICROPY_PY_THREAD                   (0)

// No asyncio until threads/event loop are sorted.
#define MICROPY_PY_ASYNCIO                  (0)

// Scheduler is used by asyncio and ussl callbacks; disable for now.
#define MICROPY_ENABLE_SCHEDULER            (0)

// Native code emitter for Motorola 68020
#define MICROPY_EMIT_68K                    (1)
// Loading pre-compiled native .mpy files is not yet supported on this port.
#define MICROPY_PERSISTENT_CODE_LOAD_NATIVE (0)

// Poll for Ctrl+C (SIGBREAKF_CTRL_C) every 1024 bytecodes.
// amiga_check_ctrl_c() is defined in mphalport.c.
#define MICROPY_VM_HOOK_COUNT  (1024)
#define MICROPY_VM_HOOK_INIT   static uint amiga_vm_hook_counter = MICROPY_VM_HOOK_COUNT;
#define MICROPY_VM_HOOK_LOOP \
    if (--amiga_vm_hook_counter == 0) { \
        extern void amiga_check_ctrl_c(void); \
        amiga_vm_hook_counter = MICROPY_VM_HOOK_COUNT; \
        amiga_check_ctrl_c(); \
    }

// Amiga-specific module
#define MICROPY_PY_AMIGA                    (1)

// bsdsocket.library networking (optional — silently absent if lib not found)
#define MICROPY_PY_AMIGA_SOCKET             (1)

// Platform string visible as sys.platform
#define MICROPY_PY_SYS_PLATFORM             "amiga"

// Board / MCU identity strings
#define MICROPY_HW_BOARD_NAME               "Amiga"
#define MICROPY_HW_MCU_NAME                 "68020"

// 68k bebbo gcc uses 16-bit alignment for ints inside structs by default,
// which leaves mp_obj_t pointers (e.g. inside mp_state_ctx) only 2-byte
// aligned. MicroPython's tagged-pointer encoding requires 4-byte alignment
// (bits 0-2 must be zero on a heap object pointer). Force 4-byte alignment
// on the object-type field that begins every mp_obj_base_t.
#define MICROPY_OBJ_BASE_ALIGNMENT  __attribute__((aligned(4)))

// Type definitions for a 32-bit big-endian machine.
// (Endianness itself is auto-detected from GCC's __BYTE_ORDER__.)
typedef int             mp_int_t;
typedef unsigned int    mp_uint_t;
typedef long            mp_off_t;

// alloca is available from GCC built-ins / newlib
#include <alloca.h>

#define MP_STATE_PORT MP_STATE_VM
