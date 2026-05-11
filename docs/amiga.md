# MicroPython port to AmigaOS 3.x — Implementation Plan

## Overview

This document describes a plan to port MicroPython to AmigaOS 3.x running on Motorola 68k hardware (68000/68020/68030/68040/68060). The target is a CLI-driven REPL with file-system access, using the `ports/minimal` port as the structural template.

### Goals

- Phase 0 ✅ Toolchain: bebbo GCC installed and working
- Phase 1 ✅ Skeleton: port builds and runs in Amiberry emulator
- Phase 2 ✅ File system access and `import` from AmigaDOS volumes
- Phase 3 ✅ Standard MicroPython library modules
- Phase 4 ✅ Amiga-specific `amiga` C module (exec/dos library bindings)
- Phase 5 ✅ 68k native code emitter (`--emit native` / `@micropython.native`)
- Phase 6 ✅ Package imports (`import mypackage`) via `Lock`/`Examine`
- Phase 7 ✅ Ctrl+C interrupt handling via dos.library break signals
- Phase 8 ✅ Native AmigaOS API migration (replace newlib stdio with dos.library)
- Phase 9 ✅ Networking via `bsdsocket.library` (`socket` module)
- Phase 10 ✅ Command-line argument parsing (`micropython script.py`, `-c`, `-m`, `-h`)
- Phase 11 — CI build workflow
- Phase 12 — 68k native emitter rework (fix ASM_CALL_IND crash; ~47 native_*/viper_* tests blocked on this)
- Phase 13 ✅ Interactive line editing at the REPL (cursor keys, history, kill/yank) via `shared/readline/`
- Phase 14 — Dynamic heap growth via `gc_add()` + runtime-configurable initial heap size
- Phase 15 — Expose AmigaOS memory-pool API (`CreatePool`/`AllocPooled`/`FreePooled`/`DeletePool`) in the `amiga` module
- Phase 16 ✅ Pythonic file I/O: VFS layer (`os.chdir`, `os.listdir`, `os.stat`, etc.) backed by `dos.library`
- Phase 17 — Native AmigaOS library access: generic `lib_open`/`lib_call` trampoline plus `.fd`-driven `amiga.library(...)` proxy for system and third-party libraries
- Phase 18 — ARexx integration: inbound `MICROPYTHON.1` port plus outbound `amiga.rexx(port, command)` client via `rexxsyslib.library`
- Phase 19 — Workbench launch support: handle `WBStartup` message and tooltype-driven configuration via `icon.library`
- Phase 20 ✅ Env-var integration: `os.getenv` / `os.putenv` / `os.unsetenv` backed by `dos.library` `GetVar`/`SetVar` (local vars, matching Unix semantics; `ENV:` and `ENVARC:` writeable directly for shell-shared / persistent vars)
- Phase 21 — Volume / assign introspection: `amiga.volumes()`, `amiga.assigns()`, `amiga.disk_info(path)`
- Phase 22 — AmigaDOS pattern matching: expose `MatchFirst`/`MatchNext` as `amiga.match("#?.py")`
- Phase 23 — `timer.device`-backed timing: replace newlib `clock()` with MICROHZ-accurate `mp_hal_delay_us` / `ticks_us`
- Phase 24 — Persistent REPL history saved to `ENVARC:MICROPYTHON_HISTORY`
- Phase 25 — Extra break signals: expose `SIGBREAKF_CTRL_D/E/F` and `amiga.signal()` / `amiga.wait_signal()`
- Phase 26 — `PROGDIR:` on `sys.path`: automatic per the standard Amiga "next-to-executable" convention
- Phase 27 ✅ Additional build variants: `a1200` (stock unaccelerated A1200), `68020fpu` (68020/68030 with 68881 coprocessor), `68040` (68040 with built-in FPU); FPU variants drop the libgcc soft-float wraps

### Non-goals (initially)

- Workbench GUI / Intuition window — CLI only
- 68000 alignment-safe build — target 68020+ first

---

## Technical Context

### CPU and ABI

- **Architecture**: Motorola 68k (big-endian, 32-bit)
- **Target minimum**: 68020 (allows unaligned access, required by MicroPython's default allocator)
- **Calling convention**: AmigaOS register-based for OS calls; standard C ABI for everything else
- **Endianness**: Big-endian → `MP_ENDIANNESS_LITTLE` will be `0` (auto-detected from GCC's `__BYTE_ORDER__`)

### AmigaOS 3.x APIs used

| Purpose | Current implementation | Target (post-NDK) |
|---------|----------------------|-------------------|
| Memory allocation | Static BSS array | `exec.library`: `AllocVec(size, MEMF_ANY\|MEMF_PUBLIC\|MEMF_CLEAR)` / `FreeVec()` |
| Console input | `getchar()` (newlib) | `dos.library`: `FGetC(Input())` |
| Console output | `fwrite()` (newlib) | `dos.library`: `Write(Output(), buf, len)` |
| File I/O | `fopen`/`fread` (newlib) | `dos.library`: `Open()`, `Close()`, `Read()`, `Write()`, `Seek()` |
| Directory stat | `fopen`-based (files only) | `dos.library`: `Lock()` / `Examine()` / `UnLock()` |
| Wall clock | `clock()` (newlib) | `dos.library`: `DateStamp()` |
| Stack bounds | local-var heuristic | `exec.library`: `FindTask(NULL)` → `tc_SPLower` / `tc_SPUpper` |

### Toolchain

**bebbo's GCC** (`m68k-amigaos-gcc`)

- **Confirmed version**: GCC 6.5.0b (installed at `/opt/amiga`)
- Produces native AmigaOS HUNK-format executables
- Install: <https://franke.ms/git/bebbo/amiga-gcc>

```sh
git clone https://franke.ms/git/bebbo/amiga-gcc
cd amiga-gcc

# Full build — recommended; installs gcc, binutils, clib2, NDK, etc.
make PREFIX=/opt/amiga all

export PATH=/opt/amiga/bin:$PATH
m68k-amigaos-gcc --version
```

Key make targets:
- `all` — build and install everything (recommended)
- `min` — compiler binaries only; **no C library headers — will not compile MicroPython**
- `clib2` — modern ANSI C library (required if not using `all`)
- `libnix` — traditional AmigaOS C library (alternative to clib2)
- `ndk` — Amiga NDK headers (required for direct exec/dos.library calls)

> **Confirmed:** `make min` does not install standard C library headers (stdint.h, string.h, stdio.h, etc.) and cannot compile MicroPython. Always use `make all` or at minimum `make clib2 ndk`.

### Exception handling (NLR)

MicroPython's NLR system auto-selects `nlrsetjmp.c` for any unrecognised architecture. The 68k falls into this path — no assembly NLR implementation is needed. Explicitly set in `mpconfigport.h`:

```c
#define MICROPY_NLR_SETJMP (1)
```

### GC and heap

Currently uses a static `char heap[MICROPY_HEAP_SIZE]` in BSS (256 KB). This avoids any OS calls during startup and works without the NDK.

Once the NDK is installed, switch to `AllocVec` for a runtime-configurable heap:

```c
void *heap = AllocVec(MICROPY_HEAP_SIZE,
                      MEMF_ANY | MEMF_PUBLIC | MEMF_CLEAR);
gc_init(heap, (char *)heap + MICROPY_HEAP_SIZE);
```

`MEMF_ANY` lets AmigaOS hand out the fastest available memory
(Fast > Slow > Chip on a system with all three), so a single call
suffices and chip-RAM-only machines still get served. `MEMF_CLEAR`
zeroes the region before `gc_init()` sets up its bookkeeping.

For GC root scanning, the port captures the address of a local in
`main()` into `gc_stack_top` at startup and scans from the current SP
(approximated by a local in `gc_collect`) up to that ceiling.

`task->tc_SPUpper` would be the principled choice on real AmigaOS, but
vamos leaves both `tc_SPUpper` and `tc_SPLower` zero on the initial
process. The naive computation `(tc_SPUpper - &dummy) / sizeof(void *)`
then produces a ~4 GB scan length which walks off into unmapped memory
and faults at vamos's 1 MiB RAM ceiling. Bounding the scan to main()'s
frame and below is sufficient: nothing above that holds MicroPython
object references.

---

## Port file structure

```
ports/amiga/
├── Makefile              # Build rules and toolchain config
├── mpconfigport.h        # Feature flags and type definitions
├── mphalport.h           # Inline HAL stubs (ticks, interrupt char)
├── mphalport.c           # Console I/O HAL
├── main.c                # Entry point, gc_collect(), required stubs
├── vfs_amiga.c           # VfsAmiga + FileIO/TextIOWrapper (Phase 16; replaces amigafile.c + amigaio.c)
├── sysstdio.c            # mp_sys_stdin/stdout/stderr stream objects
├── floatconv.c           # bebbo soft-float library bug fixes
├── modjson.c             # Port-local json module (json.loads bypass for 68k)
├── qstrdefsport.h        # Port-specific interned strings
└── variants/
    └── standard/
        ├── mpconfigvariant.h
        └── mpconfigvariant.mk
```

---

## Phase 0 — Toolchain ✅

bebbo GCC 6.5.0b is installed at `/opt/amiga`. Cross-compilation confirmed working.

```sh
export PATH=/opt/amiga/bin:$PATH
m68k-amigaos-gcc --version
# m68k-amigaos-gcc (GCC) 6.5.0b
```

---

## Phase 1 — Skeleton ✅

The port builds successfully. Output is confirmed as an AmigaOS loadseg()ble executable:

```
build/micropython: AmigaOS loadseg()ble executable/binary
architecture: m68k:68000
text: 185580  data: 864  bss: 263404
```

Build from `ports/amiga/`:

```sh
export PATH=/opt/amiga/bin:$PATH
make clean && make
```

**Confirmed running** in Amiberry emulator.

Known limitations of the current newlib stdio I/O:
- Ctrl+D (EOF) does not exit the REPL. AmigaOS uses Ctrl+\ for EOF; `mp_hal_stdin_rx_chr` maps it to the MicroPython exit character (Ctrl+D, ASCII 4). `sys.exit(0)` also works.
- Ctrl+C does not interrupt execution; requires dos.library break signal handling (phase 2).

### Key implementation notes

**mpconfigport.h** — notable settings:
- `MICROPY_NLR_SETJMP (1)` — explicit setjmp-based exceptions
- `MICROPY_CONFIG_ROM_LEVEL_CORE_FEATURES` — enables array, collections, io, struct, gc, sys, micropython modules
- `MICROPY_HEAP_SIZE (256 * 1024)` — static heap, increase as needed
- `MICROPY_PY_THREAD (0)` — AmigaOS cooperative multitasking only
- `MICROPY_PY_ASYNCIO (0)` — deferred
- `mp_int_t` = `int`, `mp_uint_t` = `unsigned int` (both 32-bit on 68k)
- `MICROPY_LONGINT_IMPL_LONGLONG` — 64-bit ints via the C `long long`
  type. Needed for the `'q'`/`'Q'` struct codes (which silently fall
  through to garbage on the SMALLINT-only build) and for source-level
  literals larger than 30 bits (e.g. `0xFFFF_FFFF` in feature-detect
  comparisons). Adds about 5 KB to the binary.
- `MICROPY_ENABLE_PYSTACK (1)` — required, not optional. The default
  path uses `alloca` for small VM code states, but bebbo gcc 6.5 returns
  mixed 2-byte and 4-byte aligned addresses from `alloca` on 68k. The VM
  walks `mp_obj_t state[]` (each slot 4 bytes) on this stack, and an
  iterator constructed in those slots via `MP_BC_GET_ITER_STACK` becomes
  unreachable on the next `MP_BC_FOR_ITER` because `mp_obj_is_obj`'s
  `(o & 3) == 0` check fails when the slot address is 2-byte aligned.
  Routing through `mp_pystack_alloc` (initialised in `main.c` with a
  4 KB `aligned(8)` buffer) gives deterministic alignment.

**mphalport.c** — must supply `mp_builtin_open_obj`:
The io module (`py/modio.c`) references `mp_builtin_open_obj` (Python's `open()`) but deliberately does not define it — this is port-supplied. A `NotImplementedError` stub is provided until phase 2:

```c
static mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("open() not yet implemented"));
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
```

**amigafile.c** — clib2 lacks `stat()`:
AmigaOS's clib2 does not provide POSIX `stat()`. Directory detection is not yet possible; `mp_import_stat()` uses `fopen` to detect files only (returns `NO_EXIST` for directories). Package imports (`import mypackage/`) will not work until the NDK's `Lock`/`Examine` API is wired up in phase 2.

### Build gotchas

| Issue | Cause | Fix |
|-------|-------|-----|
| Empty `qstr.i.last` on second build | File created as zero-bytes when first build failed before clib2 was installed; `make` treats it as up-to-date | `make clean` before rebuilding after toolchain changes |
| `mp_import_stat_t` undeclared | Missing `#include "py/builtin.h"` | Added to `amigafile.c` |
| `stat()` undefined at link | clib2 does not provide POSIX `stat()` | Replaced with `fopen`-based check |
| `mp_module_io` undefined at link | `mp_builtin_open_obj` not provided; io module compiled but `mp_module_io` symbol absent | Added `mp_builtin_open_obj` stub to `mphalport.c` |
| `STATIC` unknown type | `STATIC` macro was removed from MicroPython in a recent cleanup | Use plain `static` |

---

## Phase 2 — File System and `import` ✅

Implemented using newlib stdio — no NDK required. `open()`, `read()`, `write()`,
`seek()`, `tell()`, `readline()`, and the `with` statement all work. `import` from
files on any mounted volume also works via `amigafile.c`.

New file: `ports/amiga/amigaio.c` — implements `mp_builtin_open_obj` as a proper
file object type wrapping `FILE*`. Both binary (`FileIO`) and text (`TextIOWrapper`)
modes are supported. `MICROPY_ENABLE_FINALISER` is enabled so files are closed
when garbage collected.

`Q(FileIO)`, `Q(TextIOWrapper)`, and `Q(mode)` were added to `qstrdefsport.h`
since they are not in MicroPython's core qstr table.

### Future replacement with dos.library (optional)

If direct AmigaOS dos.library calls are needed (e.g. for async I/O or to avoid
newlib overhead), replace `amigafile.c` and `amigaio.c` with NDK-based versions.
The dos.library sketches below are preserved for reference.

Requires NDK to be installed (`make PREFIX=/opt/amiga ndk`).

#### mp_lexer_new_from_file (dos.library version)

### mp_lexer_new_from_file

```c
#include <proto/dos.h>
#include "py/lexer.h"
#include "py/builtin.h"
#include "py/runtime.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *path = qstr_str(filename);
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        mp_raise_OSError(MP_ENOENT);
    }

    LONG size = Seek(fh, 0, OFFSET_END);
    Seek(fh, 0, OFFSET_BEGINNING);

    char *buf = m_new(char, size + 1);
    Read(fh, buf, size);
    Close(fh);
    buf[size] = '\0';

    return mp_lexer_new_from_str_len(filename, buf, size, true);
}
```

### mp_import_stat

```c
mp_import_stat_t mp_import_stat(const char *path) {
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    if (!lock) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    mp_import_stat_t result = MP_IMPORT_STAT_NO_EXIST;
    if (fib && Examine(lock, fib)) {
        result = (fib->fib_DirEntryType > 0) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
    }
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return result;
}
```

Also replace `mp_builtin_open_obj` stub with a real implementation, and switch console I/O from newlib stdio to `dos.library` `FGetC`/`Write`.

### Deliverable

`import mymodule` works when `mymodule.py` exists on any mounted AmigaDOS volume.

---

## Phase 3 — Standard Library Modules ✅

`MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES` is set in `mpconfigport.h`. The following
modules are confirmed working on Amiberry: `math`, `struct`, `json`, `re`, `hashlib`,
`float`. `sys.platform == "amiga"` works.

The 68k has no hardware FPU. `-msoft-float` (already in CFLAGS) causes GCC to emit
software float routines — correct but slower than hardware. Use `MICROPY_FLOAT_IMPL_NONE`
for minimal builds.

### json.loads workaround

`json.loads` from upstream `extmod/modjson.c` fails on 68k with
`OSError: stream operation not supported`. The upstream implementation creates a
stack-allocated `mp_obj_stringio_t` and passes it through MicroPython's stream
protocol. This fails because `mp_obj_is_obj()` requires the object pointer to be
4-byte aligned, which the 68k SysV ABI does not guarantee for all stack frames.

**Fix**: `ports/amiga/modjson.c` — a port-local replacement that bypasses the stream
protocol entirely for `json.loads`. The parser body is extracted into an internal
`json_parse(json_stream_t *s)` helper. `mod_json_loads` builds a `json_buf_t` on the
stack and calls `json_parse` directly via `json_buf_read`, with no `mp_obj_t` cast on
the buffer struct. `mod_json_load` (for file streams) is unchanged.

The upstream file is excluded from the build in `Makefile`:

```makefile
SRC_EXTMOD_C := $(filter-out extmod/modjson.c, $(SRC_EXTMOD_C))
```

And `modjson.c` is added to `SRC_C` to pick up the port-local version.

### Deliverable

`import math`, `import struct`, `import json`, `import re` work. `sys.platform == "amiga"` works.

---

## Phase 4 — Amiga-specific `amiga` Module ✅

`ports/amiga/modamiga.c` exposes key exec.library and dos.library functions to
Python. The NDK is available at `/opt/amiga/m68k-amigaos/ndk-include/` and is on
the default compiler include path — no extra `-I` flag needed.

`modamiga.c` must be in `SRC_QSTR` (not just `SRC_C`) so that `MP_REGISTER_MODULE`
and its qstr identifiers are picked up by `makemoduledefs.py` and `makeqstrdefs.py`.

### API

```python
import amiga

amiga.os_version()          # → (version, revision) tuple from ExecBase
amiga.find_task(name=None)  # → int address of named task, or current task if None
amiga.alloc_vec(size, flags=amiga.MEMF_ANY)  # → int address; raises MemoryError on failure
amiga.free_vec(addr)        # → None
amiga.execute(cmd)          # → int return code (0=OK, 5=WARN, 10=ERROR, 20=FAILURE, -1=failed to start)
amiga.exists(path)          # → bool; does this file/dir/volume exist? Suppresses
                            #   AmigaDOS volume requesters around the Lock()

# Memory flags
amiga.MEMF_ANY              # 0
amiga.MEMF_PUBLIC           # 1
amiga.MEMF_CHIP             # 2
amiga.MEMF_FAST             # 4
amiga.MEMF_CLEAR            # 0x10000
```

### Notes

- `amiga.execute()` uses `SystemTagList()` (not `Execute()`). `Execute()` returns
  an AmigaOS BOOL where TRUE = -1, giving no indication of the command's exit code.
  `SystemTagList()` returns the actual CLI return code.
- `amiga.find_task(None)` calls `FindTask(NULL)` which returns the current task pointer.
- `amiga.exists()` wraps `Lock(SHARED_LOCK)` / `UnLock()` with `pr_WindowPtr`
  saved and temporarily set to `-1`, so probing a path on an unmounted volume
  returns `False` instead of stalling on a "Please insert volume X:" requester.
  The requester suppression is scoped to just the `Lock` call.

### Deliverable

`import amiga; print(amiga.os_version())` prints the AmigaOS version tuple.
`amiga.execute("echo hello")` returns 0.

---

## Phase 5 — 68k Native Code Emitter ✅

`@micropython.native` and `--emit native` are now supported via the GENERIC_ASM_API.

### Files added

| File | Role |
|------|------|
| `py/asm68k.h` | 68k instruction encoder: MOVE, ADD/SUB/MUL, logical, shift, CMP, Scc, LEA, Bcc/BRA, JSR/RTS, LINK/UNLK, MOVEM |
| `py/asm68k.c` | Non-inline implementations: entry/exit, call_ind, branches, local access, load/store |
| `py/emit68k.c` | Thin wrapper: `#define N_68K 1` + `GENERIC_ASM_API`, then `#include "py/emitnative.c"` |

### Design decisions

**Register allocation:**

| Role | Register | Note |
|------|----------|------|
| REG_RET / REG_ARG_1 | D0 | Also return value |
| REG_ARG_2 | D1 | |
| REG_ARG_3 | D2 | Callee-saved; loaded from 16(A5) in prologue |
| REG_ARG_4 | D3 | Callee-saved; loaded from 20(A5) in prologue |
| REG_TEMP0/1/2 | D4, D5, D6 | Scratch |
| REG_LOCAL_1 | D7 | Only cached-local register (data reg safe for arithmetic) |
| REG_LOCAL_2 | A2 | Address register; used for REG_GENERATOR_STATE |
| REG_LOCAL_3 | A3 | Address register; used for REG_QSTR_TABLE |
| REG_FUN_TABLE | A4 | Points to `mp_fun_table` |
| Frame pointer | A5 | LINK/UNLK; locals at (local-n_locals)*4(A5) |
| A0 | scratch | Used by `asm_68k_ensure_areg` when base is Dn |

**Calling convention:** AmigaOS/cdecl — args pushed right-to-left.
- Entry prologue: `LINK A5, #-N; MOVEM.L D2-D7/A2-A4, -(SP)` then load 4 args from stack.
- `ASM_CALL_IND`: push D3/D2/D1/D0; `MOVEA.L (idx*4, A4), A0; JSR (A0); ADDA.L #16, SP`.
- Branches: always `.W` form (4 bytes: opcode + int16 displacement); displacement = target − (instruction + 2).
- Comparisons: `CMP.L reg_rhs, REG_ARG_2; Scc REG_RET; ANDI.L #1, REG_RET`.
- `MAX_REGS_FOR_LOCAL_VARS = 1`: only D7 used for register-cached locals; A2/A3 are address registers and cannot be used for general arithmetic by `emitnative.c`.

### Known limitations

- **try/except in native mode is broken.** With `MICROPY_NLR_SETJMP = 1`, the `nlr_buf_t` slot used by `NLR_BUF_IDX_LOCAL_1` falls inside the `jmp_buf`, which is overwritten by `setjmp`. Functions using exceptions are unsafe in native/viper mode. Non-exceptional native functions work correctly.
- **Viper integer arithmetic on address registers** is prevented by `MAX_REGS_FOR_LOCAL_VARS = 1`. Viper `int`/`uint` locals beyond the first will always use stack slots.
- **Calling another function from a `@micropython.native` body crashes.** Tracked as Phase 12 — see that section for symptoms and probable causes.

### Configuration

```c
// mpconfigport.h
#define MICROPY_EMIT_68K (1)
```

---

## Future Work

### Phase 6 — Package imports ✅

`mp_import_stat()` in `amigafile.c` now uses dos.library `Lock`/`Examine` to
distinguish files from directories. `fib_DirEntryType > 0` means directory,
`< 0` means file. `import mypackage` works when `mypackage/__init__.py` exists
on any mounted AmigaDOS volume.

---

### Phase 7 — Ctrl+C interrupt handling ✅

Ctrl+C now works via two paths:

1. **During computation**: `MICROPY_VM_HOOK_LOOP` polls `CheckSignal(SIGBREAKF_CTRL_C)`
   every 1024 bytecodes in `amiga_check_ctrl_c()` (`mphalport.c`) and calls
   `mp_sched_keyboard_interrupt()` when the signal is set. This delivers
   `KeyboardInterrupt` to a running script.

2. **During input**: `mp_hal_stdin_rx_chr()` checks `mp_interrupt_char` (set to Ctrl+C by
   `pyexec.c` before running user code) and calls `mp_sched_keyboard_interrupt()` if the
   typed character matches.

`shared/runtime/interrupt_char.c` provides `mp_interrupt_char` and
`mp_hal_set_interrupt_char()`, replacing the previous no-op stub in `mphalport.h`.

---

### Phase 8 — Native AmigaOS API migration ✅

All newlib stdio replaced with direct dos.library calls:

| Component | Was | Now |
|-----------|-----|-----|
| Console input | `getchar()` | `FGetC(Input())` via `WaitForChar()` 200ms poll |
| Console output | `fwrite(stdout)` | `Write(Output(), buf, len)` |
| File I/O | `FILE*` + `fopen`/`fread`/`fwrite` | `BPTR` + `Open`/`Read`/`Write`/`Close`/`Seek`/`Flush` |
| Heap | static 256 KB BSS array | `AllocVec(MEMF_ANY\|MEMF_PUBLIC\|MEMF_CLEAR)` |
| GC stack bounds | local-variable heuristic | `FindTask(NULL)->tc_SPUpper` |
| `mp_hal_delay_ms` | `clock()` busy-wait | `Delay()` (yields to other tasks) |

The `WaitForChar()` poll also improves Ctrl+C responsiveness during input: instead of
blocking in `FGetC`, the REPL polls every 200 ms and calls `amiga_check_ctrl_c()`
between polls.

Remaining newlib use: `clock()` in `mp_hal_delay_us` (sub-millisecond busy-wait) and
`mp_hal_ticks_*` in `mphalport.h`; a timer.device implementation would improve
accuracy but is not required for normal use.

---

### Phase 9 — Networking (`bsdsocket.library`) ✅

`ports/amiga/modsocket.c` implements the `socket` module via `bsdsocket.library`.
Key implementation details:

- `SocketBase` is opened in `main()` via `amiga_socket_open()`; silently absent if
  the library is not installed (socket creation raises `OSError`).
- `errno` is per-library: `Errno()` used instead of global errno.
- `IoctlSocket(FIONBIO)` for non-blocking mode; `SO_RCVTIMEO`/`SO_SNDTIMEO` for timeouts.
- `Inet_NtoA()` / `inet_addr()` / `gethostbyname()` for address conversion.
- `getaddrinfo()` / `freeaddrinfo()` / `gethostname()` for higher-level resolution.
- `__NO_NETINCLUDE_TIMEVAL` guard avoids `devices/timer.h` conflict with `sys/time.h`.
- `CloseSocket()` (not `close()`) used to close socket descriptors.

The socket object implements MicroPython's stream protocol (read/write/ioctl), so
it can be used with `readline()`, `with` statements, and anywhere a stream is expected.

```python
import socket
s = socket.socket()
s.connect(('1.1.1.1', 80))
s.send(b'GET / HTTP/1.0\r\nHost: 1.1.1.1\r\n\r\n')
print(s.recv(512))
s.close()
```

---

### Phase 10 — Command-line argument parsing ✅

`main.c` now parses `argc`/`argv` before starting the REPL:

```sh
micropython                     # interactive REPL (unchanged)
micropython script.py [args]    # run a file; sys.argv = ["script.py", ...]
micropython -c "code" [args]    # run a statement; sys.argv = ["-c", ...]
micropython -m module [args]    # run a module; sys.argv = ["module", ...]
micropython -h / --help         # print help and exit
micropython --version           # print version string and exit
```

Implementation notes:

- `MICROPY_PY_SYS_ARGV (1)` enabled; `mp_sys_argv` initialised to an empty list after `mp_init()`.
- `mp_sys_path` initialised to `[""]` (empty string = current directory on AmigaOS).
  For a script file, its directory is prepended to `sys.path[0]` using AmigaOS path parsing
  (`"Work:scripts/foo.py"` → `"Work:scripts"`; `"Work:foo.py"` → `"Work:"`).
- `-c` uses `pyexec_vstr()` from `shared/runtime/pyexec.c`.
- `-m` calls `mp_builtin___import__()` with `fromlist=False` inside an `nlr_buf_t`.
- Script files use `pyexec_file()` which handles its own NLR and exception printing.
- Raw console mode (`SetMode(stdin_fh, 1)`) is only applied for the REPL, not for scripts or `-c`/`-m`.
- `genhdr/mpversion.h` (generated at build time) is now included in `main.c` so that
  `MICROPY_BANNER_NAME_AND_VERSION` / `MICROPY_GIT_TAG` are defined.

---

### Phase 11 — CI build workflow

Add `.github/workflows/ports_amiga.yml` to confirm the port cross-compiles on Linux
without an emulator run. bebbo GCC can be installed in CI via a binary release:

```yaml
- name: Install bebbo GCC
  run: |
    curl -L https://github.com/bebbo/amiga-gcc/releases/download/2024-01-01/amiga-gcc-linux.tar.bz2 | \
      sudo tar -xj -C /opt
    echo "/opt/amiga/bin" >> $GITHUB_PATH
```

---

### Phase 12 — 68k native emitter rework

Phase 5 landed a working 68k native code emitter, but only for the
single-statement `return <const>` shape. The moment a
`@micropython.native` body calls another function — `print(1)`,
indexing, slicing, `range()`, anything that goes through
`mp_fun_table` — the CPU jumps off into the 68k vector-table area
(typically PC=0x404, SR=0x0700) and faults. About 47 of the
`tests/micropython/native_*.py` and `tests/micropython/viper_*.py`
tests fail in exactly this pattern, with consistent symptoms:

- `D0 = self_in` (function object) is correct on entry
- `REG_FUN_TABLE` (A4) ends up pointing at random memory by the
  time `ASM_CALL_IND` runs
- The indirect `MOVEA.L (idx*4, A4), A0; JSR (A0)` then dereferences
  the wrong address and lands in the vector table

**Probable causes (need investigation):**

1. **Prologue ordering.** `asm_68k_entry` does `LINK A5,#-N; MOVEM.L
   D2-D7/A2-A4, -(SP); load D0..D3 from 8(A5)..20(A5)`. But A4 is in
   the saved set, so its value at function entry is whatever the
   parent left there. `emitnative.c` then loads A4 from the const
   table via `REG_PARENT_ARG_1` (D0), but the load goes through
   `asm_68k_ensure_areg` to materialise an areg from the dreg —
   verify that this still leaves A4 with the right value at the
   time of the first `ASM_CALL_IND`.
2. **Register clobbering through CALL_IND.** Bebbo cdecl says caller
   preserves nothing; we save D2–D7/A2–A4 across the *whole*
   function but every `ASM_CALL_IND` could clobber A4 inside the
   callee. If the C runtime function we call doesn't preserve A4,
   the next CALL_IND in the same body sees garbage. Need either
   per-call save/restore of A4 or a different register choice.
3. **Stack frame mismatch.** Verify that `sp += MP_OBJ_ITER_BUF_NSLOTS - 1`
   and friends in the VM's emit-time stack tracking match what
   `LINK A5, #-N` actually allocates. The pystack fix (Phase 8 +
   alignment work) deals with the bytecode VM stack, not the native
   emitter's frame layout.

The fix is a non-trivial emitter rework, but the existing tests in
`tests/micropython/native_*.py` and `tests/micropython/viper_*.py`
provide a precise repro corpus — start with the simplest one
(`native_const.py` line 14: a nested native function that just
returns 123, called from a wrapper) and work up.

---

### Phase 13 — Interactive line editing at the REPL ✅

`shared/readline/readline.c` drives the REPL. `pyexec_friendly_repl()` calls
`readline()` directly, so once the file was in `SRC_C` (since the initial
skeleton commit) and `MICROPY_HELPER_REPL` / `MICROPY_REPL_EMACS_KEYS` /
`MICROPY_REPL_AUTO_INDENT` were enabled (all true under
`MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES`), the only remaining piece was
escape-sequence translation.

**CSI translation.** AmigaOS's `console.device` emits cursor reports as a
single CSI byte (`0x9B`) followed by parameters, where xterm-style
consoles use the two-byte `ESC [` form that `shared/readline/` expects.
`mp_hal_stdin_rx_chr()` in `mphalport.c` keeps a one-byte pending buffer:
when `FGetC` returns `0x9B`, the function returns `ESC` (`0x1B`) and
hands `[` to the next call. Hosts that already deliver `ESC [` (vamos's
xterm pass-through) see no change because `0x9B` simply never appears.

This is enough for cursor left/right, up/down history, Home/End,
Delete-forward, and any future CSI key that `readline.c` already handles
on other ports. Pure `ESC` (no follow-up byte) still works as plain ESC
because nothing else writes to `pending`.

Verified under vamos with piped input:

- `5` + CSI D + `1` → `15` (cursor-left + insert)
- `x=42`<Enter> + CSI A + <Enter> → re-executes `x=42` (history recall)
- `abcdef` + `^A` + `^K` + `7` → `7` (kill to end of line)
- `999` + `^U` + `8` → `8` (kill to start)
- `99` + backspace + `9` → `99` (backspace)

`MICROPY_READLINE_HISTORY_SIZE` defaults to 8, which is fine for an
interactive REPL on a 256 KB heap. The `readline_hist` root pointer is
zero-initialised by BSS, so no explicit `readline_init0()` is needed.

**Things deliberately left alone:**

- Tab completion is enabled implicitly via `MICROPY_HELPER_REPL` and
  works for top-level identifiers; nothing port-specific to do.
- Persistent history (`S:micropython_history` on disk) is not
  implemented; it's nice-to-have and easy to bolt on later via
  `readline_push_history` calls at startup.
- Multi-line paste relies on terminal bracketed-paste, which the AmigaOS
  console doesn't support; pasting indented code into the REPL will
  still misbehave the same way it does on serial consoles for other
  ports. Document, don't fix.
- The 200 ms `WaitForChar` poll in earlier drafts was removed in Phase 8;
  `FGetC` blocks efficiently and Ctrl+C is caught either by the VM hook
  or by `mp_interrupt_char` in `mp_hal_stdin_rx_chr`.

#### Running the REPL interactively under vamos

`SetMode(stdin, 1)` puts the AmigaOS console into raw mode under
Amiberry / real hardware, but vamos doesn't translate that into a
`tcsetattr()` on the host TTY. On a default macOS Terminal (or any
cooked-mode shell) the result is that cursor keys echo as `^[[D` and
read() doesn't deliver bytes to vamos until Enter — so `readline.c`
never sees the escape sequence and there's no editing.

Use `tools/amiga-vamos-repl.sh` for interactive vamos sessions. It puts
the host TTY into `-icanon -echo -isig` (raw, no echo, byte-at-a-time,
let the binary handle ^C/^D itself) for the duration of the run and
restores the original mode on exit:

```sh
tools/amiga-vamos-repl.sh           # start the REPL (standard variant)
tools/amiga-vamos-repl.sh script.py # run a script (host TTY stays raw,
                                    # so any input() prompts get raw mode too)

AMIGA_VARIANT=68040 tools/amiga-vamos-repl.sh   # FPU-capable build
AMIGA_VARIANT=a1200 tools/amiga-vamos-repl.sh   # minimal build
```

`AMIGA_VARIANT` picks which `build-<variant>/micropython` to launch and
the appropriate vamos `--cpu` flag (same mapping as
`amiga-vamos-run.sh` — see the testing section). `68020fpu` can't run
under vamos because vamos has no 68881 emulation.

Pipe input (`printf ... | vamos ...`) is unaffected and doesn't need the
wrapper, because pipes never get cooked-mode line buffering.

---

### Phase 14 — Dynamic heap growth and runtime sizing

Today the GC heap is a single 256 KB chunk allocated once at startup in
`main.c` via `AllocVec(MICROPY_HEAP_SIZE, MEMF_FAST|MEMF_PUBLIC)` (with
`MEMF_ANY` fallback). That's fine for a 1 MB chip-RAM A500, but on a
machine with 16+ MB of Fast RAM we leave most of it on the table; once
the 256 KB is full the port raises `MemoryError` instead of expanding.
Goals for this phase:

> Note on allocation flags: use `MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR` for
> both the initial alloc and any subsequent growth chunks. AmigaOS's
> free memory list is ordered fastest-first, so `MEMF_ANY` already
> prefers Fast RAM when it's available and degrades gracefully to
> Chip RAM on a stock A500/A1000/A2000 that has no Fast at all.
> `MEMF_CLEAR` zeroes the chunk so neither `gc_init()` nor `gc_add()`
> can see stale bits from a previous task. The existing main.c heap
> allocation switched to this combination on 2026-05-11 to match.

1. **Runtime initial heap size.** Replace the compile-time
   `MICROPY_HEAP_SIZE` with a value derived at startup. Sources, in
   priority order:
     - `-X heap=<N>[K|M]` on the command line (matches what other ports
       accept; `main.c` already parses `-X` for `compile-only` etc.).
     - `MICROPYHEAP` env var (read via `GetVar()` from dos.library).
     - A sensible default (probably still 256 KB) so out-of-the-box
       behaviour is unchanged for users not configuring anything.

2. **Dynamic growth via `gc_add()`.** When `gc_alloc` can't satisfy a
   request after a collection, ask `AllocVec` for another chunk
   (typically 128–256 KB) and feed it to the GC via `gc_add(start,
   end)`. MicroPython supports multiple non-contiguous GC arenas
   natively, so this is just plumbing.
   Track each chunk in a small linked list of `BPTR`s so we can
   `FreeVec` them all in `mp_deinit()` at exit.

3. **Growth policy.** Don't grow on every collection — only after a
   full collection still leaves the heap above some occupancy
   threshold (e.g. >80 %). Cap the total via a `-X maxheap=<N>`
   option so a runaway script can still be stopped without thrashing
   the system. On the failure to grow (out of Fast RAM, hit the cap)
   fall back to the standard `MemoryError` path.

4. **Reporting.** Extend `amiga.os_version()`-style introspection with
   `amiga.heap_info()` returning a tuple of `(total_bytes,
   free_bytes, num_arenas)`. Useful for the user to tune `-X heap` /
   `-X maxheap` for their workload.

Implementation lives in `main.c` (parsing, initial allocation, the
chunk list, `mp_deinit` cleanup) and a small helper hook from
`gc.c`-callers — MicroPython already calls a port-provided
`gc_helper_get_regs_and_sp` etc., but the natural extension hook is
`gc_collect_root_with_alloc_fail` or similar; failing that, the port
overrides `mp_alloc_failed_oom` via a port-local wrapper.

Open questions to settle before starting:

- Should chunks be released when the heap drops back below some low-
  water mark? Probably not — `FreeVec` on a `gc_add`'d region needs
  the region to be empty of live objects, which the GC can't guarantee.
  Just keep grown chunks until exit.
- What `MEMF_*` mask should grown chunks use? `MEMF_ANY|MEMF_PUBLIC`,
  same as the initial allocation under this plan. AmigaOS already
  hands out the fastest available region first; chip-RAM-only
  machines (stock A500/A1000/A2000) simply can't satisfy `MEMF_FAST`
  so insisting on it would close those configurations off entirely.

Acceptance: a build with `MICROPY_HEAP_SIZE` at its default plus
`-X heap=2M` can compile and execute a script that allocates well
beyond 256 KB; `amiga.heap_info()` shows the heap growing across
multiple arenas; rebooting after the run frees all chunks cleanly
(verified under vamos's orphan-memory check).

---

### Phase 15 — AmigaOS memory-pool API in the `amiga` module

`exec.library` provides a memory-pool allocator (`CreatePool`,
`AllocPooled`, `FreePooled`, `DeletePool`) that's well suited to
workloads with many small short-lived allocations sharing a lifetime
— parser AST nodes, per-request scratch buffers, level-loaded game
state, etc. The pool grows in "puddles" of a configurable size and
`DeletePool` reclaims everything in one call without needing to
track individual pointers. We already expose `AllocVec` /
`FreeVec` (Phase 4); this phase rounds out the resource module with
the pool API.

Goals for this phase:

1. **C bindings in `ports/amiga/modamiga.c`.** Mirror the existing
   `alloc_vec` / `free_vec` style:

   ```python
   pool = amiga.create_pool(flags=amiga.MEMF_ANY,
                            puddle_size=4096,
                            threshold_size=1024)
   addr = amiga.alloc_pooled(pool, size)
   amiga.free_pooled(pool, addr, size)
   amiga.delete_pool(pool)
   ```

   Pool handles are returned as Python ints (matching `alloc_vec`'s
   integer-address convention) since `BPTR`-style opaque handles
   would require a new type and finaliser for cleanup. Users keep
   the int in a variable and pass it back; on `MemoryError` from
   `CreatePool` we raise the standard exception.

2. **Defaults that match common AmigaOS usage.**
     - `flags` defaults to `MEMF_ANY` (matches `alloc_vec`).
     - `puddle_size` defaults to 4 KB — large enough for many small
       allocs without wasting a lot on the first puddle.
     - `threshold_size`: any single request ≥ this bypasses the
       pool and goes straight to `AllocVec`. AmigaOS docs recommend
       setting this to roughly half the puddle size; default 2 KB.

3. **Light Python-level wrapper (optional).** A small
   `class MemoryPool` in `ports/amiga/modamiga.c` (or shipped as a
   frozen Python module) that wraps the four C calls in a context
   manager so `with amiga.MemoryPool() as p: ...` auto-deletes on
   exit. This keeps the low-level functions exposed for users who
   want them while making the common case easier.

4. **Documentation in the `Phase 4 — amiga module API` table.**
   Extend the API summary in this doc and the inline qstr table
   with the new symbols (`Q(create_pool)`, `Q(alloc_pooled)`,
   `Q(free_pooled)`, `Q(delete_pool)`, `Q(MemoryPool)` if we ship
   the wrapper class). `MEMF_*` constants are already exported.

Implementation notes:

- `CreatePool` etc. were added in OS 3.0 (`exec.library` v37); the
  port already requires `-m68020` + clib2 / NDK from bebbo so this
  is a no-op compatibility-wise. Add a runtime guard via
  `SysBase->LibNode.lib_Version >= 39` just in case (matches the
  pattern used for `GetProgramName` in `main.c`).
- Pool handles are opaque void pointers; we cast to `mp_int_t` for
  Python and back to `APTR` for `Alloc/FreePooled`. The Phase 4
  pattern for `alloc_vec` does the same with `AllocVec` results.
- `FreePooled` requires the original `size` argument (AmigaOS pools
  don't track per-allocation sizes); make this a hard parameter
  on the Python side rather than trying to remember it ourselves.
- The optional `MemoryPool` wrapper class should call
  `delete_pool` in `__del__` (`MICROPY_ENABLE_FINALISER` is already
  on for Phase 2's file handles), with a `closed` flag so an
  explicit `delete_pool()` followed by GC doesn't double-free.

Open questions to settle before starting:

- Do we add bounds-checking on `alloc_pooled` returns / `free_pooled`
  args, or trust the user? The existing `alloc_vec` / `free_vec`
  trust the user; pools should match for consistency. The cost of
  a stray `FreePooled` is a crash, but that's already true of the
  current `free_vec`.
- Should pool handles be registered as GC roots so a forgotten
  pool gets collected? Probably not — pools are an *explicit*
  resource, and the optional `MemoryPool` wrapper covers the
  common "I forgot to free" case via its finaliser.

Acceptance: a script can create a pool, allocate from it a few
thousand times, optionally `free_pooled` some entries, and
`delete_pool` cleanly with no memory leak under vamos's
orphan-memory check. The wrapper class also works as a
`with`-statement context manager.

---

### Phase 16 — Pythonic file I/O via MicroPython VFS ✅

Phases 2 and 8 wired up file I/O via `dos.library` directly: a
port-local `mp_builtin_open` in `amigaio.c` returned `FileIO` /
`TextIOWrapper` stream objects, and `amigafile.c` implemented
`mp_lexer_new_from_file` + `mp_import_stat` from the same primitives.
This worked for `open()` and `import`, but the standard `os.*` file
operations (`os.chdir`, `os.getcwd`, `os.listdir`, `os.stat`,
`os.mkdir`, `os.remove`, `os.rename`, `os.rmdir`) were unavailable
because those route through MicroPython's VFS layer when
`MICROPY_VFS=1`, and the port didn't enable it.

Phase 16 enables `MICROPY_VFS=1` and `MICROPY_READER_VFS=1`, and
ships a port-local `VfsAmiga` type in `ports/amiga/vfs_amiga.c` that
implements every VFS method by wrapping the corresponding
`dos.library` call. The VFS object itself is stateless — AmigaDOS
keeps the cwd in `pr_CurrentDir`, so the VFS just dispatches into
`Lock` / `Examine` / `CurrentDir` / `Open` / `Read` / `Write` /
`CreateDir` / `DeleteFile` / `Rename`. `main.c` mounts a single
`VfsAmiga` instance at `/` at startup; the VFS lookup treats
AmigaOS-style paths ("`volume:dir/file`" or relative) as
"relative-on-current-VFS" and routes them straight to us.

Files moved or removed:

| File | Status |
|------|--------|
| `vfs_amiga.c` | **New.** Houses the `VfsAmiga` type, the `FileIO`/`TextIOWrapper` stream objects (previously in `amigaio.c`), the VFS `import_stat` protocol callback, and an `ilistdir` iterator with a finaliser so abandoned `for f in os.listdir(...)` loops don't leak the directory `Lock`. |
| `amigafile.c` | **Deleted.** `mp_lexer_new_from_file` is now provided by `extmod/vfs_reader.c` (via `MICROPY_READER_VFS`); `mp_import_stat` is an inline in `py/builtin.h` that delegates to `mp_vfs_import_stat`. |
| `amigaio.c` | **Deleted.** File-type definitions and `mp_builtin_open` moved into `vfs_amiga.c`. With `MICROPY_VFS=1`, `mp_builtin_open_obj` is aliased to `mp_vfs_open_obj`, which dispatches to `VfsAmiga.open`. |

The `dos.library` requester suppression pattern from `amiga.exists`
(save `pr_WindowPtr`, set to `-1`, do the `Lock`, restore) is reused
around every `Lock` call so a path on an unmounted volume reports a
clean `OSError(ENOENT)` instead of stalling on a `"Please insert
volume X:"` dialog.

`os.chdir` keeps the first inherited cwd lock around (the one
returned by `CurrentDir` on the first call — it's the shell's lock,
not ours to free) and `UnLock`s any subsequent ones it gets back as
we walk between directories. Children spawned via `amiga.execute()`
inherit cwd from `pr_CurrentDir`, so the test runner can `os.chdir`
once into the test directory and every spawned `micropython`
subprocess sees relative `open("data/file")` paths resolve correctly.

`tools/amiga-runtests.py` switched to `os.chdir(test_dir)` instead
of the previously-considered `amiga.chdir` shim — same effect, but
uses the standard Python idiom. The previously-failing `io/*` tests
that all opened `data/file1` relative to cwd now pass.

Verified under vamos:

- `os.getcwd()` returns the volume-prefixed cwd string (e.g.
  `"mp:basics"`, where `mp:` is the internal vamos-side mount of
  `tests/` set up by `tools/amiga-vamos-run.sh`).
- `os.chdir("mp:")` / `os.listdir("basics")` work.
- `os.stat("basics/bool1.py")` returns a 10-tuple with mode/size set.
- `open("nonexistent", "r+b")` raises `OSError(ENOENT)` — Python's "r+"
  semantic (fail if missing) is correctly mapped to AmigaOS
  `MODE_OLDFILE` rather than `MODE_READWRITE` (which would
  create-on-missing).
- Full `basics/` regression: 490 / 1 / 83 unchanged from the
  pre-Phase-16 baseline.
- Full `io/` regression: 15 pass / 1 skip / 0 fail, up from the pre-
  Phase-16 12 pass / 3 skip / 1 fail. (The previously-failing
  `io/argv.py` and `import/import_file.py` now pass too: the runner
  invokes each test by basename with cwd set, so `sys.argv[0]` and
  `__file__` come out path-prefix-free on both the host and the
  Amiga side.)

---

### Phase 17 — Native AmigaOS library access

Today the only way to call a system library from Python is for the port to
hand-roll a C wrapper in `modamiga.c` (current set: `os_version`, `find_task`,
`alloc_vec`, `free_vec`, `execute`, `exists`). That's fine for a handful of
bindings, but it doesn't scale to the dozens of system libraries that make
AmigaOS *AmigaOS* — let alone third-party libraries. This phase adds a generic
library-call mechanism that exposes any AmigaOS library function — system or
third-party — to Python with no port-side C changes per library.

#### Why this is tractable on AmigaOS

The AmigaOS calling convention is unusually regular:

- Every library function lives at a negative offset (LVO) from the library
  base pointer.
- Args go in a fixed set of D/A registers — never on the stack.
- Return value is always in D0.
- The library base must be in A6 before the call.
- No struct-in-register passing, no varargs at the ABI level, all values are
  32-bit.

So one small 68k assembly trampoline can call any function in any library,
given a register descriptor.

The signature data already exists in machine-readable form: the NDK ships
a `.fd` (Function Definition) file for every library at
`/opt/amiga/m68k-amigaos/ndk-include/fd/*.fd`:

```
##base _IntuitionBase
##bias 30
OpenWindow(newWindow)(A0)
CloseWindow(window)(A0)
...
##end
```

That's enough to mechanically look up call signatures (the bias is added to
each function's index × 6 to compute the LVO). Vamos's `amitools` already
parses these in Python — there's prior art to borrow.

#### Two-layer design

**Layer 1 — low-level trampoline (in `modamiga.c` + inline asm)**

```python
base = amiga.lib_open("intuition.library", 39)    # OpenLibrary
addr = amiga.lib_call(base, -204,                 # OpenWindow's LVO
                     a0=newwin_ptr,
                     ret="d0")
amiga.lib_close(base)                             # CloseLibrary
```

The trampoline takes the library base, an offset, and a register descriptor
(which Dn/An registers to load and with what value). It saves A6, loads the
base into A6, loads the requested registers, executes `jsr (offset, a6)`,
restores A6, returns D0. Roughly 30 lines of inline 68k assembly. This
layer alone unlocks every library on the system.

API:

- `amiga.lib_open(name, version)` → `OpenLibrary(name, version)`, returns
  int base or raises `OSError(ENOENT)` on failure.
- `amiga.lib_close(base)` → `CloseLibrary(base)`.
- `amiga.lib_call(base, offset, **regs, ret="d0")` → set up registers, call,
  return D0 (interpreted per `ret=`: `"d0"` signed int, `"d0u"` unsigned,
  `"void"` to ignore).
- Register kwargs: `d0`–`d7`, `a0`–`a5` (A6 is reserved for the base; A7 is SP).

**Layer 2 — `.fd`-driven proxy (pure Python, frozen module)**

```python
intuition = amiga.library("intuition.library", 39)
win = intuition.OpenWindow(nw)                   # signature looked up from .fd
intuition.CloseWindow(win)
```

`amiga.library(name, version)` opens the library, looks up its `.fd`
signature table, and returns a `LibraryProxy` whose `__getattr__` builds the
right `lib_call` invocation. Frozen Python module on top of layer 1; no
port-side C beyond what layer 1 already provides.

#### Shipping `.fd` data

**System libraries.** A build-time tool (`tools/amiga-fdgen.py`) parses every
`.fd` in the NDK and emits a compact frozen Python dict mapping
`library_name → {function_name: (offset, regs_in, ret_type)}`. The result is
frozen into the binary so there's no startup parse cost and no runtime
dependency on the NDK being installed at the target.

**Third-party libraries.** At runtime, `amiga.library(name)` falls back to
parsing `.fd` files from a search path: `PROGDIR:fd/`, `LIBS:fd/`,
`mp_lib_fd/`. User drops `mylibrary.fd` next to their script and
`amiga.library("mylibrary.library", 1)` finds it. Zero rebuild required.

Or programmatically, when no `.fd` is available:

```python
mylib = amiga.library_from_signatures(
    "mylibrary.library", 1,
    {"DoStuff": (-30, "a0,d0", "d0")})
```

#### Things to deal with

1. **Type marshalling.** Most arg slots are LONG (int → register, free).
   Strings need `bytes`/`str` → `char *`: encode to latin-1 (Amiga charset),
   ensure NUL-termination, hand over the buffer pointer. MicroPython's GC is
   non-moving, so we don't need to pin the buffer during the call. Pointers
   are Python ints (matches the `alloc_vec` convention).

2. **Tag lists.** Pervasive on OS3.x (`OpenWindowTagList`, `EasyRequestArgs`,
   `OpenScreenTagList`...). Helper:
   `tags = amiga.taglist(WA_Width=640, WA_Height=400, WA_Title="hi")` returns
   a pointer to a `TAG_DONE`-terminated array allocated from a per-call
   arena. Lifetime tied to the next library call.

3. **Structures.** Two layers: `amiga.peek_l(addr, offset)` / `peek_w` /
   `peek_b` / `poke_*` as the dumb primitive, then a tiny ctypes-lite for
   users who want `Window(ptr).Width`. The struct layouts live in `clib/*.h`
   — much harder to mechanically parse than `.fd`, so hand-curate the common
   ones (Window, Screen, RastPort, IntuiMessage, Process, Task, DateStamp,
   FileInfoBlock) and let users define their own with a layout DSL.

4. **Callback hooks.** Some library functions (`CreateNewProc`, hook-driven
   blits, ARexx command dispatchers) want a C function pointer the library
   will call back. Hard: needs executable memory and a per-callable thunk.
   Defer this; most OS3.x scripting doesn't need it, and the few cases that
   do (ARexx ports being the main one) are better served by a dedicated
   module (see Phase 18).

#### Implementation order within this phase

1. `lib_open` / `lib_close` / `lib_call` with the asm trampoline + a tag-list
   helper. Smoke test: reopen `intuition.library` and call `DisplayBeep(0)`.
2. Frozen `.fd` table generator (`tools/amiga-fdgen.py`) for NDK libraries.
3. `amiga.library(name, version)` proxy on top.
4. Third-party `.fd` search path.
5. Minimal struct helpers (peek/poke + hand-curated Window/Screen/RastPort).
6. (Later) callback thunks if anyone actually needs them.

Steps 1–3 should land as a single deliverable — that's "anything in the NDK,
callable from Python" for roughly 400 lines of C+asm+Python.

#### Acceptance

```python
import amiga
intuition = amiga.library("intuition.library", 39)
intuition.DisplayBeep(0)         # screen flashes / speaker beeps
```

Equivalent works for `exec.library`, `dos.library`, `graphics.library`,
`gadtools.library`, `asl.library`, `icon.library` without any port-side code
changes. A user-supplied `mylibrary.fd` dropped in `PROGDIR:fd/` makes its
functions callable the same way.

Note: once Phase 17 lands, several later phases collapse to small pure-Python
wrappers — Phase 21 (volumes/assigns) and Phase 22 (pattern matching) become
trivial `amiga.library("dos.library", ...)` calls, and an `EasyRequest`
helper drops to a one-liner instead of a port-side C binding. Land Phase 17
first if any of these are on the immediate horizon.

---

### Phase 18 — ARexx integration

ARexx is *the* Amiga IPC mechanism: every well-behaved Amiga application
exposes an ARexx port that external scripts can drive. Without one,
MicroPython feels foreign on AmigaOS. Two halves:

1. **Outbound client.** `amiga.rexx("DOPUS.1", "lister new")` posts a
   command to another app's ARexx port and waits for the result. Pure
   send-and-wait; doesn't require an inbound port. Implemented via
   `rexxsyslib.library` `CreateRexxMsg` / `FillRexxMsg` / `PutMsg` to the
   target port plus a wait on the reply.

2. **Inbound port.** Open `MICROPYTHON.1` (or `.2`, `.3`... if multiple
   instances are running) via `CreateMsgPort` + `AddPort`. External scripts
   can send eval-this-string commands and get results back. The interpreter
   services the port between bytecode batches (the existing
   `MICROPY_VM_HOOK_LOOP` is the right hook, alongside the Ctrl+C poll).

API sketch:

```python
import amiga

# Outbound
result = amiga.rexx("PPAINT.1", "ScreenToFront")

# Inbound (cooperative; in the REPL or a long-running script)
amiga.rexx_open("MICROPYTHON")    # opens MICROPYTHON.1
# external 'rx "address MICROPYTHON.1; ..."' commands get eval'd in this
# interpreter's globals, with their result returned to the caller
amiga.rexx_close()
```

Outbound is the easier and more broadly useful half — every Amiga app
becomes scriptable from a Python one-liner. Inbound is what makes
MicroPython itself a good Amiga citizen.

Acceptance: from the AmigaShell, `rx "address MICROPYTHON.1; '2 + 2'"`
returns 4. From Python, `amiga.rexx("WORKBENCH", "WINDOWTOFRONT")` brings
Workbench's window forward.

---

### Phase 19 — Workbench launch and tooltypes

When an Amiga executable is launched from Workbench (by double-clicking its
`.info` icon) instead of from a Shell, it gets a `WBStartup` message in
place of `argc`/`argv`, and its configuration comes from tooltypes stored
in the `.info` file. Today the port treats Workbench launch as "zero args"
— the script just exits immediately because there's no REPL TTY either.

Two pieces:

1. **Detect Workbench launch.** When `pr_CLI == 0`, the process was started
   from Workbench. The `WBStartup` message arrives on the process's port;
   it carries the path to the executable plus zero or more "selected" file
   arguments (icons shift-clicked alongside).

2. **Read tooltypes.** `icon.library` `GetDiskObject(exe_name)` returns the
   parsed `.info`; `FindToolType(do->do_ToolTypes, "SCRIPT")` etc. extract
   individual settings. Conventional tooltypes: `SCRIPT=Work:scripts/foo.py`,
   `HEAP=2M`, `DEBUG=1`.

API sketch:

```python
import amiga
if amiga.launched_from_workbench():
    script = amiga.tooltype("SCRIPT")
    heap   = amiga.tooltype("HEAP")
    for arg in amiga.wb_selected_files():
        ...
```

Also ship `tools/amiga-mkicon.py` that generates a companion `.info` file
for a frozen-script binary — drops on the desktop, double-clickable, with
tooltypes ready for the user to edit.

Open question: how should stdout/stderr behave for a Workbench-launched
script? Two options — open a `CON:` window automatically (so `print()`
appears somewhere visible), or require the user to call
`amiga.open_console("CON:0/0/640/200/MicroPython")` explicitly. The former
is friendlier; the latter is what most Amiga apps do.

Acceptance: double-clicking a `micropython.info` icon on the Workbench
launches the interpreter; the `SCRIPT=` tooltype gets executed; output
appears in an auto-opened (or explicitly opened) CON: window.

---

### Phase 20 — env-var integration via `os.getenv`/`os.putenv`/`os.unsetenv` ✅

AmigaDOS environment variables live in a real shared store: `ENV:` is a
filesystem assign, every variable is a file under it, and `SetVar` /
`GetVar` / `DeleteVar` from `dos.library` are the API. Crucially this is
the *same* store the shell uses — that's not true on Unix (child processes
inherit copies of the parent's environment).

MicroPython doesn't expose a CPython-style `os.environ` dict; the actual
API surface is the three functions `os.getenv` / `os.putenv` /
`os.unsetenv`, gated by `MICROPY_PY_OS_GETENV_PUTENV_UNSETENV`. The port
enables that flag and supplies the three function bodies in
`ports/amiga/modos.c`, included by `extmod/modos.c` via
`MICROPY_PY_OS_INCLUDEFILE`. The end result:

```python
import os
os.putenv("EDITOR", "Ed")
print(os.getenv("EDITOR"))            # 'Ed'
print(os.getenv("MISSING", "n/a"))    # 'n/a' default
os.unsetenv("EDITOR")
print(os.getenv("EDITOR"))            # None
```

#### Local vs global

All three calls pass `flags=0` (no `GVF_GLOBAL_ONLY` / `GVF_LOCAL_ONLY`).
On real AmigaOS this means:

- `getenv` looks up local CLI vars first, falling through to global
  (`ENV:`).
- `putenv` creates or replaces a *local* CLI variable. Inherited by
  child processes launched via `amiga.execute()`; not visible to other
  unrelated shells.
- `unsetenv` removes the local variable.

This matches Unix `os.putenv` semantics ("affects this process and its
children"). Users wanting a system-wide variable visible to *all* shells
can write directly to `ENV:` (it's just a filesystem assign), and for
persistence across reboots also write the same value to `ENVARC:`:

```python
with open("ENV:MY_VAR", "w") as f:        # visible to other shells now
    f.write("value")
with open("ENVARC:MY_VAR", "w") as f:     # restored on reboot
    f.write("value")
```

#### Vamos workarounds

Two bugs in `amitools.vamos.lib.DosLibrary` had to be worked around:

1. **`GetVar` returns 0 (instead of -1) for a missing variable.** That
   makes the return value alone unable to distinguish *missing* from
   *empty value*. We treat `len <= 0` as missing — correct on vamos for
   the common case, but on real AmigaOS this misreports a genuine
   empty-string variable as missing. (`FindVar` would be the obvious
   existence test, but vamos's `FindVar` hits an `AttributeError`
   `'AccessStruct' object has no attribute 'struct_addr'` in its
   success-path logging.)

2. **`DeleteVar` reads the `flags` argument from `D4`** but the NDK fd
   (`/opt/amiga/m68k-amigaos/ndk/lib/fd/dos_lib.fd`) specifies `D2`.
   With stale data in `D4`, vamos can spuriously take the
   `GVF_GLOBAL_ONLY` branch and silently skip the deletion. We use the
   V36-documented "`SetVar` with `NULL` buffer deletes the variable"
   form instead — vamos reads `SetVar`'s registers correctly.

Both worth fixing upstream in vamos eventually; both behave correctly
on real AmigaOS where `DeleteVar` and `GetVar` follow the documented
contract.

Implementation: `ports/amiga/modos.c` (three function bodies);
`mpconfigport.h` adds `MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (1)` and
`MICROPY_PY_OS_INCLUDEFILE "ports/amiga/modos.c"`. No Makefile changes —
the file is `#include`d by `extmod/modos.c`, not compiled separately.

Acceptance: round-trip `putenv` → `getenv` → `unsetenv` works under
vamos; the `basics/` test suite still passes 490/491 (same as the
pre-Phase-20 baseline — the one `struct1.py` failure is pre-existing
and unrelated).

---

### Phase 21 — Volume and assign introspection

Surface the AmigaDOS device list — mounted volumes, assigns, and free-space
info — to Python. Walks `DosList` via `dos.library` `LockDosList` /
`NextDosEntry` / `UnLockDosList`, and `Info()` for per-volume stats.

```python
import amiga
amiga.volumes()         # → ['Work:', 'Workbench:', 'RAM:']
amiga.assigns()         # → {'LIBS:': 'Workbench:Libs',
                        #    'C:':    'Workbench:C', ...}
amiga.disk_info("Work:")
# → (free_bytes, total_bytes, block_size)
```

Trivial on top of Phase 17 (a handful of `dos.library` calls), or hand-written
as port-side C if Phase 17 isn't there yet — but the post-Phase-17 version is
a ~50-line frozen Python module.

---

### Phase 22 — AmigaDOS pattern matching

AmigaDOS patterns (`#?.py`, `~(#?.bak)`, `[a-z]#?`) are richer than POSIX
glob and what users at the AmigaShell already know. Expose `dos.library`
`ParsePattern` / `MatchFirst` / `MatchNext` / `MatchEnd`:

```python
import amiga
for path in amiga.match("Work:src/#?.py"):
    print(path)
```

Generator form (`amiga.imatch`) for memory-efficient iteration over large
match sets — useful since `MatchFirst` builds an `AnchorPath` that holds
real file `Lock`s during iteration.

Also trivial post-Phase-17 — pure-Python wrapper around `dos.library` calls.

---

### Phase 23 — `timer.device`-backed timing

The Phase 8 section notes that `mp_hal_delay_us` and `mp_hal_ticks_*` still
use newlib `clock()` — busy-wait, millisecond-ish resolution. AmigaOS's
`timer.device` MICROHZ unit gives microsecond accuracy and doesn't
busy-wait. Replace the `clock()`-based path with `timer.device` for:

- `mp_hal_delay_us(n)` → `timer.device` `TR_ADDREQUEST` with
  `tv_secs=0, tv_micro=n` (or a tight busy-loop for `<100µs`, where the
  IORequest setup overhead dominates).
- `mp_hal_ticks_us()` / `mp_hal_ticks_ms()` → `ReadEClock()` (uses the
  EClock hardware counter — sub-microsecond, monotonic, cheap to read).
- `time.sleep_ms` / `sleep_us` get accuracy as a side effect since they
  route through these HAL hooks.

Setup: `CreateMsgPort` + `CreateIORequest` + `OpenDevice("timer.device",
UNIT_MICROHZ, ...)` once at startup; teardown in `mp_deinit`. The
IORequest object goes in a port-local static; not thread-safe but the port
is single-threaded.

Acceptance: `time.sleep_us(500)` actually waits ~500 µs (not 0 or 1000);
`time.ticks_diff` between two `ticks_us()` calls returns a stable
microsecond delta; the CPU isn't pinned at 100% during a long
`time.sleep`.

---

### Phase 24 — Persistent REPL history

Phase 13 explicitly punted persistent history. The implementation is
small: at startup, read `ENVARC:MICROPYTHON_HISTORY` (one line per entry,
oldest first) and `readline_push_history()` each line in turn; at shutdown,
walk the history ring and write it back out.

`mp_sys_atexit` (`MICROPY_PY_SYS_ATEXIT = 1`, already on) is the natural
hook for the write side. `MICROPY_READLINE_HISTORY_SIZE` is 8 today; bump
to 32 alongside this phase since disk storage costs nothing.

`ENVARC:` rather than `ENV:` so history survives a reboot —
`copy ENVARC: ENV: all` at startup is the standard AmigaOS pattern and
`ENVARC:` is what the user's S-S preferences for other apps already live in.

---

### Phase 25 — Extra break signals

AmigaOS gives every task four user-defined break signals: `SIGBREAKF_CTRL_C`,
`_D`, `_E`, `_F`. Today only Ctrl+C is wired up. Exposing the others gives
Python scripts a cheap cooperative IPC primitive — one task signals
another's task pointer, no port / message setup needed.

```python
import amiga
amiga.signal(other_task_addr, amiga.SIGBREAKF_CTRL_E)
mask = amiga.wait_signal(amiga.SIGBREAKF_CTRL_D | amiga.SIGBREAKF_CTRL_E,
                         timeout_ms=5000)
```

Implementation: `exec.library` `Signal()` / `Wait()` / `SetSignal()` —
small enough to bind directly in `modamiga.c`, no need to wait on
Phase 17. Combine with `amiga.find_task(name)` (already exposed) for
named-task signalling.

The wait wrapper has to be careful with Ctrl+C: any `Wait()` must OR in
`SIGBREAKF_CTRL_C`, and a returned mask containing Ctrl+C should raise
`KeyboardInterrupt` rather than being delivered to the script. Otherwise
a script blocked in `wait_signal` becomes uninterruptible from the
console.

---

### Phase 26 — `PROGDIR:` on `sys.path`

Standard Amiga pattern: the executable's own directory is referenced as
`PROGDIR:` (an automatic assign created by AmigaDOS at launch and unique
per process). Put `PROGDIR:` on `sys.path` automatically at startup so a
user can drop `.py` / `.mpy` files next to the `micropython` binary and
import them with no path setup.

One-line change in `main.c`'s `sys.path` initialization: insert
`"PROGDIR:"` before the existing `""` entry. AmigaDOS resolves the assign
itself, so the rest of the import path stack just works.

---

### Phase 27 — Build variants (low-memory + FPU) ✅

The Makefile honours `VARIANT=<name>`, with the build directory named
`build-<variant>`. Each variant lives under `ports/amiga/variants/<name>/`
and supplies an `mpconfigvariant.h` (overrides like MCU name, heap size,
disabled modules) plus an `mpconfigvariant.mk` (CPU flags, soft-float
linker wraps). `mpconfigport.h` includes `mpconfigvariant.h` early and
`#ifndef`-guards the overridable defaults.

Four variants ship:

| Variant | CPU | Heap | Notes |
|---------|-----|------|-------|
| `standard` (default) | `-m68020 -msoft-float` | 256 KB | Current behaviour preserved; runs on any 68020+ Amiga. |
| `a1200` | `-m68020 -msoft-float` | 128 KB | Stock unaccelerated A1200 (68EC020, 2 MB Chip, no Fast). Strips `bsdsocket` module and `MICROPY_EMIT_68K`. Note: A500 isn't a target — its 68000 lacks unaligned access. |
| `68020fpu` | `-m68020 -m68881` | 512 KB | 68020/68030 with 68881/68882 FPU coprocessor (A2620/A2630, A3000). FPU compares and conversions are emitted directly, so the libgcc soft-float wraps from `floatconv.c` aren't applied. |
| `68040` | `-m68040` | 1 MB | 68040 with built-in FPU (A3640, A4000/040, Blizzard 1240). Same float-wrap story as `68020fpu`. |

The libm `pow` / `tgamma` wraps in `floatconv.c` apply unconditionally on
all variants — those are math-library bugs in clib2/libnix, unrelated to
FPU codegen.

Build sizes (text segment, `size` output):

```
standard   355 848 bytes
a1200      327 080 bytes   (-29 KB: no socket, no native emitter)
68020fpu   329 740 bytes   (-26 KB: no libgcc soft-float helpers)
68040      339 012 bytes
```

FPU variants require the bebbo toolchain to have FPU multilibs built for
libgcc/clib2 (standard with `make all`). If `m68k-amigaos-gcc -m68881
-print-multi-lib` shows only a soft-float entry, rebuild bebbo with the
FPU multilibs enabled or the link step will fail with `cannot find -lgcc`.

**Why `68040` is larger than `68020fpu`.** The 68040's on-die FPU is a
strict subset of the 68881/68882 — Motorola dropped the transcendental
instructions (`FSIN`, `FCOS`, `FTAN`, `FATAN`, `FACOS`, `FASIN`, `FSINH`,
`FCOSH`, `FTANH`, `FETOX`, `FLOGN`, `FLOG10`, `FLOG2`, `FTENTOX`, plus
some packed-decimal and rounding variants) to save silicon. With
`-m68040`, gcc knows the chip can't execute these instructions, so it
emits libm function calls instead of single-instruction inlines.
clib2's software `sin`/`cos`/`tan`/`log`/`exp`/`pow` then get pulled in
(~9 KB). With `-m68020 -m68881`, gcc emits one-instruction `FSIN` etc.
inline and the libm functions shrink to roughly "FPU op; return".

An alternative for `68040` would be `-m68040 -m68881`: tell gcc the
68881 ISA is available, get the smaller binary back, and rely on
AmigaOS's **FPSP** (Floating Point Software Package — `68040.library`,
shipped with Workbench 3.0+) to trap-and-emulate the missing
transcendentals. We chose `-m68040` alone deliberately: a binary that
uses `FSIN` on a 68040 without the FPSP loaded (custom Workbench,
stripped Startup-Sequence, certain demos that take over the machine)
faults with a guru. The ~9 KB extra is the price of self-containment.

`sys.implementation._machine` reports the variant CPU: `"Amiga with
68EC020"` / `"Amiga with 68020/68881"` / `"Amiga with 68040"` etc., so a
script can branch on hardware capability at runtime.

Open items deferred to later phases:

- A `60` (or `68060`) variant. `-m68060` requires bebbo to have a 68060
  multilib; the 060 also faults on some 040 instructions in user mode
  (handled by emulation in `cpu060.library` on real hardware) so the
  variant has to set `-mno-unaligned-access` for portions of generated
  code. Punt until someone with the hardware confirms.
- Once Phase 14 (dynamic heap) lands, the per-variant `MICROPY_HEAP_SIZE`
  default becomes the *initial* size and users can override via
  `-X heap=...` at runtime.

---

### Other known limitations

| Issue | Status | Fix |
|-------|--------|-----|
| `try/except` in `@micropython.native` crashes | Known bug | Needs a 68k assembly NLR (`nlr68k.S`) that saves/restores D2–D7/A2–A5 in the `nlr_buf_t`; until then, avoid exceptions inside native functions |
| `@micropython.viper` limited to 1 register local | `MAX_REGS_FOR_LOCAL_VARS = 1` (D7 only) | Add a 68k-specific viper register allocator, or accept stack-based locals |

---

## Key Risks and Mitigations

| Risk | Mitigation |
|------|-----------|
| 68000 alignment traps | Target 68020+ (`-m68020`); 68000 unsupported for now |
| Low RAM (512 KB Chip only) | Use `MEMF_FAST` first; fall back to `MEMF_ANY`; keep `MICROPY_HEAP_SIZE` configurable |
| Stack overflow in deep recursion | Increase CLI stack via AmigaDOS `stack` command; add `MICROPY_STACK_CHECK (1)` |
| No 68k NLR assembly | Handled automatically — MicroPython falls back to `nlrsetjmp.c` |
| Cooperative multitasking | No issue for single-threaded REPL; `MICROPY_PY_THREAD (0)` |
| Endianness in `.mpy` files | Bytecode is endian-neutral; only relevant for native (phase 5) modules |
| clib2 missing POSIX APIs | Use dos.library equivalents once NDK installed; avoid assuming POSIX |

---

## Testing Strategy

1. **Emulator**: **Amiberry** confirmed working. FS-UAE and WinUAE are also options; all support 68020+ emulation and AmigaOS 3.x ROMs.
2. **CI**: Add `.github/workflows/ports_amiga.yml` — cross-compile on Linux with bebbo GCC; confirm build succeeds (no emulator run initially).
3. **Real hardware**: Final validation on a real Amiga (A1200, A2000+030 card, etc.) or MiSTer Amiga core.

---

## Running the Test Suite

With `micropython script.py` support (Phase 10), any test from `tests/` can be run
either **on the host via vamos** (fastest, no emulator GUI) or **inside Amiberry**
(closer to real hardware behaviour).

### Option A: vamos on the host (recommended for iteration)

`vamos` is a userspace 68k/AmigaOS emulator from the `amitools` package that
runs AmigaOS HUNK binaries directly on the host. It boots in milliseconds, has
no GUI, and integrates with `tests/run-tests.py`.

This setup assumes vamos is installed at `~/vamos/` and activated via `pipenv`
(the standard `amitools` install).

#### Smoke-test a single script

```sh
cd ~/vamos
pipenv run vamos --cpu 68020 \
    -V "mp:/path/to/micropython/tests" \
    --cwd mp:basics \
    -- /path/to/micropython/ports/amiga/build/micropython string1.py
```

Key flags:
- `--cpu 68020` — **required**; the vamos default is 68000, which faults on
  any `m68020` instruction emitted by the build.
- `-V name:/host/path` — mount a host directory as an AmigaOS volume named
  `name:`. The volume name is purely internal to vamos and never appears in
  any captured test output as long as you pass the test script by basename
  and use `--cwd` to point vamos at its directory.
- `--cwd <vol>:<subdir>` — sets the spawned process's cwd. Combined with
  basename invocation, this means `sys.argv[0]` / `__file__` come out as
  just the filename inside the test, exactly matching what host CPython
  produces in `tools/amiga-gen-exp.py`.
- `--` — separator between vamos options and the binary + args. Without it,
  vamos consumes `--version`, `-h`, `-c`, etc. as its own options.

A bare REPL works too:

```sh
pipenv run vamos --cpu 68020 -- /path/to/build/micropython
```

#### Wrapper script for run-tests.py

`tests/run-tests.py` invokes its target as `<MICROPY_MICROPYTHON> path/to/test.py`
from inside `tests/`. The wrapper at `tools/amiga-vamos-run.sh` mounts
`tests/` as an internal `mp:` volume (a name chosen to avoid colliding with
any conventional AmigaOS assign), points vamos's cwd at the test's
directory, and replaces the test-script argument with its basename. It also
uses a private `--vols-base-dir` so parallel workers don't collide on the
auto `RAM:` volume, and adds `-q` so vamos log lines don't pollute the
captured stdout/stderr that run-tests.py diffs against `.exp` files. The
salient parts:

```sh
amiga_args=""
test_cwd=""
for arg in "$@"; do
    case "$arg" in
        "$TESTS_DIR"/*.py)
            rel="${arg#$TESTS_DIR/}"
            d="$(dirname "$rel")"
            [ "$d" = "." ] && d=""
            test_cwd="mp:$d"
            amiga_args="$amiga_args $(basename "$arg")"
            ;;
        *) amiga_args="$amiga_args $arg" ;;
    esac
done

cd "$HOME/vamos"
exec pipenv run vamos -q --cpu 68020 -s 32 \
    --vols-base-dir "$VOLS_DIR" \
    -V "mp:$TESTS_DIR" \
    --cwd "$test_cwd" \
    -- "$MPY_BIN" $amiga_args
```

Save it executable (`chmod +x tools/amiga-vamos-run.sh`) and run:

```sh
export MICROPY_MICROPYTHON="$(pwd)/tools/amiga-vamos-run.sh"
cd tests
./run-tests.py -d basics float io micropython misc
```

Results land in `tests/results/`. Failures show a diff of expected vs. actual
output. Re-run only failures with `--run-failures`.

#### Testing non-default variants

`AMIGA_VARIANT` picks which `ports/amiga/build-<variant>/micropython` the
wrapper launches and selects the matching vamos `--cpu` flag and RAM
size:

```sh
AMIGA_VARIANT=a1200 ./run-tests.py -d basics    # --cpu 68020,  2 MiB RAM
AMIGA_VARIANT=68040 ./run-tests.py -d basics    # --cpu 68040,  4 MiB RAM
                                                # (vamos's 68040 has an FPU)
```

The `68020fpu` variant cannot be tested under vamos — it emits 68881
coprocessor instructions and vamos has no 68881 emulation. The wrapper
detects this and exits with a pointer at Amiberry / FS-UAE / real
hardware. For host-side regression on FPU codegen, use
`AMIGA_VARIANT=68040`, whose codegen is also FPU-using.

The default variant when `AMIGA_VARIANT` is unset is `standard`.

#### Exclude directories that can't run on Amiga

```sh
./run-tests.py -d basics float io micropython misc \
    -e "inlineasm|machine_|thread|extmod/ussl|extmod/uasync"
```

#### Test-runner integration requirements

For `tests/run-tests.py` (default `--test-instance unix`) to work the binary
must:

- accept `-X <option>` flags as no-ops (run-tests.py always emits
  `-X emit=bytecode` and on macOS adds `-X realtime`); `main.c` consumes them
  and skips the argument.
- return POSIX-style exit codes (0 on success). Set
  `MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)` in `mpconfigport.h` so
  `pyexec_file` returns 0 on a clean run instead of the embedded REPL's
  bitmask convention (which has 1 = normal exit).
- free its `AllocVec`'d argv buffers before returning, otherwise vamos's
  orphan-memory check makes the process exit non-zero even on success.

#### Known vamos quirks

- `pr_Arguments` is set correctly, but bebbo's C runtime argv parser
  produces broken pointers under vamos with multi-arg invocations. The port
  parses `pr_Arguments` itself in `amiga_parse_args` (`ports/amiga/main.c`),
  so this is invisible to test code.
- `WaitForChar` returns 0 immediately rather than blocking. The REPL uses
  plain `FGetC` (which vamos handles correctly), so this only matters if you
  reintroduce a `WaitForChar` poll loop.
- `SetMode(fh, 1)` (raw console mode) is logged as an unknown call and is a
  no-op; vamos already delivers characters one at a time.
- Quiet noisy logs with `-q` once you've confirmed setup; logs go to stderr
  and `run-tests.py` filters stderr cleanly.

### Option B: inside Amiberry

Use this for verifying that something works on real-AmigaOS-like behaviour
(timing, OS calls vamos doesn't fully implement, etc.) before flashing to
hardware.

#### Mounting the repo in Amiberry

In Amiberry's hard disk configuration add a single host-directory entry
that points at the repo root (so `tools/` and `tests/` are both
reachable from the same volume):

| Field | Value |
|-------|-------|
| Device | `PY0` |
| Volume label | `Py0` |
| Host path | `/path/to/micropython` |
| File system | `FFS` or automount |

The volume name is arbitrary; the example uses `PY0:` because that's the
mount the on-device runner snippets here are written against. There's
no need for a separate `tests:` (or similarly-named) assign — relative
paths from `PY0:` are enough.

#### Running individual tests in Amiberry

```sh
1> cd py0:tests
1> micropython basics/string_format.py
1> micropython float/float_parse.py
1> micropython io/file1.py
```

To verify output, compare against CPython on the host:

```sh
python3 tests/basics/string_format.py
```

The outputs should be identical for passing tests. Tests that require a feature
absent from the port print `SKIP` and exit cleanly.

### Test suite summary

Latest snapshot under vamos via `tools/amiga-vamos-run.sh`:

| Directory | Files | Pass | Self-skip | Fail | Notes |
|-----------|------:|-----:|----------:|-----:|-------|
| `basics/`     | 574 | 490 | 83 | 1  | `struct1.py` (bebbo ABI alignment) |
| `float/`      | 68  | 54  | 11 | 3  | EXACT-mode precision at double-range edges |
| `io/`         | 16  | 12  | 3  | 1  | `argv.py` (vamos host-path rewriting) |
| `import/`     | 30  | 28  | 0  | 2  | `import_file.py`, `builtin_ext.py` |
| `micropython/`| 108 | 42  | 19 | 47 | All native_*/viper_* — Phase 12 |
| `misc/`       | 14  | 6   | 8  | 0  | Skips are settrace, sys_exc_info, cexample (build-time module) |
| `cmdline/`    | 25  | 9   | 2  | 14 | Unix-port-specific (REPL banner, `-v`, terminal editing) |
| `stress/`     | 13  | 12  | 0  | 1  | `bytecode_limit.py` parser memory pressure |

Aggregate: **653 pass / 126 self-skip / 69 fail** out of 848 test files.

Excluding the Phase-12 native/viper failures, the unix-port-specific
cmdline tests, and the deferred `bytecode_limit.py` / `extreme_exc.py`
parser-memory edge cases, **9 individual tests fail** — the rest are
real platform differences documented in *Known test failures* below.

### Recommended test directories

| Directory | Tests | Expected result |
|-----------|-------|----------------|
| `basics/` | 491 | 490 pass, 83 self-skip (mostly intbig-only and threading), `struct1.py` fails (see below) |
| `float/` | 57 | 54 pass, 11 self-skip (mostly intbig-only); 3 fail on EXACT-mode precision near double range edges (see below) |
| `io/` | 16 | 12 pass, 3 self-skip (`os.remove`, `sys.std*.buffer`), `argv.py` fails (see below) |
| `micropython/` | 108 | 42 pass, 19 self-skip; 47 fail in `native_*` and `viper_*` (Phase 12) |
| `misc/` | 14 | 6 pass, 8 self-skip (settrace, sys_exc_info, cexample build-time module) |
| `cmdline/` | 25 | 9 pass, 2 self-skip, 14 fail — most failures are unix-port-specific (REPL banner format, `-v` bytecode dump, terminal-editing). `-X compile-only`, `-O`, `-m` SystemExit handling, and `sys.atexit` all pass. |
| `import/` | 30 | 28 pass; `import_file.py` fails for the same vamos path-rewriting reason as `io/argv.py`, `builtin_ext.py` fails because `uos` exposes no attributes (test relies on `os.sep` via `from uos import *`). |
| `stress/` | 13 | 12 pass; `bytecode_limit.py` fails with a parser IndentationError partway through (memory-pressure edge case as it exec()s bodies that approach the bytecode-jump limit). |

### Directories to skip

| Directory | Reason |
|-----------|--------|
| `inlineasm/` | Tests x86/ARM/Thumb/RISC-V inline assembly syntax |
| `extmod/machine_*.py` | Hardware peripherals (I2C, SPI, UART, …) not present |
| `multi_*/` | Requires two MicroPython instances communicating |
| `net_inet/`, `net_hosted/` | Requires a host-side network test harness |
| `thread/` | `MICROPY_PY_THREAD (0)` — threading disabled |

### Known test failures (real platform differences, not port bugs)

| Test | Cause |
|------|-------|
| `basics/struct1.py` | `struct.calcsize("97sI")` returns 102, the test expects 104. bebbo gcc on m68k uses 2-byte alignment for `int` per the AmigaOS m68k ABI (`__alignof__(int) == 2`), while CPython on x86 uses 4. Both are platform-correct; the test implicitly assumes POSIX/x86 alignment. |
| `float/float_parse*.py` | A handful of edge-case parses (very long mantissa with very negative exponent; `1e+300` vs `9.999...e+299` differing by 2 ULP; `1e4294967301` not detected as overflow → inf) come out 1–2 ULP off CPython. Bebbo's 80-bit long double soft-float has just enough precision loss that the EXACT-mode parser can't always nail the closest double. |
| `float/float_format_accuracy.py` | repr round-trip rate ~72% (test wants ≥99.7% for double EXACT mode). Same long-double precision tax — repr hands an inexact long double to the format routine for a fraction of inputs and gets a string that doesn't round back exactly. |

### Bebbo soft-float library bugs

Bebbo gcc 6.5b on `-msoft-float` ships several incorrect floating-point
helpers in libgcc / clib2 / libnix. `ports/amiga/floatconv.c` overrides
each one (some directly, some via `--wrap` because clib2 fat-packs
them with `__muldf3` and friends). If you ever drop `-msoft-float`
or move to a different toolchain, revisit whether these workarounds
are still needed.

| Routine | Bug | Trigger |
|---------|-----|---------|
| `__floatunsidf`, `__floatundidf`, `__floatdidf` | High-bit set values convert to garbage (e.g. `(double)0x8AC7230489E7FFFF` → `1.353e+18` instead of `9.999e+18`) | `float("9"*51 + "e-39")`, `array.array('Q', [...])`, anywhere `mp_obj_new_int_from_uint(>2^31)` is converted to double |
| `__eqdf2`, `__nedf2`, `__ledf2`, `__gedf2`, `__ltdf2`, `__gtdf2` | Treat NaN as ordered/equal (`nan == nan` returns true) | Every `==`/`!=`/`<=`/`>=` involving NaN; affects `math.isclose`, set/dict NaN keys, `if x != x` NaN check |
| `pow(-1, NaN)` (libnix) | Returns `1.0`; CPython expects NaN | `(-1) ** float('nan')` |
| `tgamma(-inf)` (libnix) | Returns `+inf`; CPython raises `ValueError` | `math.gamma(-inf)` |
| `__fixdfsi` (clib2) | Calls `MathIeeeDoubBasBase::IEEEDPFix`, which under vamos raises `ValueError: cannot convert float NaN to integer` and aborts the whole emulator | `hash(float('nan'))` (gcc 6.5 emits `__fixdfsi` for the bit-level body of `mp_float_hash` after optimisation) |

#### On-device batch runner (Amiberry)

The on-device test runner has no CPython to diff against, so it relies on
a `.exp` file existing next to every `.py` test. The upstream tree only
ships `.exp` files for tests where MicroPython output is *expected* to
differ from CPython — most tests are compared against CPython on the
host and don't carry one. Generate the missing ones with host CPython
before running the suite on the Amiga:

```sh
# From the repo root, once. Re-run if you sync the test tree from
# upstream. Files are written next to the .py tests as <test>.py.exp
# in LF format; existing .exp files are left alone.
tools/amiga-gen-exp.py tests/basics tests/float tests/io \
    tests/import tests/micropython tests/misc tests/cmdline tests/stress
```

Without this, every test whose expected output happens to contain the
substring `Error` (e.g. anything that prints `TypeError` deliberately,
of which there are many in `basics/`) gets flagged as a failure by the
on-device runner's `"Error" in output` fallback.

Set the AmigaDOS stack to at least 32 KB before invoking the runner —
the AmigaDOS default (typically 4–8 KB) is too small for the deep
compile-time recursion that some tests (notably `try_except_break.py`)
trigger, and overflow shows up as a `Software Failure 8000000B`
(Line F trap) rather than a Python-side `RuntimeError`:

```
1> Stack 32768
```

The on-device runner lives at `tools/amiga-runtests.py`. With the repo
mounted as `PY0:` (per the section above), it's already accessible at
`PY0:tools/amiga-runtests.py` — no need to copy it anywhere.

It uses `amiga.execute()` with shell redirection to capture each test's
output, compares against the matching `.exp` file (with `########` line
wildcards handled the same way `tests/run-tests.py` does), and writes
per-test failure artefacts to a result directory the same way the host
runner does:

```sh
1> Stack 32768
1> cd py0:
1> micropython tools/amiga-runtests.py tests/basics
1> micropython tools/amiga-runtests.py tests/float T:my-results/
1> micropython tools/amiga-runtests.py tests/io
```

The second positional argument is the result directory; default is
`T:mp-test-results/` (RAM under AmigaOS, so a reboot wipes it). On
FAIL, the captured stdout/stderr lands at `<dir>/<test>.py.out` and
the expected output at `<dir>/<test>.py.exp`. On pass (or skip), any
stale `.out`/`.exp` from a previous run gets deleted so the dir
always reflects the current state. After a run you can inspect a
failure with:

```sh
1> Type T:mp-test-results/struct1.py.out
1> Type T:mp-test-results/struct1.py.exp
```

The generated `.exp` files in `tests/` are not checked into the
repository; add `tests/**/*.py.exp` to `.git/info/exclude` if
`git status` chatter bothers you.


---

## Implementation Order Summary

| Phase | Status | Outcome |
|-------|--------|---------|
| 0 — Toolchain | ✅ Done | bebbo GCC 6.5.0b at `/opt/amiga` |
| 1 — Skeleton | ✅ Done | Builds and runs in Amiberry (185 KB AmigaOS executable) |
| 2 — File I/O | ✅ Done | `open()`, `import`, context manager; newlib stdio (no NDK needed) |
| 3 — Stdlib | ✅ Done | math, struct, json, re, hashlib, float; json.loads via port-local modjson.c |
| 4 — `amiga` module | ✅ Done | os_version, find_task, alloc_vec, free_vec, execute; SystemTagList for exit codes |
| 5 — 68k emitter | ✅ Done | `@micropython.native` via `MICROPY_EMIT_68K`; try/except in native mode is a known limitation |
| 6 — Package imports | ✅ Done | `Lock`/`Examine` in `mp_import_stat()`; enables `import mypackage` |
| 7 — Ctrl+C | ✅ Done | `CheckSignal(SIGBREAKF_CTRL_C)` via `MICROPY_VM_HOOK_LOOP` |
| 8 — Native API migration | ✅ Done | `dos.library` throughout; BSS 263 KB → 1 KB; exact GC stack bounds |
| 9 — Networking | ✅ Done | `bsdsocket.library` socket module; `SocketBase` opened in `main()` |
| 10 — CLI args | ✅ Done | `-h/--help/--version/-c/-m/script.py`; sys.argv, sys.path populated |
| 11 — CI | Planned | `.github/workflows/ports_amiga.yml` cross-compile check |
| 12 — Native emitter rework | Planned | Fix `ASM_CALL_IND` crash that blocks ~47 native/viper tests; see Phase 12 section |
| 13 — REPL line editing | ✅ Done | `shared/readline/` drives the REPL; `mp_hal_stdin_rx_chr` translates AmigaOS CSI (`0x9B`) to `ESC [`; cursor keys, history, kill/yank all work |
| 14 — Dynamic heap / runtime sizing | Planned | Replace fixed 256 KB heap with `-X heap=<N>`/`MICROPYHEAP` initial size plus `gc_add()`-based growth on demand, capped by `-X maxheap=<N>`; `amiga.heap_info()` for introspection |
| 15 — Memory-pool API | Planned | Expose `CreatePool`/`AllocPooled`/`FreePooled`/`DeletePool` on the `amiga` module; optional `MemoryPool` context-manager wrapper for `with`-statement use |
| 16 — VFS / `os.*` file I/O | ✅ Done | `MICROPY_VFS=1` with port-local `VfsAmiga` in `vfs_amiga.c` wrapping `dos.library`; `os.chdir`/`listdir`/`stat`/etc. work; `amigafile.c` and `amigaio.c` removed; on-device test runner uses `os.chdir` |
| 17 — Native library access | Planned | `amiga.lib_open` / `lib_close` / `lib_call` asm trampoline + `.fd`-driven `amiga.library(...)` proxy; unlocks every system and third-party library without per-library C bindings |
| 18 — ARexx integration | Planned | Outbound `amiga.rexx(port, command)` and inbound `MICROPYTHON.1` port via `rexxsyslib.library` |
| 19 — Workbench launch / tooltypes | Planned | Handle `WBStartup` message, read tooltypes via `icon.library`; ship `tools/amiga-mkicon.py` for companion `.info` files |
| 20 — Env-var integration | ✅ Done | `os.getenv`/`os.putenv`/`os.unsetenv` via `GetVar`/`SetVar` (flags=0, local vars; matches Unix `os.putenv` semantics) in `ports/amiga/modos.c` (included via `MICROPY_PY_OS_INCLUDEFILE`); two vamos bugs worked around — see Phase 20 |
| 21 — Volume / assign introspection | Planned | `amiga.volumes()` / `assigns()` / `disk_info()` via `DosList` and `Info()` |
| 22 — AmigaDOS pattern matching | Planned | `amiga.match("#?.py")` via `MatchFirst`/`MatchNext` |
| 23 — `timer.device` timing | Planned | Replace newlib `clock()` in `mp_hal_delay_us` / `ticks_*` with MICROHZ unit + `ReadEClock()` |
| 24 — Persistent REPL history | Planned | Save/restore readline ring to `ENVARC:MICROPYTHON_HISTORY` |
| 25 — Extra break signals | Planned | Expose `SIGBREAKF_CTRL_D/E/F`, `amiga.signal()`, `amiga.wait_signal()` (Ctrl+C-safe) |
| 26 — `PROGDIR:` on `sys.path` | Planned | One-line `main.c` change so scripts next to the binary are importable |
| 27 — Build variants | ✅ Done | `make VARIANT=<name>` with `build-<name>` output dir; `standard` (default, 68020 soft-float), `a1200` (trimmed for stock unaccelerated A1200), `68020fpu` (68020 + 68881 coprocessor), `68040` (68040 with built-in FPU); FPU variants skip libgcc soft-float wraps |
