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
- Phase 14 ✅ Dynamic heap growth via `MICROPY_GC_SPLIT_HEAP_AUTO`; initial size from `-X heap=<N>` / `MICROPYHEAP`; total bounded by `-X maxheap=<N>` / `MICROPYHEAPMAX`; `amiga.heap_info()` for introspection
- Phase 16 ✅ Pythonic file I/O: VFS layer (`os.chdir`, `os.listdir`, `os.stat`, etc.) backed by `dos.library`
- Phase 17 — Native AmigaOS library access: generic `lib_open`/`lib_call` trampoline plus `.fd`-driven `amiga.library(...)` proxy for system and third-party libraries
- Phase 18 ✅ ARexx integration — inbound via `amiga.rexx_open` / `rexx_recv` / `RexxMessage.reply`; outbound via `amiga.rexx(host, command)` / `_amiga.rexx_send`
- Phase 19 ✅ Workbench launch support: `amiga.launched_from_workbench()` / `wb_selected_files()` / `tooltype()`, auto-opened CON: window for stdin/stdout, `SCRIPT=` / `HEAP=` / `MAXHEAP=` tooltypes honoured at startup
- Phase 20 ✅ Env-var integration: `os.getenv` / `os.putenv` / `os.unsetenv` backed by `dos.library` `GetVar`/`SetVar` (local vars, matching Unix semantics; `ENV:` and `ENVARC:` writeable directly for shell-shared / persistent vars)
- Phase 21 ✅ Volume / assign introspection: `amiga.volumes()`, `amiga.assigns()`, `amiga.disk_info(path)` via `LockDosList`/`NextDosEntry` and `Info()`
- Phase 22 ✅ AmigaDOS pattern matching: `amiga.match("#?.py")` (eager list) and `amiga.imatch(...)` (iterator with finaliser) via `MatchFirst`/`MatchNext`/`MatchEnd`
- Phase 23 ✅ `timer.device`-backed timing: MICROHZ-accurate `mp_hal_delay_us` / `ticks_us` via `timer.device` + cached-frequency `ReadEClock()`
- Phase 24 ✅ Persistent REPL history saved to `S:MicroPython.history` (overridable via `MICROPYHISTORY` env-var); ring size bumped to 32
- Phase 25 ✅ Extra break signals: `SIGBREAKF_CTRL_D/E/F` constants plus `amiga.signal()` / `amiga.wait_signal(mask, timeout_ms=N)` (Ctrl+C-safe; timeout backed by a dedicated `timer.device` async port)
- Phase 26 ✅ `PROGDIR:` appended to `sys.path` after `mp_init()` so modules dropped next to the binary import without setup
- Phase 27 ✅ Additional build variants: `minimal` (stock unaccelerated A1200), `68020fpu` (68020/68030 with 68881 coprocessor), `68040` (68040 with built-in FPU); FPU variants drop the libgcc soft-float wraps

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

# Phase 21 — dos.library introspection (see Phase 21 below)
amiga.volumes()             # → list[str] of mounted volume names ("Work:", "RAM:", ...)
amiga.assigns()             # → dict[str, str] mapping assign name to resolved target
amiga.disk_info(path)       # → (free_bytes, total_bytes, block_size) for the path's volume

# Phase 22 — AmigaDOS pattern matching (see Phase 22 below)
amiga.match(pattern)        # → list[str] of full paths matching an AmigaDOS pattern
amiga.imatch(pattern)       # → iterator yielding matches lazily (frees AnchorPath on GC)

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

Remaining newlib use: superseded by Phase 23 — `mp_hal_delay_us` and `mp_hal_ticks_*`
now use `timer.device` (MICROHZ + `ReadEClock()`) for microsecond accuracy.

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
AMIGA_VARIANT=minimal tools/amiga-vamos-repl.sh   # minimal build
```

`AMIGA_VARIANT` picks which `build-<variant>/micropython` to launch and
the appropriate vamos `--cpu` flag (same mapping as
`amiga-vamos-run.sh` — see the testing section). `68020fpu` can't run
under vamos because vamos has no 68881 emulation.

Pipe input (`printf ... | vamos ...`) is unaffected and doesn't need the
wrapper, because pipes never get cooked-mode line buffering.

---

### Phase 14 — Dynamic heap growth and runtime sizing ✅

The GC heap is no longer a fixed `MICROPY_HEAP_SIZE` block: the port
allocates an initial chunk at startup whose size comes from
(in priority order) `-X heap=<N>[K|M]`, the `MICROPYHEAP` env var
(`GetVar` from `dos.library`), or the compile-time `MICROPY_HEAP_SIZE`
default. When that chunk fills up MicroPython grows the heap on demand
by allocating further `AllocVec` chunks and feeding them to the GC via
`gc_add` — this is the upstream `MICROPY_GC_SPLIT_HEAP_AUTO` machinery.
A `-X maxheap=<N>` cap (or the `MICROPYHEAPMAX` env var) bounds total
growth so a runaway script can't exhaust system RAM.

#### Implementation

Three small pieces:

**`mpconfigport.h`** enables `MICROPY_GC_SPLIT_HEAP=1` and
`MICROPY_GC_SPLIT_HEAP_AUTO=1`, and points
`MP_PLAT_ALLOC_HEAP`/`MP_PLAT_FREE_HEAP` at `amiga_alloc_heap` /
`amiga_free_heap` in `main.c`. The compile-time `MICROPY_HEAP_SIZE`
becomes the default *initial* size only.

**`main.c`** owns the tracking array, the size-string parser, and
`gc_get_max_new_split()`:

- `amiga_heap_chunks[16]` records every still-live chunk
  (`(ptr, bytes)` pairs). `amiga_alloc_heap` slots a new chunk in;
  `amiga_free_heap` clears one. Tracking is essential: AmigaOS does
  *not* automatically reclaim `AllocVec`'d memory when the task exits,
  so at shutdown we walk the array and `FreeVec` everything still
  recorded.
- `parse_heap_size("256K"|"2M"|"524288")` parses size strings into
  byte counts. Accepts unsuffixed decimals, `K` / `k`, `M` / `m`.
- A pre-scan over `argv` for `-X heap=` and `-X maxheap=` runs *before*
  the initial allocation, so the chosen size is in effect for
  `gc_init()`. The same loop also consults `GetVar(MICROPYHEAP)` and
  `GetVar(MICROPYHEAPMAX)` (lower priority than `-X` flags). The
  existing `-X` handler later in `main()` accepts the same flags as
  no-ops since the pre-scan has already consumed them.
- `gc_get_max_new_split()` reports the largest `AllocVec`-able block
  via `AvailMem(MEMF_ANY|MEMF_PUBLIC|MEMF_LARGEST)` minus a small
  exec.library bookkeeping headroom, clamped further to the
  `-X maxheap` cap.
- At shutdown, the chunks array is walked and every live entry is
  `FreeVec`'d. The GC's own `gc_sweep_free_blocks` already auto-releases
  empty grown chunks during normal operation, so most of the time only
  the initial chunk is still live at exit anyway.

**`modamiga.c`** adds `amiga.heap_info()` returning
`(total_bytes, free_bytes, num_arenas)`:

```python
>>> import amiga
>>> amiga.heap_info()
(256000, 248000, 1)              # 256 KB initial, mostly free, one chunk
>>> data = [bytearray(32*1024) for _ in range(20)]   # ~640 KB live
>>> amiga.heap_info()
(1024000, 366000, 3)             # grew to ~1 MB across three chunks
>>> data = None
>>> import gc; gc.collect()
>>> amiga.heap_info()
(256000, 248000, 1)              # back to one chunk; grown chunks released
```

`gc_info()` (in `py/gc.c`) is the source for total/free; the arena
count comes from the port-side tracker.

#### Flags choice

All allocations use `MEMF_ANY|MEMF_PUBLIC|MEMF_CLEAR`. AmigaOS's free
list is ordered fastest-first, so `MEMF_ANY` already prefers Fast RAM
when available; chip-RAM-only machines (stock A500/A1000/A2000) get
served from Chip without needing a fallback path. `MEMF_CLEAR` zeroes
each chunk so `gc_init`/`gc_add` don't see stale bits from a previous
task or from `AllocVec`'s freelist.

#### Decisions captured

- **Don't release chunks below a low-water mark.** The GC already
  releases empty grown chunks during sweep (per
  `MICROPY_GC_SPLIT_HEAP_AUTO`); we don't need additional policy.
  Eager release of chunks that still hold live objects is unsafe.
- **No artificial growth throttle.** Upstream's growth heuristic
  (double-the-total-heap-size each grow, clamped to "what AmigaOS can
  give us") is sensible; adding our own occupancy threshold on top
  would just delay growth to the point of needless GC churn.
- **`MICROPYHEAP` / `MICROPYHEAPMAX` are checked before `-X` flags
  but command-line flags win.** Lets a user pin a workload's size in
  the shell environment yet override per-invocation when scripting.

#### Acceptance

Verified end-to-end under vamos: `-X heap=512K` and `-X heap=128K` set
the initial chunk size correctly; an in-script allocation of 640 KB
grows the heap from 256 KB / 1 arena to ~1 MB / 3 arenas; freeing the
data and calling `gc.collect()` shrinks back to the initial 256 KB / 1
arena (the GC's own chunk-release path); `-X maxheap=512K` produces a
clean `MemoryError` when the cap is hit rather than crashing or
ignoring the cap. The `basics/` regression remains at 490/491.
`MICROPYHEAP` env-var lookup uses the same `GetVar(flags=0)` path that
Phase 20 validated; testing the env-var read end-to-end requires real
AmigaOS / Amiberry because vamos doesn't propagate ENV: assigns into
the local-var lookup path.

---

### Phase 15 — *withdrawn*

Originally planned to expose `exec.library`'s memory-pool API
(`CreatePool` / `AllocPooled` / `FreePooled` / `DeletePool`) as
`amiga.create_pool()` etc. Dropped: Phase 14's dynamic GC heap
already covers the common case for Python-level allocations, and the
only remaining use (explicit native buffer lifetimes) is rare enough
that `alloc_vec` is sufficient. If per-call scratch pools turn out to
be useful inside the Phase 17 native-library trampoline, fold the
pool bindings in there instead of as a standalone phase.

(Phase number kept as a placeholder to avoid renumbering later phases.)

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

1. ✅ `lib_open` / `lib_close` / `lib_call` with the asm trampoline.
   Smoke test: reopen `intuition.library` and call `DisplayBeep(0)`.
2. ✅ Frozen `.fd` table generator (`tools/amiga-fdgen.py`) for NDK
   libraries, with chronological NDK-release ordering and per-function
   `since` stamps.
3. ✅ `amiga.library(name, version)` proxy on top of the trampoline,
   driven by the frozen FD table — the headline deliverable for the
   phase, "anything in the NDK callable from Python".
4. ✅ Tag-list helper (`amiga.taglist(WA_Width=640, ...) -> TagList`)
   plus low-level peek/poke primitives.
5. ✅ Third-party `.fd` search path plus explicit
   `signatures=` / `library_from_signatures(...)` override.
6. ✅ Minimal struct helpers on top of the peek/poke primitives —
   `amiga.Struct` ctypes-lite, plus hand-curated Node / Task /
   Library / DateStamp / FileInfoBlock / IntuiMessage layouts.
7. (Later) callback thunks if anyone actually needs them.

#### Step 1 — trampoline (shipped)

`ports/amiga/amiga_lib_call.S` is a single hand-written 68k routine.
Its C entry point is

```c
uint32_t amiga_lib_call_asm(
    uint32_t base, int32_t offset,
    uint32_t d0, uint32_t d1, uint32_t d2, uint32_t d3,
    uint32_t d4, uint32_t d5, uint32_t d6, uint32_t d7,
    uint32_t a0, uint32_t a1, uint32_t a2, uint32_t a3,
    uint32_t a4, uint32_t a5);
```

The asm saves the m68k SysV callee-saved set (`d2-d7/a2-a6`), loads
`a6` with the library base, then loads the 14 register slots from the
stack. The runtime jump to `base + offset` uses an **RTS-trick**
rather than a computed `JSR`: with all of `d0`-`d7` / `a0`-`a5`
already committed to user-supplied values, no scratch register is
free to hold the call target at the moment of the jump. Pushing the
target plus a local return label and issuing `rts` transfers control
to the library; the library's own `rts` pops the local label and
returns to our cleanup. Effectively a JSR-through-runtime-pointer
without burning a register.

`modamiga.c` exposes three Python entry points:

- **`amiga.lib_open(name, version=0)`** wraps `OpenLibrary`. Returns
  the library base as a Python int (matching the existing `alloc_vec`
  pointer convention); raises `OSError(ENOENT)` on failure.
- **`amiga.lib_close(base)`** wraps `CloseLibrary`; tolerates a zero
  base so callers can unconditionally close in a `try/finally`.
- **`amiga.lib_call(base, offset, **regs, ret="d0")`** drives the
  trampoline. Register kwargs are `d0`-`d7` and `a0`-`a5`; unspecified
  registers default to 0. `ret="d0"` interprets the return as a signed
  32-bit int (default), `ret="d0u"` as unsigned, `ret="void"` returns
  `None`.

Smoke-tested under Amiberry: `FindTask(NULL)` via the trampoline
matches `amiga.find_task(None)` (proves A1 + D0 round-trip);
`AllocVec(64, MEMF_ANY|MEMF_CLEAR)` returns a valid pointer freed by
a paired `FreeVec` (proves D0/D1/A1 + correct LVO dispatch);
`DisplayBeep(NULL)` against `intuition.library` returns cleanly;
`lib_open("nosuch.library", 0)` raises `OSError(ENOENT)`; unknown
`lib_call` kwargs raise `TypeError`.

#### Step 2 — FD table generator (shipped)

`tools/amiga-fdgen.py` parses every `.fd` file under one or more NDK
trees and emits a single Python module whose `LIBRARIES` dict maps
each openable name (`"intuition.library"`, `"console.device"`,
`"card.resource"`, ...) to a per-library dict of
`function_name -> (lvo, regs_csv, since)`.

Key design choices:

- **Chronological release ordering.** AmigaOS development restarted
  under Hyperion several years after 3.9, so 3.1.4 → 3.2 → 3.2.x → 3.3
  are *calendar-newer* than 3.5/3.9. A simple numeric-tuple sort would
  put 3.2 before 3.9 and silently wreck the `since` stamps. The tool
  uses a hand-maintained `AMIGA_OS_RELEASE_ORDER` list and looks up
  each source's chronological position from it. Unknown versions emit
  a one-time stderr warning and sort after every known release.
- **Drift detection.** LVOs are append-only in AmigaOS (libraries
  never renumber); a function that appears with a different offset or
  register list in a later NDK is a hard warning. The earlier entry
  wins on the assumption that the older NDK is more authoritative
  about historical layout.
- **Public-only by default.** `##private` sections still consume LVO
  slots so the offset arithmetic is correct, but the resulting
  functions are dropped from the output unless `--include-private` is
  passed.
- **CIA + IEEE-double exceptions.** Two corners of the NDK don't fit
  the standard 1-reg-per-arg / no-A6-input convention
  (`cia_lib.fd`'s `AbleICR`/`SetICR` take a CIA pointer in A6;
  `mathieeedoub*` doubles span two registers per arg). The tool warns
  about these and skips them; users wanting access can write a
  small port-side wrapper.

Run against the current bebbo NDK (which already ships the Hyperion
3.2 FDs): **76 .fd files → 75 openable names → 1146 public function
signatures**, ~80 KB of Python source.

#### Step 3 — `amiga.library` proxy (shipped)

The C extension is now registered as **`_amiga`** (the underscore
prefix is a deliberate signal that callers should reach for the
high-level wrapper). A frozen Python module at
`ports/amiga/modules/amiga.py` re-exports every name from `_amiga`
via `from _amiga import *`, and adds the `Library` class on top of
the FD table baked in from `ports/amiga/modules/_amiga_fd.py`:

```python
import amiga

with amiga.library("intuition.library", 37) as intuition:
    intuition.DisplayBeep(0)            # signature from _amiga_fd
    win = intuition.OpenWindow(nw_ptr)  # A0 = nw_ptr, return in D0
    intuition.CloseWindow(win)
```

`Library.__getattr__` looks up the signature, builds a thin closure
that maps positional args into the right register kwargs of
`_amiga.lib_call`, and `setattr`s the closure onto the instance —
so subsequent reads skip `__getattr__` and execute as fast as a
normal method call. `Library` supports context-manager use
(`__enter__`/`__exit__` close on exit), explicit `close()`, and a
GC-time best-effort cleanup via `__del__`.

To wire frozen modules in: `ports/amiga/manifest.py` is the standard
`freeze("$(PORT_DIR)/modules")` one-liner; the Makefile sets
`FROZEN_MANIFEST = manifest.py` and forces the freeze pipeline to
use the LONGLONG int impl via `MPY_TOOL_FLAGS = -mlongint-impl=longlong`
(the port's `MICROPY_LONGINT_IMPL` setting), since `mpy-tool.py`
defaults to MPZ-encoded literals which would fail the `static_assert`
in `frozen_content.c` at link time.

Smoke-tested under Amiberry:

- `amiga.find_task` and other re-exports from `_amiga` are available
  on the `amiga` module without explicit imports.
- `amiga.library("exec.library", 0)` opens; `ex.FindTask(0)` matches
  the C-side `amiga.find_task(None)` (proves A1 dispatch + D0 return).
- `ex.AllocVec(64, MEMF_ANY|MEMF_CLEAR)` / `ex.FreeVec(...)` round
  trip cleanly (proves D0/D1/A1 dispatch).
- `with amiga.library("intuition.library", 37) as intuition`:
  `intuition.DisplayBeep(0)` returns cleanly; the context manager
  closes on `__exit__`.
- Error paths: unknown library → `OSError(ENOENT)`; missing function
  → `AttributeError` with a clear message; wrong arg count →
  `TypeError`; call after close → `ValueError`.
- The closure is cached via `setattr(self, fnname, call)` after first
  use; verified by `'FindTask' in ex.__dict__` after the first call.

#### Step 4 — peek/poke + TagList (shipped)

Tag lists are pervasive on OS3.x — `OpenWindowTagList`,
`OpenScreenTagList`, `EasyRequestArgs`, `NewObject`, and most of
gadtools all consume a `TAG_DONE`-terminated `TagItem[]` array.
Building one from Python needs three pieces.

**Low-level memory primitives (in `modamiga.c`).** A small set of
read/write helpers on raw addresses:

- `amiga.peek_b(addr)` / `peek_w(addr)` / `peek_l(addr)` — read
  unsigned 8 / 16 / 32-bit value at `addr`.
- `amiga.peek_bytes(addr, n)` — copy `n` bytes out as a `bytes`.
- `amiga.poke_b(addr, v)` / `poke_w(addr, v)` / `poke_l(addr, v)` —
  write the corresponding-size value.
- `amiga.poke_bytes(addr, data)` — `memcpy` the buffer at `addr`.

These are the same primitives Phase 17 step 6 will use for struct
helpers, so they're now in place ahead of time.

**`TagList` class (in frozen `amiga.py`).** Wraps an `AllocVec`'d
`TagItem[]` array.  Each `(tag, value)` slot is two ULONGs (8 bytes);
ints go straight into `ti_Data`, while `bytes` and `str` values get
their own `AllocVec`'d buffer (NUL-terminated for C interop) with the
buffer's address stored as `ti_Data`.  All allocations — main array
plus per-string buffers — are released by `close()`, by `__exit__`
on a `with`-block, or as a GC fallback in `__del__`.  The class
defines `__int__` returning the head address so callers can pass a
`TagList` directly to a `Library` proxy call.

**`amiga.taglist(...)` factory.** Builds a `TagList` from kwargs
and/or positional args.  Kwargs are resolved against the hand-curated
table in `_amiga_tags.TAGS` (universal `TAG_*` markers plus the
common `WA_*` and `SA_*` IDs computed from
`intuition/intuition.h` and `intuition/screens.h`).  Positional args
support three forms: a single iterable of `(tag, value)` pairs;
multiple `(tag, value)` tuples; or alternating `tag, value, tag,
value, ...`.  Unknown tag names raise `KeyError`.

```python
with amiga.taglist(WA_Width=640, WA_Height=480, WA_Title="hi") as tags:
    with amiga.library("intuition.library", 37) as intuition:
        intuition.OpenWindowTagList(0, tags)   # auto-unwraps via __int__
```

**Library proxy transparency.** `Library.__getattr__`'s generated
closure now runs `int(v)` on any non-int argument, so a `TagList`
passes through cleanly without the caller having to write
`tags.addr` or `int(tags)`.

Smoke-tested under Amiberry:

- `peek_b/w/l/bytes` and `poke_b/w/l/bytes` round-trip the expected
  values on an `AllocVec`'d buffer (big-endian, native m68k).
- `TagList([(WA_Width, 640), (WA_Height, 400)])` produces a 24-byte
  layout `[80 00 00 66] [00 00 02 80] [80 00 00 67] [00 00 01 90]
  [00 00 00 00] [00 00 00 00]` (TAG_DONE terminator), confirmed via
  `peek_l` at each offset.
- A `TagList` containing `WA_Title="hi"` allocates a separate
  3-byte buffer (`b"hi\x00"`); `peek_bytes` at the `ti_Data` pointer
  reads back the correct bytes.  `close()` zeros the buffer count.
- `amiga.taglist(WA_Width=640, WA_Title="hi")` resolves both kwargs
  from `_amiga_tags.TAGS`; `amiga.taglist(NoSuchTag=1)` raises
  `KeyError`.
- Passing the `TagList` directly to a `Library` call
  (`ex.FindTask(tl)`) reaches the trampoline without `TypeError` —
  the proxy unwrapped via `int()` and the trampoline forwarded the
  address into A1.

#### Step 4 follow-up — full NDK tag-name coverage, parsing, chaining

The hand-curated starter table in `_amiga_tags.py` was replaced by a
generated one covering every plausible tag name in the 3.2 NDK.

**`tools/amiga-taggen.py`** drives bebbo's m68k cross-gcc to harvest
tag IDs:

1. Runs `gcc -E -dM` over a generated C source that `#include`s the
   common tag-bearing headers (`intuition/intuition.h`,
   `intuition/screens.h`, `gadgetclass.h`, `libraries/asl.h`,
   `libraries/gadtools.h`, `datatypes/datatypes.h`, etc.) and
   collects every macro name matching `<UPPER>_<TagName>`.
2. Generates a second C file with
   `const ULONG _x_<NAME> = (ULONG)(<NAME>);` per candidate.  Compiles
   it with `-S -O2` so the compiler folds each constant expression
   (`(WA_Dummy + 0x01)` and so on) into a literal `.long`.
3. Parses the assembly — bebbo prepends `_` to every global, so we
   match `__x_<NAME>:` followed by `.long <value>` and decode either
   decimal or hex.
4. Bisects the candidate list to drop entries that aren't constant
   expressions (rare; usually function-style macros that slipped past
   the regex).
5. Keeps only values with bit 31 set (the TAG_USER namespace) plus
   the five universal `TAG_*` markers — drops the ~1800 unrelated
   uppercase constants that happen to match the regex.

Running it against `/opt/amiga/m68k-amigaos/ndk-include` yields
**1072 tag IDs** in `ports/amiga/modules/_amiga_tags.py` (was: ~85
hand-curated).  Tags covered include the full WA_*, SA_*, GA_*,
ICA_*, IA_*, PA_*, GTMN_*, GTLV_*, GTCB_*, ASLFR_*, ASLFO_*, ASLSM_*,
DTA_*, BIDTAG_*, IFFParseTag_* namespaces.  Frozen-module bloat is
~48 KB (the standard build went 439 KB → 488 KB).

**`amiga.parse_taglist(addr, max_items=64)`** walks a TagItem array
back into a `{tag_id: value}` dict.  Follows `TAG_MORE` chains
across multiple arrays (with `max_items` capping the total to
prevent runaway loops); drops `TAG_IGNORE` slots; honours
`TAG_SKIP` by advancing `1 + ti_Data` slots.  Returns `{}` for a
null address so callers can pass an unconditional
`parse_taglist(GetAttr(...))`.

**`taglist(..., more=other_taglist)`** writes a `TAG_MORE` terminator
pointing at `other_taglist`'s head address instead of the usual
`TAG_DONE`, and pins the chained `TagList` alive via a reference on
the outer one.  Closing the outer drops the keepalive; the inner
then GC's normally.

Smoke-tested under Amiberry:

- New-namespace kwargs resolve (`GA_Width = 0x80030005`,
  `ASLFR_TitleText = 0x80080001`, `DTA_GroupID = 0x8000101f`,
  `GTMN_FullMenu = 0x8008003e`).
- `parse_taglist(tl.addr)` returns `{WA_Width_id: 640, GA_Width_id: 120,
  ASLFR_TitleText_id: <ptr to "Hi" buffer>}` — string-payload tags
  surface their buffer pointer, exactly as written.
- `outer = taglist(WA_Width=640, more=inner)` writes
  `(TAG_MORE=0x2, inner.addr)` as the outer's terminator; a single
  `parse_taglist(outer.addr)` returns the union of both lists.
  Closing the outer keeps the inner alive (`inner.addr != 0`) until
  the user closes it.
- A manually-built TagList with a `TAG_IGNORE` slot in the middle
  parses back without the ignored entry.

#### Step 5 — runtime `.fd` loading + explicit signatures (shipped)

Two complementary mechanisms let callers reach libraries that aren't
baked into `_amiga_fd.LIBRARIES`:

**Runtime `.fd` search path.** When `Library(name)` doesn't find
`name` in the frozen table, it falls back to a small pure-Python
`.fd` parser in `amiga.py` (`_parse_fd`) and walks
`PROGDIR:fd/` then `LIBS:fd/`, trying both the bebbo `<base>_lib.fd`
and the bare `<base>.fd` convention.  Drop `mylibrary.fd` next to
the executable (or into LIBS:fd/), and `amiga.library("mylibrary.library")`
finds it.  The parser drops `##private` functions and skips
arg/reg-count mismatches, matching `tools/amiga-fdgen.py` semantics
— `##private` entries still consume LVO slots so that subsequent
public functions get the correct offsets (verified end-to-end in the
smoke test).

**Explicit `signatures=`.** `Library(name, version, signatures=...)`
(or the convenience factory `amiga.library_from_signatures(name,
version, signatures)`) bypasses both the frozen table and the runtime
search and uses the caller-supplied dict directly.  Accepts entries
in either `(lvo, regs_csv)` 2-tuple or `(lvo, regs_csv, metadata)`
3-tuple form; the trampoline only consumes the first two slots.

Smoke-tested under Amiberry: the parser correctly handled a
synthetic 5-function `.fd` snippet (including the
public→private→public bias-slot drift); `_load_fd("nosuch.library")`
returned `None`; `amiga.library("exec.library", signatures={"MyAllocVec": ...})`
dispatched the renamed function correctly and `ex.AllocVec` raised
`AttributeError` (proving the override is exclusive); the
`library_from_signatures(...)` factory matched the runtime
`amiga.find_task` result.

#### Step 6 — `Struct` ctypes-lite (shipped)

`amiga.Struct(addr, layout, name=None)` wraps a C struct living at a
fixed memory address and gives Python-attribute access to its
fields, dispatching through the peek/poke primitives from step 4.
`layout` is a `dict` mapping field name to `(offset, type_code)`:

| code | meaning |
|------|---------|
| `B` / `b` | unsigned / signed 8-bit |
| `H` / `h` | unsigned / signed 16-bit (big-endian m68k native) |
| `L` / `l` | unsigned / signed 32-bit |
| `P` | pointer (alias for `L`) |
| `sN` | raw N-byte region — read returns `bytes`, write accepts `bytes`/`str` and pads with NUL |
| `S` | NUL-terminated string *pointed at* by a ULONG slot (read-only; capped at 256 bytes) |

`Struct` defines `__int__` so an instance can be passed straight to
a `Library` proxy call wherever an address is expected (transparent
unwrap, same mechanism as `TagList`).  Unknown fields raise
`AttributeError`; reads always peek the live address (no caching);
writes go through `_amiga.poke_*` immediately.

`ports/amiga/modules/_amiga_structs.py` ships starter layouts for
the most-used Exec / DOS / Intuition structs:

| factory | struct | size |
|---------|--------|------|
| `amiga.Node(addr)` | `struct Node` | 14 bytes |
| `amiga.Task(addr)` | `struct Task` | 92 bytes |
| `amiga.Library_struct(addr)` | `struct Library` | 34 bytes |
| `amiga.DateStamp(addr)` | `struct DateStamp` | 12 bytes |
| `amiga.FileInfoBlock(addr)` | `struct FileInfoBlock` | 260 bytes |
| `amiga.IntuiMessage(addr)` | `struct IntuiMessage` | 52 bytes |

`amiga.Library_struct` carries the trailing `_struct` to avoid the
class-vs-struct collision with `amiga.Library` (which represents an
*opened* library proxy, not the raw memory layout).  Users wanting
other AmigaOS structs build their own `dict` and pass it to
`amiga.Struct(addr, my_layout)`.

Smoke-tested under Amiberry — every read pulled live data from the
running system:

- `amiga.Task(find_task(None))` returned `ln_Name = b'Initial CLI'`,
  `ln_Type = 13` (`NT_PROCESS`), `tc_State = 2` (`TS_RUN` for the
  currently running task, confirming the offsets are right), and
  non-zero stack bounds (4 KB CLI stack).
- `amiga.Library_struct(exec.base)` returned `ln_Name =
  b'exec.library'`, `lib_Version = 40`, `lib_Revision = 10`,
  `lib_IdString = b'exec 40.10 (15.7.93)\\r\\n'` — exactly the
  Kickstart 40.10 firmware identifier Amiberry boots.
- DateStamp round-trip: `ds.ds_Days = 18000; ds.ds_Tick = 0xDEADBEEF`
  poked the right offsets (raw `peek_l` confirms the encoding) and
  the attribute reads returned the same values.
- Unknown fields raise `AttributeError`.

Step 7 (callback thunks for hook-driven library calls) is the only
remaining piece, and is deferred until a concrete user need lands.

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
exposes an ARexx port that external scripts can drive.  Two halves:

1. **Outbound client — ✅ shipped.** `amiga.rexx("DOPUS.1", "lister new")`
   posts a command to another app's ARexx port and blocks for the result.
   Pure send-and-wait; doesn't require an inbound port to be open
   ourselves.  See "Outbound" below.

2. **Inbound port — ✅ shipped.** A public MsgPort named `MICROPYTHON.1`
   (or `.2`, `.3`... if multiple instances are running) is created on
   demand via `CreateMsgPort` + `AddPort`.  External `rx "address
   MICROPYTHON.1; ..."` invocations send command strings; the Python
   side calls `amiga.rexx_recv()` to pull them off the queue and
   `msg.reply(result, rc)` to send back a value.

#### Inbound port — as shipped

Five C primitives in `modamiga.c` and a Python `RexxMessage` facade in
`amiga.py`:

```python
import amiga

name = amiga.rexx_open()              # opens MICROPYTHON.1 (or .N)
while True:
    msg = amiga.rexx_recv(timeout_ms=1000)
    if msg is None:
        continue
    try:
        result = eval(msg.command.decode("ascii"))
        msg.reply(str(result), rc=0)
    except Exception as e:
        msg.reply(str(e), rc=10)
amiga.rexx_close()

# Or the ready-made dispatcher:
amiga.rexx_serve(lambda cmd: eval(cmd))
```

- **`amiga.rexx_open(stem="MICROPYTHON")`** finds the lowest free
  `.N` (1-16) suffix under a `Forbid()`/`Permit()` so a racing task
  can't claim the same name, `CreateMsgPort()` + `AddPort()`, and
  returns the assigned name.
- **`amiga.rexx_close()`** drains anything still queued — replying
  rc=20 (FAIL) so a hung `rx` doesn't stay blocked forever —
  `RemPort()`, `DeleteMsgPort()`, and closes `rexxsyslib.library`
  if it was opened.  Also called from `main.c` on interpreter exit
  as a safety net.
- **`amiga.rexx_port_name()`** returns the current name, or `None`.
- **`amiga.rexx_recv(timeout_ms=None)`** polls then `Wait()`s.  The
  Phase 25 async `timer.device` port supplies the timeout signal
  bit, and `SIGBREAKF_CTRL_C` is always ORed in so the user can break
  out — if it fires, `KeyboardInterrupt` is raised.  Returns a
  `RexxMessage` instance, or `None` on timeout.
- **`amiga.rexx_command(msg_ptr)`** reads `rm_Args[0]` as bytes
  (the command string the sender packed via `CreateArgstring`).
- **`amiga.rexx_reply(msg_ptr, rc, result_or_None, secondary=0)`**
  sets `rm_Result1` = `rc`.  If `rc == 0` and the result is non-None,
  opens `rexxsyslib.library` lazily, calls `CreateArgstring` over the
  bytes, and stores the argstring pointer in `rm_Result2`; the rx
  interpreter then reads it into the special variable `result` (with
  `options results` enabled) and `DeleteArgstring`s when done.
  Otherwise `rm_Result2` gets `secondary` as the conventional error-
  code slot.  Finally `ReplyMsg()`.

#### Failure modes if `rexxsyslib.library` is missing

Phase 18 lazy-opens `rexxsyslib.library` only when a path actually
needs `CreateArgstring` / `DeleteArgstring` / `CreateRexxMsg`.  So on
a stripped AmigaOS install without ARexx, MicroPython still starts,
`import amiga` works, and a script that opens an inbound port to
receive rc-only requests works.  The hard failures are scoped:

| Code path | If rexxsyslib missing |
|-----------|-----------------------|
| `amiga.rexx_open` / `rexx_close` / `rexx_recv` / `RexxMessage.command` | OK (exec.library only) |
| `RexxMessage.reply(result=None, rc=...)` (no string) | OK |
| `RexxMessage.reply(result="...", rc=0)` | Replies rc=10 to the sender so it doesn't hang, then raises `OSError("rexxsyslib.library unavailable")` to the script |
| `amiga.rexx(host, command)` (outbound) | Raises `OSError("rexxsyslib.library unavailable")` before any send happens |

The `RexxMessage` wrapper class in `amiga.py` exposes `.command`,
`.action`, and `.reply(result=None, rc=0, secondary=0)`.  `__del__`
sends an automatic rc=20 reply if the script forgets — a forgotten
reply would block the sender forever, which is worse than a synthetic
failure code.

#### `rx` script gotcha

Worth pinning down because it cost a debugging round-trip and any
future test author will hit the same wall: `call open(...)`,
`call writeln(...)`, and `call close(...)` each clobber the rx
special variable `result` with the called function's return value
(byte counts, boolean OK, etc.).  Any rx script that wants to
inspect the host's reply must snapshot it into another variable
*immediately* after `address`:

```rexx
options results
address MICROPYTHON.1 'some command'
saved_rc = rc
saved_result = result
call open(out, 'somefile.txt', 'w')
call writeln(out, 'rc=' || saved_rc)
call writeln(out, 'result=' || saved_result)
call close(out)
```

Without the snapshot, the file ends up with `result=` set to the byte
count of the previous `writeln`.

#### Smoke test results

Tested in Amiberry against a real `rx` invoked through `amiga.execute`:

- `amiga.rexx_open()` returns `"MICROPYTHON.1"`, visible to ARexx.
- A round-trip `rx "address MICROPYTHON.1 '2 + 2'"` arrives at our
  port as `b'2 + 2'` with action `0x01020000` (`RXCOMM | RXFF_STRING`),
  is `eval`'d to `"4"`, replied via `CreateArgstring`, and the rx
  script captures `rc=0`, `result=4`.
- `'hello'.upper()` round-trips as `"HELLO"`,
  `[i*i for i in range(5)]` round-trips as `"[0, 1, 4, 9, 16]"`.
- `amiga.rexx_close()` correctly tears the port down; reopening
  picks `MICROPYTHON.1` again.
- The shell's first-of-session `rx` invocation takes 10+ seconds to
  cold-start the rexxmast machinery and parse the script; subsequent
  rx invocations are sub-second.  Test scripts driving rx must
  account for this by using a long first-iteration timeout (30 s) or
  by pre-warming with a no-op `rx`.

#### Outbound — as shipped

One C primitive in `modamiga.c` and a thin Python facade in
`amiga.py`:

```python
import amiga

# Default check=True: returns the host's result string (bytes) on
# rc==0, or raises OSError(rc) if the host returns a non-zero rc.
result = amiga.rexx("PPAINT.1", "ScreenToFront")  # bytes

# check=False: returns (rc, result_or_None) so the caller can
# distinguish "success with empty result" from "host returned an
# error code".
rc, result = amiga.rexx("MYHOST.1", "DoSomething", check=False)
```

The C path is the textbook send-and-wait dance documented in the
AREXX programmer's manual:

1. `FindPort(host)` → `OSError(ENOENT)` if the port isn't published.
2. Lazy `OpenLibrary("rexxsyslib.library", 0)`; cached in the same
   `RexxSysBase` the inbound side uses, so two-way scripts pay only
   one library open.
3. `CreateMsgPort()` for the private reply port; `CreateRexxMsg`
   with that as `mn_ReplyPort`.
4. `CreateArgstring` for the command bytes; stored as `rm_Args[0]`.
5. `rm_Action = RXCOMM | RXFF_RESULT` — command-level invocation,
   asks the host to populate `rm_Result2` with a result argstring.
6. `PutMsg(target, &msg->rm_Node)`.
7. `Wait(reply_sig | SIGBREAKF_CTRL_C)` until our reply port fires.
8. Unpack `rm_Result1` (rc) and `rm_Result2` (the result argstring
   pointer if rc==0); copy the bytes into Python via
   `mp_obj_new_bytes` + `LengthArgstring`.
9. `DeleteArgstring` the result and our command; `DeleteRexxMsg`;
   `DeleteMsgPort`.
10. Return `(rc, result_or_None)` as a 2-tuple.

**Ctrl+C is deferred, not aborting.** Once `PutMsg` has happened we
can't safely tear the reply port down — the host will `PutMsg` into
freed memory when it does reply.  The wait loop latches the Ctrl+C
bit but keeps spinning until the reply is in hand; cleanup runs
normally, then `KeyboardInterrupt` is raised before returning.  In
practice ARexx hosts reply within milliseconds so the visible delay
is negligible.

#### Outbound smoke test

Two MicroPython instances talking to each other over ARexx, no `rx`
or `rexxmast` involved:

- `phase18b_helper.py` opens `PYHOST.1` (inbound), receives one
  message, `eval()`s the command, replies, exits.
- `phase18b_test.py` `amiga.execute`s the helper in the background,
  retries `amiga.rexx("PYHOST.1", expr)` until the helper has
  AddPort'd, then verifies the round-trip.

Verified: `2 + 2` → `b'4'`, `'hello'.upper()` → `b'HELLO'`,
`3 * 14` → `b'42'`, `[1, 2, 3]` → `b'[1, 2, 3]'`.  Missing host →
`OSError(ENOENT)`.  `check=False` against an eval error (`1/0`,
helper replies rc=10) returns `(10, None)`.

Acceptance: a Python expression sent via `amiga.rexx(host, expr)`
to a MICROPYTHON-style host running `rexx_serve(eval)` returns the
eval'd value as bytes.

---

### Phase 19 — Workbench launch and tooltypes ✅

When an Amiga executable is launched from Workbench (by double-clicking
its `.info` icon) instead of from a Shell, it gets a `WBStartup` message
in place of `argc`/`argv`, and its configuration comes from tooltypes
stored in the `.info` file. Three things were needed: detect the launch
type, give the process a console to print into, and read tooltypes.

**Detecting the launch type.** Bebbo's `crt0.o` already does the heavy
lifting: when it sees `pr_CLI == 0` it `WaitPort`'s the process port,
`GetMsg`'s the `WBStartup`, stashes the pointer in the global `_WBenchMsg`,
and on exit `Forbid()`s and `ReplyMsg`s it back to Workbench so our segment
can be unloaded cleanly. From C we just declare it `extern struct WBStartup
*_WBenchMsg;` and test for null. No `WaitPort`/`ReplyMsg` code in `main.c`.

**Console for stdout/stderr.** A Workbench-launched process has `pr_CIS`
and `pr_COS` both `NULL`, so any `Write(Output(), ...)` would silently
drop output. `main.c` opens `CON:0/30/640/200/MicroPython/AUTO/CLOSE/WAIT`
and points the process's stdin/stdout/console-task at it. The `/AUTO`
keyword defers the window's first appearance until something is actually
written; `/WAIT` keeps it open after `main()` returns so output from a
short script doesn't flash by and vanish.

**Tooltypes.** `icon.library` `GetDiskObject(sm_ArgList[0].wa_Name)`
returns the parsed `.info` from the directory Workbench has already
`CurrentDir`'d us to. The handle is cached on the port (`IconBase` +
`amiga_wb_diskobject` in `main.c`) and exposed via `amiga.tooltype(name,
default=None)`, which is a thin wrapper around `FindToolType`. Two
tooltypes are also consumed during startup:

- `SCRIPT=<path>` — script to run instead of dropping to the REPL
- `HEAP=<N>[K|M]` / `MAXHEAP=<N>[K|M]` — heap sizing (same parser as
  `-X heap=` / `MICROPYHEAP`; env vars still win since they're more
  explicitly user-set)

A `DEBUG=` tooltype is not currently consumed — there's no port-level
debug flag yet to bind it to.

API:

```python
import amiga
if amiga.launched_from_workbench():
    # Either inspect tooltypes...
    extra = amiga.tooltype("EXTRA_PATH", "")
    # ...or iterate over shift-clicked-alongside icons.
    for path in amiga.wb_selected_files():
        process(path)
```

`amiga.tooltype()` returns the string after `=` (or `""` for a bare
tooltype with no `=`), or the supplied default (default `None`) if the
key is absent. `wb_selected_files()` returns the list of full paths for
icons shift-clicked alongside the tool — `sm_ArgList[0]` (the tool
itself) is excluded, and each remaining `WBArg` is rendered with
`NameFromLock` + `AddPart` so the caller gets ready-to-`open()` paths.

**Limitations and what's tested.** Vamos has no Workbench and no
`icon.library`, so the Phase 19 code paths can only be smoke-tested
under vamos via their CLI-launch behaviour (`launched_from_workbench()`
returns `False`, `wb_selected_files()` returns `[]`, `tooltype()` returns
the default). Full validation requires Amiberry/FS-UAE or real hardware.
`tools/amiga-mkicon.py` for generating a companion `.info` file isn't
implemented yet — for now, hand-edit the icon's tooltypes in Workbench's
"Information" requester.

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

### Phase 21 — Volume and assign introspection ✅

Three port-side C bindings in `modamiga.c` surface the AmigaDOS device
list to Python:

```python
import amiga
amiga.volumes()
# → ['Python:', 'Ram Disk:', 'Workbench:']
amiga.assigns()
# → {'C:': 'Workbench:C', 'LIBS:': 'Workbench:Libs',
#    'ENV:': 'Ram Disk:ENV', 'HELP:': 'LOCALE:Help', ...}
amiga.disk_info("SYS:")
# → (free_bytes, total_bytes, block_size)
```

Implementation:

- **`amiga.volumes()`** locks the DosList with
  `LockDosList(LDF_VOLUMES | LDF_READ)`, walks it via `NextDosEntry`,
  and copies each `dol_Name` (a BSTR — length byte followed by chars)
  into a `volume:` C string. `UnLockDosList` releases the read lock.
- **`amiga.assigns()`** does the same walk under `LDF_ASSIGNS`. The
  target path comes from `NameFromLock(dol_Lock, ...)` for standard
  (`DLT_DIRECTORY`) assigns, or from `dol_misc.dol_assign.dol_AssignName`
  for late-binding / non-binding assigns (which have no Lock).
  Multi-directory assigns are reported by their first directory only;
  the rare caller that needs the full chain can fall back to the
  shell `Assign` command.
- **`amiga.disk_info(path)`** `Lock`s the given path (suppressing
  AmigaDOS auto-requesters via `pr_WindowPtr = (APTR)-1`, so an
  unmounted volume raises `OSError(ENODEV)` instead of popping
  "Insert volume X:" in the user's face), calls `Info()` to fill an
  `InfoData`, then `UnLock`s. Returns `(free, total, block_size)`
  with the byte counts computed as `uint64_t` so volumes >4 GB
  (typical under Amiberry) report correctly.

A small `dos_errno → MP_E*` mapping table in `modamiga.c` converts
`IoErr()` values to MicroPython errno constants (`ERROR_DEVICE_NOT_MOUNTED`
→ `MP_ENODEV`, `ERROR_OBJECT_NOT_FOUND` → `MP_ENOENT`, etc.) so
Python exception handlers see sensible codes. (`vfs_amiga.c` has a
similar mapping for VFS operations; the two are kept separate rather
than coupled.)

Tested under Amiberry against an A1200 disk image: `volumes()` finds
Python:/Ram Disk:/Workbench:; `assigns()` returns 16 entries including
the well-known set (C:, LIBS:, S:, ENV:, ENVARC:, FONTS:, ...);
`disk_info("SYS:")` reports a sane `(free, total, bs)` triple; a
deliberately-bogus `disk_info("NoSuchVol:")` raises `OSError` with
errno 19 (`MP_ENODEV`).

---

### Phase 22 — AmigaDOS pattern matching ✅

AmigaDOS patterns (`#?.py`, `~(#?.bak)`, `[a-z]#?`) are richer than
POSIX glob and are what users at the AmigaShell already know. Two
port-side bindings in `modamiga.c` expose the `dos.library` matcher:

```python
import amiga
for path in amiga.match("S:#?"):       # eager list
    print(path)

for path in amiga.imatch("Work:#?.py"):  # lazy iterator
    print(path)
```

Implementation: one `AnchorPath` (the `dos/dosasl.h` struct expected
by `MatchFirst`/`MatchNext`) is `AllocVec`'d with a trailing 512-byte
buffer for the full path; `ap_Strlen` is preset to that size so the
matcher writes each result into `ap_Buf` as a null-terminated string.
`MatchFirst` parses the pattern internally — there's no separate
`ParsePattern` call.

- **`amiga.match(pattern)`** is the eager form. Loops
  `MatchFirst` → `MatchNext` while the return code is 0, appending
  each `ap_Buf` to a Python list. `MatchEnd` and `FreeVec` always
  run before return. Return codes `ERROR_NO_MORE_ENTRIES` (no more
  matches) and `ERROR_OBJECT_NOT_FOUND` (path/volume missing) are
  treated as "empty list" rather than raising; any other DOS error
  becomes `OSError`.
- **`amiga.imatch(pattern)`** is the iterator form, built on the
  same `mp_type_polymorph_iter_with_finaliser` pattern that
  `vfs_amiga.c`'s `ilistdir` uses. The iterator owns the
  `AnchorPath`; its `iternext` calls `MatchNext` and yields
  `ap_Buf`; its finaliser (`__del__`, run on GC of the iterator)
  calls `MatchEnd` + `FreeVec` so an abandoned loop —
  `for p in amiga.imatch(...): break` — doesn't leak the anchor.
  `MatchFirst` runs eagerly inside `imatch()` itself so the first
  result is ready on the very first `next()` call.

Both calls suppress AmigaDOS auto-requesters around the matcher
(via `pr_WindowPtr = (APTR)-1`) so an unmounted volume in the
pattern just produces an `OSError` instead of an interactive
"Insert volume" dialog.

Tested under Amiberry: `match("S:#?")` finds the 9 expected
startup-script files (Shell-Startup, user-startup, PCD, etc.);
`imatch("S:#?")` yields the same 9 paths one at a time with the
correct first result; `imatch` aborted via `break` + `gc.collect()`
runs the finaliser without leaks; `imatch("NoSuchVol:#?")` raises
`OSError(ENODEV)`.

---

### Phase 23 — `timer.device`-backed timing ✅

The Phase 8 section noted that `mp_hal_delay_us` and `mp_hal_ticks_*` still
used newlib `clock()` — busy-wait, millisecond-ish resolution. AmigaOS's
`timer.device` MICROHZ unit gives microsecond accuracy and doesn't
busy-wait. Replaced the `clock()`-based path with `timer.device` for:

- `mp_hal_delay_us(n)` → `timer.device` `TR_ADDREQUEST` with
  `tv_secs=us/1e6, tv_micro=us%1e6` via `DoIO()` for `≥200 µs`, and a
  tight `ReadEClock()` busy-loop for `<200 µs` (where the IORequest
  round-trip dominates).
- `mp_hal_delay_ms(n)` likewise routes through `timer.device` for
  arbitrary-millisecond accuracy (the previous `dos.library Delay()` had
  20 ms granularity).
- `mp_hal_ticks_us()` / `mp_hal_ticks_ms()` → `ReadEClock()` (EClock
  hardware counter, sub-microsecond, monotonic, cheap to read). The
  EClock frequency is cached at init so the hot path is one `ReadEClock`
  plus a 64-bit divide.
- `time.sleep_ms` / `sleep_us` get accuracy as a side effect since they
  route through these HAL hooks.

Setup: `CreateMsgPort` + `CreateIORequest` + `OpenDevice("timer.device",
UNIT_MICROHZ, ...)` once at startup (`amiga_timer_open()` in
`amiga_timer.c`, called from `main()` just before `mp_init()`); teardown
after `mp_deinit()`. The IORequest object goes in a port-local static;
not thread-safe but the port is single-threaded. `TimerBase` (referenced
by the bebbo `proto/timer.h` inlines) is also set in `amiga_timer.c` from
the request's `io_Device` after open.

Acceptance verified on Amiberry 2026-05-11: `time.sleep_us(500)` waits
~500 µs (measured ~1250 µs end-to-end after subtracting the Python-to-C
round-trip overhead, well above the old 0 µs floor and below the
1000 µs/`Delay()` granularity); `time.ticks_diff` between two
`ticks_us()` calls returns a stable microsecond delta (~270–580 µs per
call on emulated 68020); `time.sleep_us(50000)` measured 50750 µs
(microsecond-accurate, via `timer.device`); `time.sleep(1)` measured
1000 ms exactly.

---

### Phase 24 — Persistent REPL history ✅

Phase 13's REPL line-editing kept history only in memory; this phase
adds load-on-startup / save-on-exit so ↑ recalls commands from a
previous session.

**Path.** Defaults to `S:MicroPython.history` (not `ENVARC:` — that's
for preferences, and dropping a frequently-written log there would
make every reboot's `copy ENVARC: ENV: all` slower).  `S:` is the
conventional AmigaOS spot for shell-style dot-files; the binary
sits there alongside `Shell-Startup` and `user-startup`.  Override
with the `MICROPYHISTORY` env-var (read via dos.library `GetVar` —
same pattern as Phase 14's `MICROPYHEAP`), so a user can keep
per-script histories or relocate to `RAM:`/`T:` if they want
session-only.

**Format.** One entry per line, oldest first, plain AmigaDOS line
endings.  Old files with more than `MICROPY_READLINE_HISTORY_SIZE`
entries truncate naturally on the next save (newest ones win since
the ring overwrites tail-end first).  CRLF tolerated on read so a
file authored from the host side still loads cleanly.

**Sizing.** `MICROPY_READLINE_HISTORY_SIZE` was 8 (the upstream
default); bumped to 32 in `mpconfigport.h` since disk storage is
free and 8 entries gets reused inside a single editing session.

**Implementation.**

- `ports/amiga/amiga_history.c` contains the two entry points
  (`amiga_history_load` / `amiga_history_save`), the
  `MICROPYHISTORY` resolution, and a tiny `Read`-one-line helper
  that handles CRLF.  Both paths suppress AmigaDOS auto-requesters
  via `pr_WindowPtr = (APTR)-1` so an unmounted `S:` volume just
  produces a silent no-op rather than popping "Insert volume" in
  the user's face.
- `ports/amiga/main.c` calls `amiga_history_load()` immediately
  after `mp_init()` (so `MP_STATE_PORT(readline_hist)` is alive)
  and `amiga_history_save()` immediately before `mp_deinit()` (so
  the ring is still valid).
- `modamiga.c` exposes two thin accessors for scripting:
  `amiga.readline_history() -> tuple[str, ...]` (most recent first)
  and `amiga.readline_push_history(line)`.  Useful for inspection
  ("what did I just type?"), for pre-seeding a session from a
  script, and — pragmatically — for testing this phase without
  needing an interactive REPL.

**Smoke test (under Amiberry).** A two-step boot script:

1. `SetEnv MICROPYHISTORY py0:phase24_hist.txt` then launches a
   script that pushes three entries and exits.  After exit, the
   on-disk file contains the three entries oldest-first:
   `x = 1\ny = 'hello'\nprint(x, y)`.
2. Relaunches a second script that prints
   `amiga.readline_history()` — comes back as
   `('print(x, y)', "y = 'hello'", 'x = 1')` (most-recent-first
   matching how `readline_push_history` builds the ring) — then
   pushes one more entry.  After this exit, the file is now four
   lines, ending with the newly pushed entry.

Failure cases (no `S:` volume; read-only filesystem; corrupt file)
are silent: history just doesn't persist that session.  A startup
error here would be more annoying than the missing-history symptom
it'd flag.

---

### Phase 25 — Extra break signals ✅

AmigaOS gives every task four user-defined break signals
(`SIGBREAKF_CTRL_C/D/E/F`); Ctrl+C is already wired to the
KeyboardInterrupt handler in `mphalport.c`.  This phase exposes the
remaining three as a cheap cooperative-IPC primitive — one task
signals another's task pointer, no port / message setup needed:

```python
import amiga
amiga.signal(other_task_addr, amiga.SIGBREAKF_CTRL_E)
mask = amiga.wait_signal(amiga.SIGBREAKF_CTRL_D | amiga.SIGBREAKF_CTRL_E,
                         timeout_ms=5000)
```

Two new C bindings in `modamiga.c`:

- **`amiga.signal(task_addr, sigmask)`** wraps `exec.library` `Signal()`
  directly.  A NULL task pointer raises `ValueError` (cheap guard against
  signalling a freshly-`Removed` task).
- **`amiga.wait_signal(mask, timeout_ms=None)`** wraps `Wait()`, with
  three guarantees: `SIGBREAKF_CTRL_C` is **always** ORed into the
  internal mask (so the user can break out of an otherwise-infinite
  wait) but is **always** stripped from the returned value (so Ctrl+C
  never spuriously satisfies a user's signal); if Ctrl+C does fire,
  `KeyboardInterrupt` is raised instead of being returned.

The `timeout_ms` path is the only non-trivial bit.  AmigaOS `Wait()`
doesn't take a timeout, so `amiga_timer.c` now also opens a **second,
async** `timer.device` IORequest dedicated to this purpose (kept
separate from the Phase 23 synchronous IORequest so a pending Wait can't
collide with an in-flight `mp_hal_delay_us` call).  `wait_signal` arms
the async timer via `SendIO`, ORs the MsgPort's signal bit into the
Wait mask, then aborts/drains the request on return.  If the second
timer port can't be opened at startup, `wait_signal` falls back to an
untimed Wait — documented best-effort.

Smoke-tested under Amiberry:

- Self-signal round-trip: `signal(me, CTRL_E)` followed by
  `wait_signal(CTRL_E)` returns `0x4000` in ~1 ms.
- Two-signal OR: `signal(me, CTRL_F)` + `wait_signal(CTRL_E | CTRL_F)`
  returns `0x8000` (the bit that actually fired).
- Pure timeout: `wait_signal(CTRL_D, timeout_ms=120)` returns 0 after
  ~121 ms (timer.device-accurate).
- Early wake: pre-signal CTRL_E then `wait_signal(CTRL_E, timeout_ms=5000)`
  returns immediately (~1 ms) with the right mask — confirms the async
  IORequest is correctly aborted on the fast path.
- Ctrl+C filtering: even if the caller passes `SIGBREAKF_CTRL_C` in
  their mask, the returned value has bit 12 cleared.

---

### Phase 26 — `PROGDIR:` on `sys.path` ✅

Standard Amiga pattern: the executable's own directory is referenced as
`PROGDIR:`, an automatic assign created by AmigaDOS at launch and
unique per process.  After `mp_init()` populates `sys.path = [""]`, the
port appends `"PROGDIR:"`, so a user can drop `.py` / `.mpy` files next
to the `micropython` binary and import them without further setup.

```c
mp_init();
// ...
mp_obj_list_append(mp_sys_path, MP_OBJ_NEW_QSTR(qstr_from_str("PROGDIR:")));
```

Implementation note: the original plan called for *prepending*
PROGDIR: to `sys.path`, but the existing positional-script handling
already replaces `sys.path[0]` with the script's directory (matching
CPython's `sys.path[0] = abs_script_dir` convention).  Prepending
PROGDIR: would lose it on every script run.  Appending puts it after
the cwd/script-dir + `.frozen` placeholder, which is the correct
import priority anyway: a same-directory script wins, with PROGDIR:
as a final fallback.

Smoke-tested under Amiberry: after launching `py0:phase2526_test.py`,
`sys.path` is `['py0:', '.frozen', 'PROGDIR:']` — script dir first,
frozen-modules placeholder second, PROGDIR: appended last.

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
| `minimal` | `-m68020 -msoft-float` | 128 KB | Stock unaccelerated A1200 (68EC020, 2 MB Chip, no Fast). Strips `bsdsocket` module and `MICROPY_EMIT_68K`. Note: A500 isn't a target — its 68000 lacks unaligned access. |
| `68020fpu` | `-m68020 -m68881` | 512 KB | 68020/68030 with 68881/68882 FPU coprocessor (A2620/A2630, A3000). FPU compares and conversions are emitted directly, so the libgcc soft-float wraps from `floatconv.c` aren't applied. |
| `68040` | `-m68040` | 1 MB | 68040 with built-in FPU (A3640, A4000/040, Blizzard 1240). Same float-wrap story as `68020fpu`. |

The libm `pow` / `tgamma` wraps in `floatconv.c` apply unconditionally on
all variants — those are math-library bugs in clib2/libnix, unrelated to
FPU codegen.

Build sizes (text segment, `size` output):

```
standard   355 848 bytes
minimal    327 080 bytes   (-29 KB: no socket, no native emitter)
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
- With Phase 14 in place, the per-variant `MICROPY_HEAP_SIZE` is the
  *initial* size only; users can override per invocation with
  `-X heap=<N>[K|M]` and cap growth with `-X maxheap=<N>[K|M]`.

---

### Phase 28 — TLS/SSL via AmiSSL v5

Phase 9 landed plain TCP via `bsdsocket.library`; the obvious next
step on the networking front is TLS. Upstream MicroPython ships two
`ssl` backends in `extmod/`: axTLS (small, dated, no SNI/hostname
verification worth trusting) and mbedTLS (modern but a ~250 KB code
add plus another C dependency to cross-compile for 68k). Neither fits
the AmigaOS ethos when the platform already has a perfectly serviceable
TLS implementation — **AmiSSL** — as a shared library that any
application can dynamically open. The TLS code stays on the user's
machine where every other TLS client can share it; we ship only the
thin Python-facing wrapper.

AmiSSL is the AmigaOS port of OpenSSL. The v5.x line is based on
OpenSSL 3.x, supports TLS 1.3, and is exposed through `amisslmaster.library`
plus a versioned `amissl.library`. The master indirection lets the
system carry multiple OpenSSL major versions simultaneously and pick
the right one at open time — so an A1200 with v4 (OpenSSL 1.1.x) and
v5 (OpenSSL 3.x) installed side-by-side can host both legacy
applications and this port. We target v5 only: modern ciphers,
ChaCha20-Poly1305 AEAD, proper TLS 1.3, the simpler `TLS_method()` /
`TLS_client_method()` selectors, and provider-based crypto. v4 is
OpenSSL 1.1.x — useful but EOL upstream and not worth carrying a
deprecated-API codepath for.

#### How AmiSSL is opened (not stock `OpenLibrary`)

Unlike `bsdsocket.library`, AmiSSL has a master-indirection step. The
v5 SDK exposes a single unified `OpenAmiSSLTags()` entry point that
replaces the legacy v3/v4 `InitAmiSSLMaster()` + `OpenAmiSSL()` +
`InitAmiSSL()` triple — one call does everything for the main task:

```c
#include <proto/amisslmaster.h>
#include <proto/amissl.h>
#include <libraries/amisslmaster.h>
#include <libraries/amissl.h>

AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                               AMISSLMASTER_MIN_VERSION);
OpenAmiSSLTags(AMISSL_CURRENT_VERSION,                  // positional
               AmiSSL_UsesOpenSSLStructs, FALSE,        // opaque-only
               AmiSSL_GetAmiSSLBase,      &AmiSSLBase,
               AmiSSL_GetAmiSSLExtBase,   &AmiSSLExtBase,  // OS3 split
               AmiSSL_SocketBase,         (ULONG)SocketBase,
               AmiSSL_ErrNoPtr,           (ULONG)&errno,
               TAG_DONE);

// ... use normal OpenSSL APIs ...

CloseAmiSSL();
CloseLibrary(AmiSSLMasterBase);
```

Four things worth noting:

- **`AMISSL_CURRENT_VERSION` is positional**, not a tag — it's the
  first arg to `OpenAmiSSLTags`. With the v5 SDK, the macro resolves
  to the v5 / OpenSSL 3.x constant. Compiling against the v5 SDK
  produces a binary that *requires* an installed AmiSSL of matching
  major; the master library refuses the open otherwise. That's the
  whole point of the master indirection.
- **`AmiSSL_UsesOpenSSLStructs = FALSE`** because MicroPython treats
  `SSL_CTX *` / `SSL *` / `X509 *` as opaque pointers and never
  reaches into struct internals. (The SDK suggests `TRUE` if unsure —
  we *are* sure.)
- **`AmiSSLExtBase` is OS3-only.** v5's OpenSSL 3.x has so many
  functions that on m68k they had to split the call surface across
  two library bases; OS4 keeps a single base. We're m68k-only here,
  so the extra base is required to reach the full v5 API.
- **`AmiSSL_SocketBase` is supplied in the same call.** The legacy
  `InitAmiSSL()` step is folded into `OpenAmiSSLTags` when the open
  happens on the main task, so the bsdsocket binding still has to
  happen *after* `amiga_socket_open()` populated `SocketBase` — i.e.
  `amiga_ssl_open()` must run later in `main()`. `errno` is per-task
  too, so the `AmiSSL_ErrNoPtr` tag aims it at our task's slot.

MicroPython is single-tasked (`MICROPY_PY_THREAD (0)`), so we never
need the `InitAmiSSL()` / `CleanupAmiSSL()` pair that subprocesses
sharing the same base require — `OpenAmiSSLTags` / `CloseAmiSSL`
brackets the whole lifetime cleanly.

The build links against the AmiSSL SDK in one of two flavours:

- **`-lamisslstubs`** — provides function stubs for every OpenSSL
  function so callbacks that take an OpenSSL function pointer (e.g.
  `sk_X509_pop_free(..., X509_free)`) link cleanly. Without this,
  passing `X509_free` as a function pointer fails to link because
  the symbol lives behind the library indirection. The wrapper code
  for `wrap_socket` doesn't strictly need stubs, but anything that
  reaches deeper into OpenSSL idioms does — including the verify
  callback we'll likely register from `modssl.c`.
- **`-lamisslauto`** — does the open/init dance automatically at
  program startup via constructor magic, transparent to the program.
  Convenient for client apps but conflicts with our explicit
  `amiga_ssl_open()` (which has to fire after `amiga_socket_open()`
  for `SocketBase` to be valid). We don't use this.

Header scheme is straight OpenSSL: `#include <openssl/ssl.h>`,
`<openssl/err.h>`, `<openssl/x509v3.h>` — same as a desktop build,
no `amissl/...` prefix needed in v5.

#### Callback ABI on m68k

OS3 callbacks under AmiSSL are called with arguments on the stack
(not in registers, unlike a normal m68k Amiga library call), and
they need `STDARGS SAVEDS` annotations so gcc emits the right
prologue/epilogue:

```c
STDARGS SAVEDS static int amiga_ssl_verify_cb(int preverify_ok,
                                              X509_STORE_CTX *ctx)
{
    return preverify_ok;
}
SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, amiga_ssl_verify_cb);
```

bebbo gcc has `__attribute__((stkparm))` for the calling convention
and `__saveds` for the base-register reload (a4-relative globals).
Worth defining a `AMISSL_CB` macro in `amiga_ssl.h` so every
callback site uses the same set of annotations.

#### Module shape

`ports/amiga/modssl.c` registers an `ssl` module that follows the
upstream `ssl` API surface, so user code stays portable between this
port, CPython, and other MicroPython targets:

```python
import ssl, socket

ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.verify_mode = ssl.CERT_REQUIRED
ctx.load_verify_locations("LIBS:amissl/certs/cacert.pem")

s = socket.socket()
s.connect(("www.example.com", 443))
ws = ctx.wrap_socket(s, server_hostname="www.example.com")
ws.write(b"GET / HTTP/1.1\r\nHost: www.example.com\r\n"
         b"Connection: close\r\n\r\n")
print(ws.read(4096))
ws.close()
```

Two Python-visible types:

- `ssl.SSLContext` — wraps `SSL_CTX *`. Carries verify mode, CA trust
  store, SNI hostname, optional ALPN list. `wrap_socket()` returns…
- `ssl.SSLSocket` — wraps `SSL *` plus the underlying socket fd.
  Implements the MicroPython stream protocol (read / write / ioctl)
  so it slots into `readline()`, `with` statements, and the file-like
  helpers in `extmod/` — same shape as Phase 9's socket object.

Per-socket lifecycle: `SSL_new(ctx)`, `SSL_set_fd(ssl, sock->fd)`,
`SSL_set_tlsext_host_name(ssl, hostname)` for SNI, `SSL_connect(ssl)`
on the client side, then `SSL_read` / `SSL_write` for I/O, and
`SSL_shutdown` + `SSL_free` on close. Errors come back via
`SSL_get_error(ssl, ret)`; transient `SSL_ERROR_WANT_READ` /
`SSL_ERROR_WANT_WRITE` map to `MP_EAGAIN` on a non-blocking socket
(or to a blocking retry on a blocking one), terminal errors raise
`OSError` with the OpenSSL string from `ERR_error_string` so the
user sees `OSError: certificate verify failed: self signed
certificate` and not just `OSError: -1`.

#### Certificate trust store

OpenSSL won't validate a chain without a trust store, and AmigaOS
has no system-wide one. Two complementary mechanisms:

1. **Default search path.** `SSL_CTX_set_default_verify_paths()`
   picks up `LIBS:amissl/certs/cacert.pem` (a Mozilla CA bundle the
   AmiSSL installer drops there). Most users already have this from
   installing AmiSSL itself.
2. **Explicit.** `ctx.load_verify_locations(cafile=...)` and
   `ctx.load_verify_locations(cadata=...)` for users who want to
   bundle their own roots or pin to a constrained set.

Don't ship a CA bundle in the MicroPython binary — adds ~200 KB to
every variant, gets stale fast, and AmiSSL's bundle is the right
shared resource. Document the path in the phase notes so users
hitting `OSError: unable to get local issuer certificate` have the
obvious lever.

#### Module size and variant gating

The wrapper module is small (~600 lines C, estimated ~10 KB text),
but it hard-depends on `MICROPY_PY_AMIGA_SOCKET` — `SSL_set_fd`
needs a bsdsocket descriptor. Add:

```c
#ifndef MICROPY_PY_AMIGA_SSL
#define MICROPY_PY_AMIGA_SSL                (MICROPY_PY_AMIGA_SOCKET)
#endif
```

so the `minimal` variant (no socket on a stock A1200) also gets no
SSL, while `standard` / `68020fpu` / `68040` build it in. If
`amisslmaster.library` isn't installed at runtime the master-open
fails and the module self-disables — `import ssl` then raises
`ImportError("amisslmaster.library not found")` rather than crashing
or returning a half-initialised module.

#### Memory shape

OpenSSL 3.x is comfortable with memory. `SSL_CTX` is ~16 KB once
the default cipher list and verify store are populated; each
`SSL` instance is ~48 KB (TLS 1.3 record buffers, key schedule,
read/write BIOs). Two simultaneous HTTPS connections plus a CA
store loaded from `cacert.pem` (~200 KB resident) pushes a stock
256 KB `standard` heap right to the edge — the AmiSSL allocations
come from system memory via `MEMF_ANY`, not MicroPython's GC heap,
so it's the *system* free pool that matters, but the symptom on a
2 MB Chip-only machine is identical: `OSError: out of memory`
mid-handshake. The lever: `-X heap=` doesn't help (different
pool), but `Avail FAST` showing low → close other apps or build
the `68020fpu` / `68040` variant which assumes a Fast-RAM-equipped
machine. Document this in the phase write-up.

#### What we deliberately *don't* do

- **Server-side TLS.** Mechanically straightforward (`TLS_server_method`,
  cert + key load, `SSL_accept`), but the Amiga isn't a webserver
  host in practice. Land client-side first; server is a small
  additional surface if anyone actually asks.
- **OpenSSL config-file integration.** OpenSSL 3.x reads `openssl.cnf`
  for provider configuration; AmiSSL ships a sane default and we
  don't expose tuning knobs to Python.
- **`asyncio.start_server` / non-blocking handshake plumbing.**
  Asyncio isn't enabled on this port (`MICROPY_PY_ASYNCIO (0)` in
  `mpconfigport.h`); a future phase that lights up the scheduler
  will revisit `SSLSocket.ioctl(MP_STREAM_POLL)` then. For now,
  blocking handshakes only.
- **Touching upstream `extmod/modssl_*.c`.** AmiSSL-only — this is
  a port-side module under `ports/amiga/modssl.c`, leaving the
  upstream axtls / mbedtls backends untouched. Same separation
  rule as the rest of this port.

#### File structure

```
ports/amiga/modssl.c          — module def, SSLContext, SSLSocket
ports/amiga/amiga_ssl.c       — AmiSSL master open / init / cleanup
ports/amiga/amiga_ssl.h       — shared prototypes + base pointers
```

`main()` calls `amiga_ssl_open()` after `amiga_socket_open()` and
`amiga_ssl_close()` before exit; both are no-ops if AmiSSL isn't
installed. `modsocket.c` and `modssl.c` share no symbols beyond
`SocketBase` (passed through the `AmiSSL_SocketBase` tag to
`OpenAmiSSLTags`).

SDK reference: the AmiSSL v5 developer documentation lives at
[`jens-maus/amissl:dist/README-SDK`](https://github.com/jens-maus/amissl/blob/master/dist/README-SDK)
— authoritative for the exact tag set, the OS3/OS4 split, and the
`STDARGS SAVEDS` callback convention. The `Examples/httpget.c` and
`Examples/https.c` programs in the SDK distribution are minimal
working clients worth mirroring.

#### Testing

- **Module-load smoke.** `import ssl; print(ssl.SSLContext)` works
  even without AmiSSL installed — only `wrap_socket` / `SSLContext()`
  construction needs the library, so a stripped Workbench can still
  load the module.
- **Handshake smoke under Amiberry.** HTTPS GET against the `badssl.com`
  family — `expired.badssl.com`, `self-signed.badssl.com`,
  `wrong.host.badssl.com` — exercises verify-failure paths. The
  `boot` hook drives this unattended and the test grep's `rx_*.log`
  for the expected `OSError: ... certificate verify failed` /
  `Hostname mismatch` strings.
- **Live target.** GET `https://www.example.com/` and confirm the
  body comes back with `Connection: close` working cleanly through
  `SSL_shutdown`.
- **Upstream `tests/extmod/ssl_*.py`.** Most require asyncio; the
  ones that just construct `ssl.SSLContext()` and call
  `wrap_socket()` against a real server are the ones to enable in
  the test-suite snapshot. Update `docs/amiga.md`'s
  *Recommended test directories* once the actual pass/skip
  breakdown is known.

#### Open items

- **AmiSSL 4 fallback.** If a target has only AmiSSL v4, the master
  open with v5 constants fails. Decide: error out cleanly (v5-only,
  recommend SDK upgrade) or carry a v4 codepath. Land v5-only
  initially; revisit if anyone with a v4-locked setup asks.
- **Cert pinning helpers.** `set_servername_callback` and the
  raw-public-key APIs OpenSSL 3 exposes are useful for IoT-style
  scripts on an Amiga reaching out to a fixed peer. Punt to a
  follow-up phase.
- **Async-friendly handshake.** Tied to the asyncio gating above.
- **`urequests` / `mip`.** Once `ssl` lands, the upstream `urequests`
  recipe and the `mip` package installer become usable — both are
  pure-Python and just need an `ssl.wrap_socket` that works. Worth
  freezing into the variant manifests as a follow-up.

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
AMIGA_VARIANT=minimal ./run-tests.py -d basics    # --cpu 68020,  2 MiB RAM
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

Latest snapshot under vamos via `tools/amiga-vamos-run.sh` —
**re-run 2026-05-12** after Phase 17 steps 3–6, Phase 18 inbound and
outbound, Phase 25, Phase 26, and the tag-list follow-up (parse_taglist,
TAG_MORE, full NDK tag harvest):

| Directory | Files | Pass | Self-skip | Fail | Notes |
|-----------|------:|-----:|----------:|-----:|-------|
| `basics/`     | 574 | 490 | 83  | 1  | `struct1.py` (bebbo ABI alignment) |
| `float/`      | 68  | 54  | 11  | 3  | EXACT-mode precision at double-range edges |
| `io/`         | 16  | 12  | 3   | 1  | `argv.py` (vamos host-path rewriting) |
| `import/`     | 30  | 29  | 0   | 1  | `import_file.py` (vamos host-path rewriting) |
| `micropython/`| 108 | 43  | 18  | 47 | All native_*/viper_* — Phase 12 |
| `extmod/`     | 205 | 67  | 131 | 7  | vamos socket / select / time-quantum / vfs_userfs gaps |
| `misc/`       | 14  | 6   | 8   | 0  | Skips: settrace, sys_exc_info, cexample (build-time module) |
| `cmdline/`    | 25  | 9   | 2   | 14 | Unix-port-specific (REPL banner, `-v`, terminal editing) |
| `stress/`     | 13  | 12  | 0   | 1  | `bytecode_limit.py` parser memory pressure |

Aggregate (this run, covering the directories listed above):
**722 pass / 256 self-skip / 75 fail** out of 1053 test files.
Adding `extmod/` to the prior baseline accounts for 67 new passes,
131 new self-skips (mostly asyncio / cryptolib / machine_* / vfs_lfs /
ssl/tls features the port doesn't compile in), and 7 new fails — all
host-side vamos limitations, not port code.

Net change from the previous snapshot (excluding `extmod/`, which
wasn't in that run): **+1 pass in `import/`** (`builtin_ext.py` now
passes — the `from uos import *` namespacing was tightened upstream)
and **+1 pass / -1 self-skip in `micropython/`** (a previously
filtered test now runs cleanly).  No new failures introduced by
recent phase work.

Excluding the 47 Phase-12 native/viper failures, the unix-port-specific
cmdline tests, vamos host-emulation gaps (extmod sockets/timers,
import/io path quirks), and the deferred `bytecode_limit.py` parser
edge case, **4 individual tests fail** — all real platform differences
documented in *Known test failures* below.

### Recommended test directories

| Directory | Tests | Expected result |
|-----------|-------|----------------|
| `basics/` | 491 | 490 pass, 83 self-skip (mostly intbig-only and threading), `struct1.py` fails (see below) |
| `float/` | 57 | 54 pass, 11 self-skip (mostly intbig-only); 3 fail on EXACT-mode precision near double range edges (see below) |
| `io/` | 16 | 12 pass, 3 self-skip (`os.remove`, `sys.std*.buffer`), `argv.py` fails (see below) |
| `micropython/` | 108 | 43 pass, 18 self-skip; 47 fail in `native_*` and `viper_*` (Phase 12) |
| `misc/` | 14 | 6 pass, 8 self-skip (settrace, sys_exc_info, cexample build-time module) |
| `cmdline/` | 25 | 9 pass, 2 self-skip, 14 fail — most failures are unix-port-specific (REPL banner format, `-v` bytecode dump, terminal-editing). `-X compile-only`, `-O`, `-m` SystemExit handling, and `sys.atexit` all pass. |
| `import/` | 30 | 29 pass; `import_file.py` fails for the same vamos path-rewriting reason as `io/argv.py`. |
| `extmod/` | 205 | 67 pass, 131 self-skip (asyncio, cryptolib, framebuf, machine_*, ssl/tls, vfs_lfs/fat, etc. — features not compiled in or not present), 7 fail (vamos's POSIX shim doesn't fully cover sockets, `select.poll`, the `time.time_ns` quantum, or `vfs_userfs`). |
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
| 14 — Dynamic heap / runtime sizing | ✅ Done | `MICROPY_GC_SPLIT_HEAP_AUTO` enabled; initial size from `-X heap=<N>[K\|M]` or `MICROPYHEAP` env, default `MICROPY_HEAP_SIZE`; growth via `AllocVec` chunks tracked port-side and `FreeVec`'d at exit; `-X maxheap=<N>`/`MICROPYHEAPMAX` cap; `amiga.heap_info()` reports `(total, free, num_arenas)` |
| 15 — Memory-pool API | Withdrawn | Dynamic GC heap (Phase 14) covers the common case; may be revisited as part of Phase 17's native-library trampoline |
| 16 — VFS / `os.*` file I/O | ✅ Done | `MICROPY_VFS=1` with port-local `VfsAmiga` in `vfs_amiga.c` wrapping `dos.library`; `os.chdir`/`listdir`/`stat`/etc. work; `amigafile.c` and `amigaio.c` removed; on-device test runner uses `os.chdir` |
| 17 — Native library access | Planned | `amiga.lib_open` / `lib_close` / `lib_call` asm trampoline + `.fd`-driven `amiga.library(...)` proxy; unlocks every system and third-party library without per-library C bindings |
| 18 — ARexx integration | ✅ Done | Inbound: `amiga.rexx_open/close/recv/reply` + `RexxMessage` facade.  Outbound: `amiga.rexx(host, cmd)` (raises `OSError(rc)` on non-zero rc with `check=True`, returns `(rc, result)` with `check=False`).  Result strings allocated via `rexxsyslib.library` `CreateArgstring`; lazy-opened so a missing library is harmless until first used.  Ctrl+C deferred (not aborting) once a message is in flight |
| 19 — Workbench launch / tooltypes | ✅ Done | `amiga.launched_from_workbench()` / `wb_selected_files()` / `tooltype(name[, default])` in `modamiga.c`; bebbo crt0 does `WaitPort`+`GetMsg`+`ReplyMsg` so `main.c` just reads `_WBenchMsg`; `CON:0/30/640/200/MicroPython/AUTO/CLOSE/WAIT` opened as stdin/stdout when WB-launched; `SCRIPT=` tooltype runs that path on startup; `HEAP=`/`MAXHEAP=` tooltypes feed the heap-sizing pre-scan (lower priority than env vars / `-X`); cannot be exercised under vamos (no Workbench, no `icon.library`) |
| 20 — Env-var integration | ✅ Done | `os.getenv`/`os.putenv`/`os.unsetenv` via `GetVar`/`SetVar` (flags=0, local vars; matches Unix `os.putenv` semantics) in `ports/amiga/modos.c` (included via `MICROPY_PY_OS_INCLUDEFILE`); two vamos bugs worked around — see Phase 20 |
| 21 — Volume / assign introspection | ✅ Done | `amiga.volumes()` / `assigns()` / `disk_info()` in `modamiga.c` via `LockDosList`+`NextDosEntry` and `Info()`; auto-requesters suppressed; `disk_info` returns 64-bit byte counts for >4 GB volumes; `IoErr()` mapped to `MP_E*` (incl. `ENODEV` for unmounted volumes) |
| 22 — AmigaDOS pattern matching | ✅ Done | `amiga.match("#?.py")` (eager list) and `amiga.imatch(...)` (iterator) in `modamiga.c` via `MatchFirst`/`MatchNext`/`MatchEnd`; iterator uses `mp_type_polymorph_iter_with_finaliser` so an abandoned loop cleans up the `AnchorPath` on GC |
| 23 — `timer.device` timing | ✅ Done | `mp_hal_delay_us` uses `timer.device` MICROHZ (`DoIO`) for ≥200 µs and a tight `ReadEClock()` loop below; `mp_hal_ticks_us/ms` read EClock with a cached frequency; `mp_hal_delay_ms` no longer goes via `dos.library Delay()` (20 ms granularity); setup/teardown in `amiga_timer.c` around `mp_init/deinit` |
| 24 — Persistent REPL history | ✅ Done | `amiga_history.c` loads + saves `S:MicroPython.history` (overridable via `MICROPYHISTORY` env-var); ring size bumped to 32; `amiga.readline_history()` / `amiga.readline_push_history()` expose the ring for scripting; silent no-op when the path isn't writable |
| 25 — Extra break signals | ✅ Done | `amiga.signal` + `amiga.wait_signal(mask, timeout_ms=N)` in `modamiga.c`; timeout uses a dedicated async `timer.device` port set up in `amiga_timer.c`; Ctrl+C always ORed into Wait mask but stripped from the return — never satisfies a user signal — and raises KeyboardInterrupt when it fires |
| 26 — `PROGDIR:` on `sys.path` | ✅ Done | `main.c` appends `"PROGDIR:"` to `sys.path` after `mp_init()`; appended (not prepended) so the existing script-dir-at-[0] machinery still works |
| 27 — Build variants | ✅ Done | `make VARIANT=<name>` with `build-<name>` output dir; `standard` (default, 68020 soft-float), `minimal` (trimmed for stock unaccelerated A1200), `68020fpu` (68020 + 68881 coprocessor), `68040` (68040 with built-in FPU); FPU variants skip libgcc soft-float wraps |
