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
- Phase 6 — Package imports (`import mypackage`) via `Lock`/`Examine`
- Phase 7 — Ctrl+C interrupt handling via dos.library break signals
- Phase 8 — Networking via `bsdsocket.library` (`usocket` module)
- Phase 9 — CI build workflow

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
| Memory allocation | Static BSS array | `exec.library`: `AllocVec(size, MEMF_FAST)` / `FreeVec()` |
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
void *heap = AllocVec(MICROPY_HEAP_SIZE, MEMF_FAST | MEMF_PUBLIC);
if (!heap) heap = AllocVec(MICROPY_HEAP_SIZE, MEMF_ANY | MEMF_PUBLIC);
gc_init(heap, (char *)heap + MICROPY_HEAP_SIZE);
```

For GC root scanning, the current port uses the same local-variable heuristic as `ports/minimal`. Once the NDK is available, replace with exact stack bounds from exec:

```c
struct Task *task = (struct Task *)FindTask(NULL);
gc_collect_root((void **)task->tc_SPLower,
    ((char *)task->tc_SPUpper - (char *)task->tc_SPLower) / sizeof(void *));
```

---

## Port file structure

```
ports/amiga/
├── Makefile              # Build rules and toolchain config
├── mpconfigport.h        # Feature flags and type definitions
├── mphalport.h           # Inline HAL stubs (ticks, interrupt char)
├── mphalport.c           # Console I/O HAL
├── main.c                # Entry point, gc_collect(), required stubs
├── amigafile.c           # mp_lexer_new_from_file(), mp_import_stat()
├── amigaio.c             # mp_builtin_open_obj (FileIO / TextIOWrapper)
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

### Configuration

```c
// mpconfigport.h
#define MICROPY_EMIT_68K (1)
```

---

## Future Work

### Phase 6 — Package imports

`mp_import_stat()` currently uses `fopen` to detect files, which cannot distinguish
directories from non-existent paths. `import mypackage` (where `mypackage/` is a
directory containing `__init__.py`) will always fail with `ModuleNotFoundError`.

Fix: replace the `fopen` probe in `amigafile.c` with dos.library `Lock`/`Examine`:

```c
mp_import_stat_t mp_import_stat(const char *path) {
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    if (!lock) return MP_IMPORT_STAT_NO_EXIST;
    struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
    mp_import_stat_t r = MP_IMPORT_STAT_NO_EXIST;
    if (fib && Examine(lock, fib))
        r = (fib->fib_DirEntryType > 0) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
    if (fib) FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    return r;
}
```

NDK is already installed at `/opt/amiga/m68k-amigaos/ndk-include/` — no extra flags needed.

---

### Phase 7 — Ctrl+C interrupt handling

Currently Ctrl+C has no effect during a running script. AmigaOS delivers break signals
via `CheckSignal(SIGBREAKF_CTRL_C)` rather than SIGINT.

Fix: in `mphalport.h`, implement `mp_hal_is_interrupted()` to check the AmigaOS break
signal and clear it:

```c
#include <proto/exec.h>
static inline int mp_hal_is_interrupted(void) {
    return (CheckSignal(SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) != 0;
}
```

Enable `MICROPY_KBD_EXCEPTION (1)` in `mpconfigport.h` so the VM calls
`mp_hal_is_interrupted()` at each bytecode step and raises `KeyboardInterrupt`.

---

### Phase 8 — Networking (`bsdsocket.library`)

AmigaOS 3.x networking is provided by `bsdsocket.library`, a BSD socket API
implemented as an Amiga shared library. It is present in any AmiTCP/Miami/Roadshow
stack and in Amiberry's built-in networking.

The MicroPython `usocket` module (`extmod/modusocket.c`) can be enabled once the
BSD socket calls are wired up. Key differences from POSIX:

- Library must be opened: `SocketBase = OpenLibrary("bsdsocket.library", 4)`
- All socket calls go through the library base, not direct syscalls: use NDK macros
  (`socket()`, `connect()`, etc. expand to `CallLib(SocketBase, ...)` via `<proto/socket.h>`)
- `errno` is per-library: use `Errno()` from `<proto/socket.h>` instead of global `errno`
- `select()` is available; `poll()` may not be

Suggested approach:
1. Open `bsdsocket.library` in `main.c`; fail gracefully if absent
2. Add `ports/amiga/amigasocket.c` providing `mp_uos_socket_*` shims that translate
   MicroPython socket calls to `bsdsocket.library` calls
3. Enable `MICROPY_PY_USOCKET (1)` in `mpconfigport.h`
4. Map `Errno()` to MicroPython's `MP_E*` errno constants

---

### Phase 9 — CI build workflow

Add `.github/workflows/ports_amiga.yml` to confirm the port cross-compiles on Linux
without an emulator run. bebbo GCC can be installed in CI via the binary release:

```yaml
- name: Install bebbo GCC
  run: |
    curl -L https://github.com/bebbo/amiga-gcc/releases/download/2024-01-01/amiga-gcc-linux.tar.bz2 | \
      sudo tar -xj -C /opt
    echo "/opt/amiga/bin" >> $GITHUB_PATH
```

---

### Other known limitations

| Issue | Status | Fix |
|-------|--------|-----|
| `try/except` in `@micropython.native` crashes | Known bug | Needs a 68k assembly NLR (`nlr68k.S`) that saves/restores D2–D7/A2–A5 in the `nlr_buf_t`; until then, avoid exceptions inside native functions |
| Heap is a static 256 KB BSS array | Works but wastes Fast RAM at link time | Switch to `AllocVec(MICROPY_HEAP_SIZE, MEMF_FAST\|MEMF_PUBLIC)` in `main.c` |
| GC stack scan is a heuristic | May miss or over-scan roots | Replace with exact `FindTask(NULL)->tc_SPLower/tc_SPUpper` bounds |
| Console uses newlib stdio | Works; small overhead | Switch to `dos.library` `FGetC`/`Write` for direct OS calls |
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

## Implementation Order Summary

| Phase | Status | Outcome |
|-------|--------|---------|
| 0 — Toolchain | ✅ Done | bebbo GCC 6.5.0b at `/opt/amiga` |
| 1 — Skeleton | ✅ Done | Builds and runs in Amiberry (185 KB AmigaOS executable) |
| 2 — File I/O | ✅ Done | `open()`, `import`, context manager; newlib stdio (no NDK needed) |
| 3 — Stdlib | ✅ Done | math, struct, json, re, hashlib, float; json.loads via port-local modjson.c |
| 4 — `amiga` module | ✅ Done | os_version, find_task, alloc_vec, free_vec, execute; SystemTagList for exit codes |
| 5 — 68k emitter | ✅ Done | `@micropython.native` via `MICROPY_EMIT_68K`; try/except in native mode is a known limitation |
| 6 — Package imports | Planned | `Lock`/`Examine` in `mp_import_stat()`; enables `import mypackage` |
| 7 — Ctrl+C | Planned | `CheckSignal(SIGBREAKF_CTRL_C)` + `MICROPY_KBD_EXCEPTION` |
| 8 — Networking | Planned | `bsdsocket.library` + `usocket` module |
| 9 — CI | Planned | `.github/workflows/ports_amiga.yml` cross-compile check |
