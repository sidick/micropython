# MicroPython port to AmigaOS 3.x — Implementation Plan

## Overview

This document describes a plan to port MicroPython to AmigaOS 3.x running on Motorola 68k hardware (68000/68020/68030/68040/68060). The target is a CLI-driven REPL with file-system access, using the `ports/minimal` port as the structural template.

### Goals

- Phase 0 ✅ Toolchain: bebbo GCC installed and working
- Phase 1 ✅ Skeleton: port builds and runs in Amiberry emulator
- Phase 2 ✅ File system access and `import` from AmigaDOS volumes
- Phase 3 ✅ Standard MicroPython library modules
- Phase 4: Amiga-specific `amiga` C module (exec/dos library bindings)
- Phase 5 (stretch): 68k native code emitter

### Non-goals (initially)

- Workbench GUI / Intuition window — CLI only
- Networking (bsdsocket.library) — later
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

## Phase 4 — Amiga-specific `amiga` Module

Expose key AmigaOS services to Python as a built-in C module. Requires NDK.

### Proposed API

```python
import amiga

amiga.version()                       # → (major, minor) of AmigaOS
amiga.alloc_vec(size, flags)          # → address int
amiga.free_vec(addr)
amiga.find_task(name=None)            # → task address or None
amiga.execute(cmd, stdin=0, stdout=0) # → return code
```

### Implementation outline

Create `ports/amiga/modamiga.c`. Note: use `static` not `STATIC` (removed from MicroPython):

```c
#include "py/runtime.h"
#include "py/obj.h"
#include <proto/exec.h>

static mp_obj_t amiga_version(void) {
    struct Library *lib = (struct Library *)SysBase;
    mp_obj_t items[2] = {
        mp_obj_new_int(lib->lib_Version),
        mp_obj_new_int(lib->lib_Revision),
    };
    return mp_obj_new_tuple(2, items);
}
static MP_DEFINE_CONST_FUN_OBJ_0(amiga_version_obj, amiga_version);

static const mp_rom_map_elem_t amiga_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_amiga) },
    { MP_ROM_QSTR(MP_QSTR_version),  MP_ROM_OBJ(&amiga_version_obj) },
};
static MP_DEFINE_CONST_DICT(amiga_module_globals, amiga_module_globals_table);

const mp_obj_module_t amiga_module = {
    .base    = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&amiga_module_globals,
};
MP_REGISTER_MODULE(MP_QSTR_amiga, amiga_module);
```

### Deliverable

`import amiga; print(amiga.version())` prints the AmigaOS version.

---

## Phase 5 (Stretch) — 68k Native Code Emitter

MicroPython has native code emitters for x64, ARM/Thumb, AArch64, Xtensa, RISC-V. Adding 68k requires:

1. `py/asm68k.h` / `py/asm68k.c` — 68k instruction encoder (MOVEx, ADD, SUB, LINK/UNLK, JSR, RTS, etc.)
2. `py/emit68k.c` — native emitter translating MicroPython IR to 68k, following `py/emitx64.c`
3. Register mapping: `d0`–`d7` (data), `a0`–`a6` (address), `a7` = SP; use `a5` or `a4` as the code-state pointer
4. Add `MICROPY_EMIT_68K` config macro and wire into `py/compile.c`

Significant work (several thousand lines); defer until phases 1–4 are solid.

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
| 4 — `amiga` module | Not started | AmigaOS library calls from Python; needs NDK |
| 5 — 68k emitter | Not started | `--emit native` support |
