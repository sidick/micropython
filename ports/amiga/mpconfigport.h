/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Simon Dick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdint.h>
#include <stddef.h>

// Variant overrides are pulled in first so the #ifndef-guarded defaults
// below can be replaced per variant. Each variant ships a mpconfigvariant.h
// under ports/amiga/variants/<variant>/.
#include "mpconfigvariant.h"

// NLR: py/nlr.h auto-detects __m68k__ and selects the register-based NLR
// implemented in ports/amiga/nlr68k.S + nlr68k.c. This is required for
// try/except/with to work inside @micropython.native code, where the
// exception handler PC is carried in REG_LOCAL_1 (D7) across nlr_push.

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

// time module wall-clock surface (Phase 39 step 1). time.time() /
// time.time_ns() are backed by timer.device GetSysTime() (already
// opened at startup for ticks_*/delay_*); gmtime/localtime/mktime go
// through extmod/modtime.c's timeutils path. Epoch is Unix (1970) to
// match CPython; the 1978-1970 offset is applied in ports/amiga/modtime.c.
#define MICROPY_EPOCH_IS_1970               (1)
#define MICROPY_PY_TIME_GMTIME_LOCALTIME_MKTIME (1)
#define MICROPY_PY_TIME_TIME_TIME_NS        (1)
#define MICROPY_PY_TIME_INCLUDEFILE         "ports/amiga/modtime.c"

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

// asyncio: cooperative event loop (no OS threads). IO readiness comes
// from select.poll() over bsdsocket (modsocket.c MP_STREAM_POLL), and the
// loop idles in mp_event_wait, which sleeps via MICROPY_INTERNAL_WFE
// instead of busy-spinning.
#define MICROPY_PY_ASYNCIO                  (1)

// Enables micropython.schedule() and the pending-callback queue asyncio
// and stream callbacks build on.
#define MICROPY_ENABLE_SCHEDULER            (1)

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

// Suspend backend for mp_event_wait_ms()/_indefinite(): without it the
// default is a no-op and select()/poll() idle-waits busy-spin the CPU.
// amiga_internal_wfe() sleeps in short Ctrl-C-aware slices (mphalport.c).
#define MICROPY_INTERNAL_WFE(TIMEOUT_MS) amiga_internal_wfe(TIMEOUT_MS)

// os.getenv / os.putenv / os.unsetenv backed by dos.library
// GetVar / SetVar / DeleteVar. Function bodies live in ports/amiga/modos.c
// and are pulled in by extmod/modos.c via MICROPY_PY_OS_INCLUDEFILE. This
// gives MicroPython and the AmigaShell a single shared env-var store
// (ENV: / GVF_GLOBAL_ONLY).
#define MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (1)
#define MICROPY_PY_OS_INCLUDEFILE           "ports/amiga/modos.c"

// Amiga-specific module
#define MICROPY_PY_AMIGA                    (1)

// Shared buffer size for "AmigaOS full filesystem path with comfortable
// headroom for long-name filesystems (SFS / PFS3 / FFS2 with the
// long-names patch, all allowing ~105-byte filenames per component)."
// Used by modasl.c, modamiga.c (match / WB-launched-args path
// reconstruction), and any future surface that builds a full path.
// Narrower buffers (e.g. assign-target resolution, history-file path
// in ENVARC:) deliberately stay smaller -- their inputs are bounded.
#define AMIGA_PATH_MAX                      1024

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

// hashlib MD5 + SHA1 (Phase 39). The upstream extmod/modhashlib.c gates
// these on MICROPY_PY_SSL because it routes through mbedtls / axtls for
// the algorithm itself; we don't have mbedtls and AmiSSL is OpenSSL-shaped
// (wrong API). Take the axtls code path -- only md5.c and sha1.c are
// compiled, no other axtls components are pulled in because MICROPY_PY_SSL
// stays 0 (modtls_axtls.c and modcryptolib.c remain dormant). SHA-256
// continues to use lib/crypto-algorithms/sha256.c, same as before.
#define MICROPY_SSL_AXTLS                   (1)
#define MICROPY_PY_HASHLIB_MD5              (1)
#define MICROPY_PY_HASHLIB_SHA1             (1)

// deflate write/compress (Phase 39). MICROPY_PY_DEFLATE is already on
// at EXTRA_FEATURES, giving us DeflateIO-as-decompressor; flipping
// MICROPY_PY_DEFLATE_COMPRESS lights up the write side. uzlib's lz77.c
// + defl_static.c are #include'd by extmod/moddeflate.c under this
// flag, so no extra SRC_C wiring is needed.
#define MICROPY_PY_DEFLATE_COMPRESS         (1)

// btree (Phase 39). modbtree.c needs mp_stream_posix_{read,write,lseek,fsync}
// to bridge berkeley-db's read/write/lseek calls onto a Python stream
// object; those are declared in py/stream.h and defined in py/stream.c
// under MICROPY_STREAMS_POSIX_API. Also gates the MICROPY_PY_BTREE C
// macro -- the Makefile sets the matching make variable so extmod.mk
// pulls in lib/berkeley-db-1.xx/btree/*.c + mpool/mpool.c.
#define MICROPY_STREAMS_POSIX_API           (1)

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
typedef int mp_int_t;
typedef unsigned int mp_uint_t;
typedef long mp_off_t;

// alloca is available from GCC built-ins / newlib
#include <alloca.h>

#define MP_STATE_PORT MP_STATE_VM
