#include <stdint.h>
#include <stddef.h>

// Variant overrides are pulled in first so the #ifndef-guarded defaults
// below can be replaced per variant. Each variant ships a mpconfigvariant.h
// under ports/amiga/variants/<variant>/.
#include "mpconfigvariant.h"

// Use setjmp-based NLR — no 68k native NLR implementation exists yet.
// (MicroPython would fall back to this automatically for an unknown arch,
// but being explicit avoids any future mis-detection.)
#define MICROPY_NLR_SETJMP                  (1)

#ifndef MICROPY_CONFIG_ROM_LEVEL
#define MICROPY_CONFIG_ROM_LEVEL            MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES
#endif

// Default initial heap size; variants tune this for their target hardware
// (a500 trims it; FPU variants assume an accelerator card with more RAM).
// At runtime, -X heap=<N>[K|M] or the MICROPYHEAP env var override this
// for the initial AllocVec. The heap grows on demand beyond the initial
// size via MICROPY_GC_SPLIT_HEAP_AUTO (see below).
#ifndef MICROPY_HEAP_SIZE
#define MICROPY_HEAP_SIZE                   (256 * 1024)
#endif

// Allow the GC heap to span multiple non-contiguous AllocVec chunks,
// growing automatically when an allocation fails. amiga_alloc_heap()
// / amiga_free_heap() in main.c wrap AllocVec/FreeVec and track grown
// chunks so they can be released at exit (exec.library does NOT
// auto-reclaim AllocVec'd memory when a task ends).
#define MICROPY_GC_SPLIT_HEAP               (1)
#define MICROPY_GC_SPLIT_HEAP_AUTO          (1)
void *amiga_alloc_heap(size_t n);
void amiga_free_heap(void *p);
#define MP_PLAT_ALLOC_HEAP(n)               amiga_alloc_heap(n)
#define MP_PLAT_FREE_HEAP(p)                amiga_free_heap(p)

#define MICROPY_ENABLE_GC                   (1)
#define MICROPY_ENABLE_FINALISER            (1)
#define MICROPY_ENABLE_COMPILER             (1)
#define MICROPY_HELPER_REPL                 (1)
#define MICROPY_REPL_AUTO_INDENT            (1)
#define MICROPY_ENABLE_EXTERNAL_IMPORT      (1)

// VFS layer. Drives os.chdir / os.getcwd / os.listdir / os.stat / etc.
// via a port-local VfsAmiga in vfs_amiga.c that wraps dos.library
// (Lock/Examine/CurrentDir/...). MICROPY_READER_VFS routes
// mp_lexer_new_from_file through the VFS so the import machinery uses
// the same file-open path as the rest of the binary, replacing the
// direct dos.library reads that used to live in amigafile.c.
#define MICROPY_VFS                         (1)
#define MICROPY_READER_VFS                  (1)

#define MICROPY_ALLOC_PATH_MAX              (256)
#define MICROPY_ALLOC_PARSE_CHUNK_INIT      (16)

// Error reporting: normal verbosity to aid early debugging.
#define MICROPY_ERROR_REPORTING             MICROPY_ERROR_REPORTING_NORMAL

// 68k has no hardware FPU; soft-float via -msoft-float in CFLAGS.
#define MICROPY_FLOAT_IMPL                  MICROPY_FLOAT_IMPL_DOUBLE

// Native 64-bit ints for values beyond 31-bit smallint range. Needed
// for the 'q'/'Q' struct codes and any int literal > 30 bits in the
// source (e.g. 0xFFFF_FFFF in feature-detect tests).
#define MICROPY_LONGINT_IMPL                MICROPY_LONGINT_IMPL_LONGLONG

// sys module
#define MICROPY_PY_SYS_EXIT                 (1)
#define MICROPY_PY_SYS_PATH                 (1)
#define MICROPY_PY_SYS_ARGV                 (1)

// POSIX-style exit codes from pyexec_file/pyexec_vstr: success returns 0,
// uncaught exception returns 1, sys.exit(N) returns N. Without this, the
// embedded REPL convention applies and pyexec returns 1 on normal success.
#define MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)
// Honour the global mp_compile_only flag in pyexec, so `-X compile-only`
// can compile a script and exit without running it (matches CPython).
#define MICROPY_PYEXEC_COMPILE_ONLY         (1)
#define MICROPY_PY_SYS_MODULES              (1)
// sys.atexit registry; needed by the test runner's `-m` invocation so
// modules that register atexit callbacks see them fire on a clean exit.
#define MICROPY_PY_SYS_ATEXIT               (1)
// Phase 24: keep more REPL history since disk storage is cheap and the
// ring is reloaded from S:MicroPython.history on startup.  Default is 8.
#define MICROPY_READLINE_HISTORY_SIZE       (32)
// sys.stdin/stdout/stderr stream objects backed by the mphal stdio HAL.
// Implementation lives in ports/amiga/sysstdio.c.
#define MICROPY_PY_SYS_STDFILES             (1)
#define MICROPY_PY_SYS_STDIO_BUFFER         (0)

// Static buffer for exceptions raised while the heap is locked, so the
// exception's args don't get lost to a failed alloc (the test
// micropython/emg_exc.py exercises this via heap_lock + raise).
#define MICROPY_ENABLE_EMERGENCY_EXCEPTION_BUF (1)
#define MICROPY_EMERGENCY_EXCEPTION_BUF_SIZE   (256)

// Track the C stack ceiling so deep recursion turns into a Python
// RuntimeError ("maximum recursion depth exceeded") rather than running
// off the AmigaDOS stack and faulting the CPU. mp_stack_set_limit() is
// called from main() with the standard 40 KB ceiling.
#define MICROPY_STACK_CHECK                    (1)

// Use the pystack for VM code-state allocations. The default path uses
// alloca for small functions, but bebbo gcc on 68k returns mixed 2-byte
// and 4-byte aligned addresses from alloca; the VM relies on mp_obj_t
// stack slots being 4-byte aligned (mp_obj_is_obj checks the low two
// bits of the pointer). Routing through mp_pystack_alloc gives a single
// AllocVec-backed buffer with deterministic 8-byte alignment.
#define MICROPY_ENABLE_PYSTACK              (1)

// No threads — AmigaOS uses cooperative multitasking.
#define MICROPY_PY_THREAD                   (0)

// No asyncio until threads/event loop are sorted.
#define MICROPY_PY_ASYNCIO                  (0)

// Scheduler is used by asyncio and ussl callbacks; disable for now.
#define MICROPY_ENABLE_SCHEDULER            (0)

// Native code emitter for Motorola 68020. Variants for low-memory
// machines (a500) disable this to save flash.
#ifndef MICROPY_EMIT_68K
#define MICROPY_EMIT_68K                    (1)
#endif
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

// os.getenv / os.putenv / os.unsetenv backed by dos.library
// GetVar / SetVar / DeleteVar. Function bodies live in ports/amiga/modos.c
// and are pulled in by extmod/modos.c via MICROPY_PY_OS_INCLUDEFILE. This
// gives MicroPython and the AmigaShell a single shared env-var store
// (ENV: / GVF_GLOBAL_ONLY).
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (1)
#define MICROPY_PY_OS_INCLUDEFILE           "ports/amiga/modos.c"

// Amiga-specific module
#define MICROPY_PY_AMIGA                    (1)

// bsdsocket.library networking (optional — silently absent if lib not found).
// a500 disables this to keep the build small.
#ifndef MICROPY_PY_AMIGA_SOCKET
#define MICROPY_PY_AMIGA_SOCKET             (1)
#endif

// AmiSSL v5 TLS (Phase 28). Defaults to whatever the socket layer is
// — TLS without sockets is meaningless on this port. Builds without
// the AmiSSL SDK installed must override this to 0 at make time.
#ifndef MICROPY_PY_AMIGA_SSL
#define MICROPY_PY_AMIGA_SSL                (MICROPY_PY_AMIGA_SOCKET)
#endif

// Platform string visible as sys.platform
#define MICROPY_PY_SYS_PLATFORM             "amiga"

// Board / MCU identity strings. MCU name is variant-overridable so
// `import sys; sys.implementation` reports the actual target CPU.
#define MICROPY_HW_BOARD_NAME               "Amiga"
#ifndef MICROPY_HW_MCU_NAME
#define MICROPY_HW_MCU_NAME                 "68020"
#endif

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
