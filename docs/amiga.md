# MicroPython port to AmigaOS 3.x — Implementation Plan

## Overview

Port of MicroPython to AmigaOS 3.x on Motorola 68k (68020+). CLI-driven REPL
with file-system access, based on the `ports/minimal` template.

### Phase status

| # | Phase | Status |
|---|-------|--------|
| 0 | Toolchain — bebbo GCC | ✅ |
| 1 | Skeleton — builds and runs in Amiberry | ✅ |
| 2 | File system access and `import` | ✅ |
| 3 | Standard MicroPython library modules | ✅ |
| 4 | Amiga-specific `amiga` C module | ✅ |
| 5 | 68k native code emitter | ✅ |
| 6 | Package imports | ✅ |
| 7 | Ctrl+C interrupt handling | ✅ |
| 8 | Native AmigaOS API migration | ✅ |
| 9 | Networking via `bsdsocket.library` | ✅ |
| 10 | Command-line argument parsing | ✅ |
| 11 | CI build workflow | planned |
| 12 | 68k native emitter rework (fix ASM_CALL_IND crash) | planned |
| 13 | Interactive line editing at the REPL | ✅ |
| 14 | Dynamic heap growth | ✅ |
| 15 | *withdrawn* | — |
| 16 | Pythonic file I/O via VFS | ✅ |
| 17 | Native AmigaOS library access (`amiga.library`) | ✅ (step 7 deferred) |
| 18 | ARexx integration (inbound + outbound) | ✅ |
| 19 | Workbench launch support | ✅ |
| 20 | Env-var integration (`os.getenv`/`putenv`/`unsetenv`) | ✅ |
| 21 | Volume / assign introspection | ✅ |
| 22 | AmigaDOS pattern matching | ✅ |
| 23 | `timer.device`-backed timing | ✅ |
| 24 | Persistent REPL history | ✅ |
| 25 | Extra break signals | ✅ |
| 26 | `PROGDIR:` on `sys.path` | ✅ |
| 27 | Additional build variants | ✅ |
| 28 | TLS/SSL via AmiSSL v5 | planned |

### Non-goals (initially)

- Workbench GUI / Intuition window — CLI only
- 68000 alignment-safe build — target 68020+ first

---

## Technical Context

### CPU and ABI

- 68k big-endian, 32-bit. Minimum 68020 (unaligned access).
- AmigaOS register-based calling convention for OS calls; standard C ABI otherwise.
- `MP_ENDIANNESS_LITTLE = 0` (auto-detected from GCC's `__BYTE_ORDER__`).

### Toolchain

bebbo's GCC (`m68k-amigaos-gcc`), GCC 6.5.0b. Produces native AmigaOS HUNK
executables.

**Recommended: container-based build.** `tools/amiga-build.sh` runs the
build inside `stefanreinauer/amiga-gcc:latest` — the same image CI uses,
so local and CI binaries are bit-identical. Output lands in the standard
`ports/amiga/build-<variant>/` paths so `tools/amiga-vamos-run.sh` and
friends find the binaries unchanged. Files are written as the host user
(via `--user`), not root.

```sh
tools/amiga-build.sh                   # all four variants
tools/amiga-build.sh standard          # one
tools/amiga-build.sh standard 68040    # several
tools/amiga-build.sh clean             # clean all build dirs
```

**Native install (alternative).** Install from
<https://franke.ms/git/bebbo/amiga-gcc>:

```sh
git clone https://franke.ms/git/bebbo/amiga-gcc
cd amiga-gcc
make PREFIX=/opt/amiga all     # installs gcc, binutils, clib2, NDK
export PATH=/opt/amiga/bin:$PATH
```

`make min` does *not* install C library headers and cannot compile MicroPython.
Always use `all` or at minimum `clib2 ndk`. Don't interleave native and
container builds in the same `mpy-cross/build/` without running
`tools/amiga-build.sh clean` first — the host and Linux mpy-cross binaries
share that path and aren't compatible.

### Exception handling (NLR)

Uses `MICROPY_NLR_SETJMP (1)` — MicroPython's setjmp fallback. No 68k assembly
NLR needed.

### GC and heap

Phase 14 manages a dynamic, growable heap of `AllocVec`'d chunks. See Phase 14
below. GC stack scan runs from current SP up to `gc_stack_top` (captured in
`main()` at startup) — `tc_SPUpper` would be principled but vamos leaves it
zero on the initial process and the resulting ~4 GB scan walks off into
unmapped memory.

---

## Port file structure

```
ports/amiga/
├── Makefile              # Build rules and toolchain config
├── mpconfigport.h        # Feature flags and type definitions
├── mphalport.h/.c        # HAL stubs + console I/O
├── main.c                # Entry point, gc_collect(), heap pre-scan
├── vfs_amiga.c           # VfsAmiga + FileIO/TextIOWrapper
├── sysstdio.c            # mp_sys_stdin/stdout/stderr stream objects
├── floatconv.c           # bebbo soft-float library bug fixes
├── modjson.c             # Port-local json.loads bypass
├── modamiga.c            # The `amiga` C module
├── modos.c               # os.getenv/putenv/unsetenv (Phase 20)
├── modsocket.c           # socket module (Phase 9)
├── amiga_timer.c         # timer.device + EClock timing (Phase 23/25)
├── amiga_history.c       # Persistent REPL history (Phase 24)
├── amiga_lib_call.S      # Library trampoline (Phase 17)
├── qstrdefsport.h        # Port-specific interned strings
├── manifest.py           # Frozen modules (amiga.py + _amiga_fd etc.)
├── modules/              # Frozen Python modules
└── variants/{standard,minimal,68020fpu,68040}/
```

---

## Phase 0 — Toolchain ✅

bebbo GCC 6.5.0b at `/opt/amiga`. `m68k-amigaos-gcc --version` confirms.

## Phase 1 — Skeleton ✅

Builds an AmigaOS `loadseg()`ble HUNK executable; confirmed running in Amiberry.

Notable `mpconfigport.h` settings worth remembering:

- `MICROPY_NLR_SETJMP (1)` — setjmp-based exceptions
- `MICROPY_LONGINT_IMPL_LONGLONG` — `'q'`/`'Q'` struct codes and >30-bit
  literals need this; adds ~5 KB.
- `MICROPY_ENABLE_PYSTACK (1)` — **not optional.** bebbo gcc 6.5's
  `alloca` returns 2-byte-aligned addresses on 68k, which break
  `mp_obj_is_obj`'s `(o & 3) == 0` check on iterator slots; routing through
  `mp_pystack_alloc` (4 KB `aligned(8)` buffer) gives deterministic alignment.

`mp_builtin_open_obj` must be supplied by the port (`py/modio.c` references
but doesn't define it).

## Phase 2 — File System and `import` ✅

`open()`, `read`, `write`, `seek`, `tell`, `readline`, `with`, and `import`
from any mounted volume all work. Originally newlib stdio in `amigaio.c` /
`amigafile.c`; Phase 8 migrated to direct `dos.library`; Phase 16 then
rewrote behind a VFS layer (`vfs_amiga.c`) — those files are gone.

## Phase 3 — Standard Library Modules ✅

`MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES`. Working: `math`, `struct`, `json`,
`re`, `hashlib`, `float`. `sys.platform == "amiga"`.

`-msoft-float` is in CFLAGS — no hardware FPU on 68020. FPU variants (Phase 27)
drop this.

**`json.loads` workaround.** Upstream `extmod/modjson.c` fails on 68k with
`OSError: stream operation not supported` — it builds a stack-allocated
`mp_obj_stringio_t` and routes through the stream protocol, but
`mp_obj_is_obj()` requires 4-byte alignment which the 68k SysV ABI doesn't
guarantee for every stack frame. `ports/amiga/modjson.c` is a port-local
replacement that bypasses the stream protocol for `loads`; `Makefile` filters
out `extmod/modjson.c`.

## Phase 4 — `amiga` Module ✅

`ports/amiga/modamiga.c` exposes exec/dos primitives. Must be in `SRC_QSTR`
(not just `SRC_C`) so `MP_REGISTER_MODULE` and qstrs are picked up.

```python
import amiga
amiga.os_version()                          # (version, revision)
amiga.find_task(name=None)                  # int task pointer (current if None)
amiga.alloc_vec(size, flags=amiga.MEMF_ANY) # int address; MemoryError on fail
amiga.free_vec(addr)
amiga.execute(cmd)                          # int rc (0/5/10/20, -1=start fail)
amiga.exists(path)                          # bool, suppresses volume requesters
# MEMF_ANY/PUBLIC/CHIP/FAST/CLEAR constants
```

`amiga.execute()` uses `SystemTagList()` (not `Execute()`) so the real CLI
return code surfaces. `amiga.exists()` brackets `Lock` with
`pr_WindowPtr = (APTR)-1` so an unmounted volume doesn't pop an
"Insert volume" requester.

Later phases add many more bindings to this module — see their sections.

## Phase 5 — 68k Native Code Emitter ✅

`@micropython.native` / `--emit native` via GENERIC_ASM_API.

| File | Role |
|------|------|
| `py/asm68k.h/.c` | 68k instruction encoder + helpers |
| `py/emit68k.c` | `#define N_68K 1` + `#include "py/emitnative.c"` |

Register allocation: D0 = RET/ARG_1, D1/D2/D3 = ARG_2/3/4 (D2/D3 callee-saved,
reloaded in prologue), D4–D6 = temps, D7 = REG_LOCAL_1, A2/A3 = LOCAL_2/3 (used
for REG_GENERATOR_STATE / REG_QSTR_TABLE), A4 = REG_FUN_TABLE, A5 = frame ptr.
`MAX_REGS_FOR_LOCAL_VARS = 1` — only D7 is a data register safe for arithmetic.

Calling convention: AmigaOS/cdecl, args pushed right-to-left.
`LINK A5,#-N; MOVEM.L D2-D7/A2-A4,-(SP)` for entry, branches always `.W` form,
`CMP.L; Scc; ANDI.L #1`.

**Known limitations** (also tracked as Phase 12):

- `try/except` in native mode is broken — `NLR_BUF_IDX_LOCAL_1` falls inside
  the `jmp_buf` and gets overwritten by `setjmp`.
- Viper integer arithmetic on address registers is prevented by
  `MAX_REGS_FOR_LOCAL_VARS = 1`.
- Calling another function from a `@micropython.native` body crashes — see
  Phase 12.

## Phase 6 — Package imports ✅

`mp_import_stat()` uses `dos.library` `Lock`/`Examine`; `fib_DirEntryType > 0`
is a directory. `import mypackage` works.

## Phase 7 — Ctrl+C interrupt handling ✅

Two paths:

1. During computation: `MICROPY_VM_HOOK_LOOP` polls
   `CheckSignal(SIGBREAKF_CTRL_C)` every 1024 bytecodes
   (`amiga_check_ctrl_c` in `mphalport.c`).
2. During input: `mp_hal_stdin_rx_chr()` checks `mp_interrupt_char`.

`shared/runtime/interrupt_char.c` provides `mp_interrupt_char` and
`mp_hal_set_interrupt_char()`.

## Phase 8 — Native AmigaOS API migration ✅

All newlib stdio replaced with `dos.library`:

| Component | Now uses |
|-----------|----------|
| Console input | `FGetC(Input())` |
| Console output | `Write(Output(), buf, len)` |
| File I/O | `BPTR` + `Open`/`Read`/`Write`/`Close`/`Seek`/`Flush` |
| Heap | `AllocVec(MEMF_ANY\|MEMF_PUBLIC\|MEMF_CLEAR)` |
| GC stack bounds | `FindTask(NULL)->tc_SPUpper` |
| `mp_hal_delay_ms` | `Delay()` (later replaced by Phase 23 `timer.device`) |

## Phase 9 — Networking ✅

`ports/amiga/modsocket.c` via `bsdsocket.library`. `SocketBase` opened in
`main()` (silently absent if library missing — socket creation raises
`OSError`). `Errno()` (per-library) instead of global errno;
`IoctlSocket(FIONBIO)`; `SO_RCVTIMEO`/`SO_SNDTIMEO`; `Inet_NtoA`/`inet_addr`/
`gethostbyname`; `getaddrinfo`/`freeaddrinfo`/`gethostname`. Stream protocol
implemented so `readline()`, `with`, etc. work. `CloseSocket()` (not `close`)
on shutdown. `__NO_NETINCLUDE_TIMEVAL` guard avoids `devices/timer.h` /
`sys/time.h` conflict.

## Phase 10 — Command-line argument parsing ✅

`main.c` parses `argc`/`argv` before the REPL:

```
micropython                     # interactive REPL
micropython script.py [args]    # sys.argv = ["script.py", ...]
micropython -c "code" [args]    # sys.argv = ["-c", ...]
micropython -m module [args]    # sys.argv = ["module", ...]
micropython -h / --help / --version
```

`MICROPY_PY_SYS_ARGV (1)`, `mp_sys_path = [""]`. Script directory is prepended
to `sys.path[0]` using AmigaOS path parsing. `-c` via `pyexec_vstr`; `-m` via
`mp_builtin___import__`. Raw console mode (`SetMode(stdin, 1)`) is REPL-only.
`genhdr/mpversion.h` included for `MICROPY_BANNER_NAME_AND_VERSION`.

The bebbo argv parser is broken under vamos with multi-arg invocations;
`amiga_parse_args` parses `pr_Arguments` itself.

---

## Phase 11 — CI build workflow (planned)

Add `.github/workflows/ports_amiga.yml` to confirm cross-compile on Linux
without an emulator run. Install bebbo GCC from a binary release tarball into
`/opt/amiga`.

---

## Phase 12 — 68k native emitter rework (planned)

Phase 5 landed a working emitter, but only for the single-statement
`return <const>` shape. The moment a `@micropython.native` body calls another
function — anything that goes through `mp_fun_table` — the CPU jumps off into
the 68k vector-table area (typically `PC=0x404`, `SR=0x0700`) and faults. About
47 of the `tests/micropython/native_*.py` and `viper_*.py` tests fail in this
pattern:

- `D0 = self_in` is correct on entry
- `REG_FUN_TABLE` (A4) ends up pointing at random memory by the time
  `ASM_CALL_IND` runs
- The indirect `MOVEA.L (idx*4, A4), A0; JSR (A0)` then lands in the vector table

**Probable causes (need investigation):**

1. **Prologue ordering.** `asm_68k_entry` does `LINK A5,#-N;
   MOVEM.L D2-D7/A2-A4,-(SP); load D0..D3 from 8(A5)..20(A5)`. A4 is in the
   saved set, so its value at function entry is whatever the parent left there.
   `emitnative.c` loads A4 from the const table via `REG_PARENT_ARG_1` (D0)
   through `asm_68k_ensure_areg` — verify A4 holds the right value when the
   first `ASM_CALL_IND` runs.
2. **Register clobbering through CALL_IND.** Bebbo cdecl says callers preserve
   nothing; we save D2–D7/A2–A4 across the whole function, but every
   `ASM_CALL_IND` may clobber A4 inside the callee. If the C function doesn't
   preserve A4, the next CALL_IND sees garbage. Need per-call save/restore or
   a different register.
3. **Stack frame mismatch.** Verify `sp += MP_OBJ_ITER_BUF_NSLOTS - 1` and
   friends match what `LINK A5, #-N` actually allocates.

Start with the simplest repro (`native_const.py` line 14: nested native
function returning 123, called from a wrapper) and work up.

`try/except` in native mode is the other gap — `NLR_BUF_IDX_LOCAL_1` falls
inside the `jmp_buf` that `setjmp` overwrites. Needs a 68k assembly NLR
(`nlr68k.S`) that saves/restores D2–D7/A2–A5 in the `nlr_buf_t`.

---

## Phase 13 — Interactive REPL line editing ✅

`shared/readline/readline.c` drives the REPL. Cursor keys, history, kill/yank,
Home/End all work.

**CSI translation.** AmigaOS's `console.device` emits cursor reports as
single-byte CSI (`0x9B`) followed by parameters; `shared/readline/` expects
the two-byte `ESC [` form. `mp_hal_stdin_rx_chr()` keeps a one-byte pending
buffer: when `FGetC` returns `0x9B`, return `ESC` and hand `[` to the next
call. Hosts that already emit `ESC [` (vamos's xterm pass-through) see no
change.

**Running the REPL under vamos:** `tools/amiga-vamos-repl.sh` puts the host
TTY into `-icanon -echo -isig` for the duration of the run (vamos's
`SetMode(stdin,1)` doesn't translate to `tcsetattr` on the host TTY).
`AMIGA_VARIANT=68040` / `=minimal` selects the build.

## Phase 14 — Dynamic heap growth ✅

Heap is a chain of `AllocVec` chunks managed via `MICROPY_GC_SPLIT_HEAP_AUTO`.
Initial size from (priority order) `-X heap=<N>[K|M]`, `MICROPYHEAP` env-var
(`dos.library GetVar`), compile-time `MICROPY_HEAP_SIZE`. Cap via
`-X maxheap=<N>` / `MICROPYHEAPMAX`.

`main.c` owns `amiga_heap_chunks[16]` for tracking (`AllocVec` is not
reclaimed by AmigaOS on task exit, so all chunks must be `FreeVec`'d
explicitly at shutdown). `gc_get_max_new_split()` reports
`AvailMem(MEMF_ANY|MEMF_PUBLIC|MEMF_LARGEST)` minus a small headroom, clamped
to `-X maxheap`. The GC's own sweep auto-releases empty grown chunks.

```python
>>> amiga.heap_info()           # → (total_bytes, free_bytes, num_arenas)
(256000, 248000, 1)
```

## Phase 15 — *withdrawn*

Originally exec.library memory pools. Phase 14's dynamic GC heap covers the
common case; explicit native-buffer lifetimes can use `alloc_vec`. Number kept
to avoid renumbering later phases.

## Phase 16 — Pythonic file I/O via VFS ✅

`MICROPY_VFS=1` + `MICROPY_READER_VFS=1` with a port-local `VfsAmiga` in
`vfs_amiga.c`. Stateless wrapper around `Lock` / `Examine` / `CurrentDir` /
`Open` / `Read` / `Write` / `CreateDir` / `DeleteFile` / `Rename`. AmigaDOS
keeps cwd in `pr_CurrentDir`. `main()` mounts a single `VfsAmiga` at `/`;
AmigaOS-style paths (`volume:dir/file` or relative) route directly.

`amigafile.c` and `amigaio.c` are deleted — `mp_lexer_new_from_file` comes
from `extmod/vfs_reader.c`; `mp_import_stat` is the inline that delegates to
`mp_vfs_import_stat`; `mp_builtin_open_obj` is aliased to `mp_vfs_open_obj`.

`vfs_amiga.c` reuses the `pr_WindowPtr = -1` requester-suppression pattern
around every `Lock`. `os.chdir` keeps the first inherited cwd lock (it's the
shell's, not ours) and `UnLock`s subsequent ones. `ilistdir` uses a finaliser
so an abandoned `for f in os.listdir(...)` doesn't leak the directory lock.
`open(..., "r+b")` correctly maps to `MODE_OLDFILE` (fail-if-missing), not
`MODE_READWRITE` (create-on-missing).

---

## Phase 17 — Native AmigaOS library access ✅ (step 7 deferred)

Generic library-call mechanism. Any function in any library — system or
third-party — callable from Python with no port-side C per library.

### Why this is tractable on AmigaOS

- Every library function lives at a negative offset (LVO) from the base.
- Args go in a fixed set of D/A registers — never on the stack.
- Return value always in D0. Library base in A6.
- All values 32-bit; no struct-in-register, no varargs.

NDK ships a `.fd` (Function Definition) file per library at
`/opt/amiga/m68k-amigaos/ndk-include/fd/*.fd` — enough to mechanically look
up call signatures.

### Two-layer design

**Layer 1 — low-level trampoline.** `ports/amiga/amiga_lib_call.S` is a
single hand-written 68k routine. C entry:

```c
uint32_t amiga_lib_call_asm(
    uint32_t base, int32_t offset,
    uint32_t d0..d7, uint32_t a0..a5);
```

Saves callee-saved set (`d2-d7/a2-a6`), loads A6 with the base, loads the
14 register slots from the stack. The jump to `base + offset` uses an
**RTS-trick** rather than computed `JSR`: with all D/A regs committed to
user values, no scratch register is free for the call target. Pushing
target + local return label and issuing `rts` transfers to the library;
the library's own `rts` pops the local label and returns to our cleanup.

C extension is registered as `_amiga`; three entry points:

```python
amiga.lib_open(name, version=0)         # OpenLibrary; OSError(ENOENT) on fail
amiga.lib_close(base)                   # CloseLibrary; tolerates 0 base
amiga.lib_call(base, offset, **regs, ret="d0")
```

Register kwargs: `d0`–`d7`, `a0`–`a5` (A6 is the base; A7 is SP).
`ret="d0"` signed, `"d0u"` unsigned, `"void"` returns `None`.

**Layer 2 — `.fd`-driven proxy.** Frozen `amiga.py` re-exports everything
from `_amiga`, adds a `Library` class on top of the FD table baked in from
`_amiga_fd.py`:

```python
with amiga.library("intuition.library", 37) as intuition:
    intuition.DisplayBeep(0)
    win = intuition.OpenWindow(nw_ptr)
    intuition.CloseWindow(win)
```

`Library.__getattr__` looks up the signature, builds a closure that maps
positional args into the right register kwargs of `_amiga.lib_call`, and
`setattr`s the closure onto the instance so subsequent reads skip
`__getattr__`. Context-manager support; explicit `close()`; GC-time
`__del__` cleanup. Errors: unknown library → `OSError(ENOENT)`; missing
function → `AttributeError`; wrong arg count → `TypeError`; call after
close → `ValueError`.

### FD table generator

`tools/amiga-fdgen.py` parses every `.fd` under one or more NDK trees and
emits a Python module whose `LIBRARIES` dict maps each openable name to
`{function_name: (lvo, regs_csv, since)}`.

- **Chronological ordering.** Hyperion's 3.1.4 → 3.2 → 3.2.x are
  *calendar-newer* than 3.5/3.9 (development restarted years after 3.9).
  Hand-maintained `AMIGA_OS_RELEASE_ORDER` list avoids `3.2 < 3.9` numeric
  sort wrecking the `since` stamps.
- **Drift detection.** LVOs are append-only; a function with a different
  offset/register list in a later NDK is a hard warning. Earlier entry wins.
- **Public-only.** `##private` sections still consume LVO slots (so offsets
  are correct) but functions are dropped unless `--include-private`.
- **CIA and `mathieeedoub*` exceptions** don't fit the convention; the
  tool warns and skips them.

Against current bebbo NDK: **76 .fd files → 75 openable names → 1146 public
function signatures**, ~80 KB Python source.

### Runtime `.fd` loading + explicit signatures

When `Library(name)` doesn't find `name` in the frozen table, it falls back
to a pure-Python `.fd` parser in `amiga.py` and walks `PROGDIR:fd/` then
`LIBS:fd/`, trying both bebbo `<base>_lib.fd` and bare `<base>.fd`.
Alternatively, `Library(name, version, signatures=...)` or
`amiga.library_from_signatures(name, version, sigs)` bypasses both lookups.

### Tag lists (peek/poke + TagList)

Tag lists are pervasive in OS3.x. Three pieces in `modamiga.c`:

- Memory primitives: `peek_b/w/l/bytes(addr[, n])`, `poke_b/w/l/bytes(addr, v)`.
- `TagList` class wrapping an `AllocVec`'d `TagItem[]`. `(tag, value)` slots
  are 8 bytes; ints go straight into `ti_Data`; `bytes`/`str` get their own
  `AllocVec`'d NUL-terminated buffer with the address stored in `ti_Data`.
  All allocations released by `close()` / `__exit__` / GC `__del__`. Defines
  `__int__` returning the head address.
- `amiga.taglist(...)` factory. Kwargs resolved against `_amiga_tags.TAGS`;
  positional args support `(tag, value)` pairs, iterable, or alternating
  `tag, value, tag, value`. Unknown tag names raise `KeyError`.
- `Library` proxy runs `int(v)` on any non-int arg so `TagList` passes through
  cleanly:

```python
with amiga.taglist(WA_Width=640, WA_Height=480, WA_Title="hi") as tags:
    with amiga.library("intuition.library", 37) as intuition:
        intuition.OpenWindowTagList(0, tags)
```

`tools/amiga-taggen.py` drives bebbo's m68k-gcc to harvest tag IDs from
NDK headers (preprocess with `-E -dM`, compile as `const ULONG _x_<NAME> =
(ULONG)(<NAME>);` with `-S -O2`, parse `.long` from the assembly). Keeps
only TAG_USER-namespace values (bit 31 set) plus universal `TAG_*` markers.
**1072 tag IDs** in `_amiga_tags.py`, ~48 KB frozen.

`amiga.parse_taglist(addr, max_items=64)` walks a TagItem array back into a
`{tag_id: value}` dict, following `TAG_MORE` chains, dropping `TAG_IGNORE`,
honouring `TAG_SKIP`. `taglist(..., more=other)` writes a `TAG_MORE`
terminator pointing at `other.addr` and pins it alive on the outer.

### Struct ctypes-lite

`amiga.Struct(addr, layout, name=None)` wraps a C struct at a fixed address.
`layout` is `{field_name: (offset, type_code)}` with codes `B/b/H/h/L/l/P/sN/S`.
Defines `__int__` so an instance passes straight to a `Library` call.
Starter layouts shipped in `_amiga_structs.py`:

| factory | struct |
|---------|--------|
| `amiga.Node(addr)` | `struct Node` |
| `amiga.Task(addr)` | `struct Task` |
| `amiga.Library_struct(addr)` | `struct Library` (trailing `_struct` to avoid clash with the proxy class) |
| `amiga.DateStamp(addr)` | `struct DateStamp` |
| `amiga.FileInfoBlock(addr)` | `struct FileInfoBlock` |
| `amiga.IntuiMessage(addr)` | `struct IntuiMessage` |

### Step 7 (deferred)

Callback thunks for hook-driven library calls (`CreateNewProc`, blit hooks,
ARexx command dispatchers). Needs executable memory and per-callable
thunks. Defer until a concrete user need lands; most OS3.x scripting
doesn't need it.

### Manifest plumbing

`ports/amiga/manifest.py` is `freeze("$(PORT_DIR)/modules")`. Makefile sets
`FROZEN_MANIFEST = manifest.py` and `MPY_TOOL_FLAGS = -mlongint-impl=longlong`
to match `MICROPY_LONGINT_IMPL_LONGLONG` (else `mpy-tool.py` emits MPZ
literals which fail the `static_assert` in `frozen_content.c`).

---

## Phase 18 — ARexx integration ✅

ARexx is *the* Amiga IPC mechanism: every well-behaved app exposes an ARexx
port. Both directions implemented.

### Inbound port

Five C primitives in `modamiga.c`, `RexxMessage` facade in `amiga.py`:

```python
name = amiga.rexx_open()                  # opens MICROPYTHON.1 (or .N)
while True:
    msg = amiga.rexx_recv(timeout_ms=1000)
    if msg is None: continue
    try:    msg.reply(str(eval(msg.command.decode("ascii"))), rc=0)
    except Exception as e: msg.reply(str(e), rc=10)
amiga.rexx_close()

amiga.rexx_serve(lambda cmd: eval(cmd))   # ready-made dispatcher
```

- `rexx_open(stem="MICROPYTHON")` finds the lowest free `.N` suffix
  under `Forbid()`/`Permit()`, `CreateMsgPort()` + `AddPort()`.
- `rexx_close()` drains the queue (replying rc=20 to anything still
  queued so a hung `rx` doesn't stay blocked), `RemPort`, `DeleteMsgPort`,
  closes `rexxsyslib.library` if opened. Called from `main.c` on exit as
  a safety net.
- `rexx_recv(timeout_ms=None)` polls then `Wait()`s. The Phase 25 async
  `timer.device` port supplies the timeout signal bit; `SIGBREAKF_CTRL_C`
  is always ORed in and raises `KeyboardInterrupt` if it fires.
- `RexxMessage.reply(result=None, rc=0, secondary=0)` sets `rm_Result1` =
  `rc`. If `rc == 0` and result non-None, lazy-opens `rexxsyslib.library`,
  `CreateArgstring` over the bytes, stores in `rm_Result2`; otherwise
  `secondary` goes into `rm_Result2`. `ReplyMsg`. `__del__` sends an
  automatic rc=20 reply if the script forgets — a forgotten reply would
  block the sender forever.

### Outbound

```python
result = amiga.rexx("PPAINT.1", "ScreenToFront")           # bytes
rc, result = amiga.rexx("HOST.1", "DoIt", check=False)     # (rc, result|None)
```

Textbook send-and-wait: `FindPort` → lazy `OpenLibrary("rexxsyslib.library")`
(cached, two-way scripts pay one open) → `CreateMsgPort` for private reply
port → `CreateRexxMsg` → `CreateArgstring` for the command → `rm_Action =
RXCOMM | RXFF_RESULT` → `PutMsg` → `Wait(reply | CTRL_C)` → unpack
`rm_Result1`/`rm_Result2`, copy result bytes via `mp_obj_new_bytes` +
`LengthArgstring`, `DeleteArgstring` both → `DeleteMsgPort`.

**Ctrl+C is deferred.** Once `PutMsg` is in flight we can't tear down the
reply port — the host would `PutMsg` into freed memory. The wait latches
the CTRL_C bit but keeps spinning until the reply is in hand; then
`KeyboardInterrupt` is raised before returning.

### `rexxsyslib.library` missing

Lazy-opened. So `import amiga` still works, the inbound port works for
rc-only replies, but:

- `RexxMessage.reply(result="...", rc=0)` → replies rc=10 to sender, raises
  `OSError("rexxsyslib.library unavailable")`.
- `amiga.rexx(host, command)` → raises `OSError(...)` before any send.

### `rx` script gotcha

`call open(...)`, `call writeln(...)`, `call close(...)` each clobber the
`result` special variable. Any `rx` script wanting to inspect the host
reply must snapshot it immediately after `address`:

```rexx
options results
address MICROPYTHON.1 'some command'
saved_rc = rc
saved_result = result
```

---

## Phase 19 — Workbench launch support ✅

WB-launched processes get a `WBStartup` message instead of `argc`/`argv`, and
their config comes from `.info` tooltypes.

**Detecting.** Bebbo's `crt0.o` already does the heavy lifting — when
`pr_CLI == 0` it `WaitPort`s, `GetMsg`s the `WBStartup`, stashes the pointer
in the global `_WBenchMsg`, and on exit `Forbid()`s and `ReplyMsg`s back to
Workbench. We just `extern struct WBStartup *_WBenchMsg;` and test for null.

**Console.** WB-launched processes have `pr_CIS`/`pr_COS` both `NULL`, so
`main.c` opens `CON:0/30/640/200/MicroPython/AUTO/CLOSE/WAIT` and points
stdin/stdout/console-task at it. `/AUTO` defers window appearance until
write; `/WAIT` keeps it open after `main()` returns.

**Tooltypes.** `icon.library` `GetDiskObject(sm_ArgList[0].wa_Name)`; handle
cached in `IconBase` + `amiga_wb_diskobject`. Two consumed at startup:

- `SCRIPT=<path>` — script to run instead of REPL
- `HEAP=<N>` / `MAXHEAP=<N>` — same parser as `-X heap=` (env vars still
  win, since they're more explicitly user-set)

```python
if amiga.launched_from_workbench():
    extra = amiga.tooltype("EXTRA_PATH", "")
    for path in amiga.wb_selected_files():     # shift-clicked icons
        process(path)
```

`wb_selected_files()` renders each WBArg with `NameFromLock + AddPart`.

Cannot be exercised under vamos (no Workbench, no `icon.library`); full
validation requires Amiberry/FS-UAE or real hardware.

## Phase 20 — Env-var integration ✅

`os.getenv` / `os.putenv` / `os.unsetenv` via `dos.library` `GetVar`/`SetVar`
with `flags=0` (local CLI vars, falling through to global `ENV:`). Matches
Unix `os.putenv` semantics — visible to child processes spawned via
`amiga.execute()`, not to unrelated shells. For system-wide / persistent,
write to `ENV:`/`ENVARC:` directly with `open()`.

`mpconfigport.h`: `MICROPY_PY_OS_GETENV_PUTENV_UNSETENV (1)` +
`MICROPY_PY_OS_INCLUDEFILE "ports/amiga/modos.c"`. `modos.c` is `#include`d
by `extmod/modos.c`; no Makefile changes.

**Vamos workarounds** (both correct on real AmigaOS):

1. `GetVar` returns 0 (not -1) for a missing variable; we treat `len <= 0` as
   missing (misreports a genuine empty-string variable on real AmigaOS,
   correct on vamos).
2. `DeleteVar` reads `flags` from `D4`, but the NDK fd specifies `D2`. We
   use the V36-documented "`SetVar` with `NULL` buffer deletes" form
   instead.

## Phase 21 — Volume / assign introspection ✅

```python
amiga.volumes()      # ['Python:', 'Ram Disk:', 'Workbench:']
amiga.assigns()      # {'C:': 'Workbench:C', 'LIBS:': 'Workbench:Libs', ...}
amiga.disk_info(path) # (free_bytes, total_bytes, block_size)
```

`volumes()`/`assigns()` walk `LockDosList(LDF_VOLUMES/LDF_ASSIGNS | LDF_READ)`
via `NextDosEntry`. Assign targets from `NameFromLock(dol_Lock)` for
`DLT_DIRECTORY`, or `dol_misc.dol_assign.dol_AssignName` for late/non-binding
assigns. Multi-directory assigns report first dir only.

`disk_info(path)` locks (with `pr_WindowPtr=-1`), calls `Info()`, computes
byte counts as `uint64_t` for >4 GB volumes. `IoErr()` mapped to `MP_E*`
constants (e.g. `ERROR_DEVICE_NOT_MOUNTED` → `MP_ENODEV`).

## Phase 22 — AmigaDOS pattern matching ✅

```python
for path in amiga.match("S:#?"):       # eager list
for path in amiga.imatch("Work:#?.py"): # lazy iterator
```

One `AnchorPath` with trailing 512-byte buffer; `ap_Strlen` preset.
`MatchFirst` parses the pattern (no separate `ParsePattern`). Empty list for
`ERROR_NO_MORE_ENTRIES` / `ERROR_OBJECT_NOT_FOUND`; other DOS errors raise
`OSError`.

`imatch` uses `mp_type_polymorph_iter_with_finaliser`; the iterator owns the
`AnchorPath`, its finaliser calls `MatchEnd` + `FreeVec` so an abandoned
loop (`for p in amiga.imatch(...): break`) doesn't leak. `MatchFirst` runs
eagerly inside `imatch()` so the first result is ready on the first
`next()`. Both suppress auto-requesters.

## Phase 23 — `timer.device`-backed timing ✅

Replaced `clock()`-based path (busy-wait, ms-ish resolution) with:

- `mp_hal_delay_us(n)` → `timer.device TR_ADDREQUEST` via `DoIO()` for
  ≥200 µs, tight `ReadEClock()` busy-loop below.
- `mp_hal_delay_ms(n)` → `timer.device` for arbitrary-millisecond accuracy
  (the previous `Delay()` had 20 ms granularity).
- `mp_hal_ticks_us/ms()` → `ReadEClock()` (hardware counter, monotonic,
  cheap). EClock frequency cached at init.

Setup in `amiga_timer.c`: `CreateMsgPort` + `CreateIORequest` +
`OpenDevice("timer.device", UNIT_MICROHZ, ...)` once at startup, before
`mp_init()`. `TimerBase` (referenced by bebbo `proto/timer.h` inlines) is
set from the request's `io_Device`. IORequest stored port-local-static;
not thread-safe but the port is single-threaded.

## Phase 24 — Persistent REPL history ✅

`amiga_history.c` loads / saves `S:MicroPython.history` (override via
`MICROPYHISTORY` env-var). One entry per line, oldest first; CRLF tolerated
on read. `MICROPY_READLINE_HISTORY_SIZE` bumped to 32 (was 8). Both paths
suppress auto-requesters; failure (no `S:` volume, read-only, corrupt) is
silent — history just doesn't persist that session.

`main.c` calls `amiga_history_load()` after `mp_init()` and
`amiga_history_save()` before `mp_deinit()`. `amiga.readline_history()` and
`amiga.readline_push_history(line)` exposed for scripting / testing.

Not in `ENVARC:` — that's for preferences; a frequently-written log there
would make every reboot's `copy ENVARC: ENV: all` slower. `S:` is the
conventional AmigaOS dotfile spot.

## Phase 25 — Extra break signals ✅

```python
amiga.signal(other_task_addr, amiga.SIGBREAKF_CTRL_E)
mask = amiga.wait_signal(amiga.SIGBREAKF_CTRL_D | amiga.SIGBREAKF_CTRL_E,
                         timeout_ms=5000)
```

- `signal(task_addr, sigmask)` → `exec.library Signal()`. NULL task raises
  `ValueError`.
- `wait_signal(mask, timeout_ms=None)` → `Wait()`. `SIGBREAKF_CTRL_C` is
  always ORed into the internal mask (so the user can break out) but
  always stripped from the return (so Ctrl+C never spuriously satisfies a
  user signal); if CTRL_C fires, `KeyboardInterrupt`.

`timeout_ms` uses a **second, async** `timer.device` IORequest in
`amiga_timer.c` (kept separate from Phase 23's synchronous request so a
pending Wait can't collide with an in-flight `mp_hal_delay_us`). `SendIO`
arms the timer; the MsgPort's signal bit ORs into the Wait mask; the
request is aborted/drained on return. If the second port fails at startup,
`wait_signal` falls back to an untimed Wait — documented best-effort.

## Phase 26 — `PROGDIR:` on `sys.path` ✅

`PROGDIR:` is the auto-assign AmigaDOS creates per process for the
executable's directory. `main.c` *appends* it to `sys.path` after
`mp_init()` (prepending would lose it on every script run, since
positional-script handling replaces `sys.path[0]` with the script's
directory). After a script launch, `sys.path` is
`['<script_dir>', '.frozen', 'PROGDIR:']` — script wins, PROGDIR: as final
fallback.

## Phase 27 — Build variants ✅

`make VARIANT=<name>`; build dir is `build-<variant>`.

| Variant | CPU | Heap | Notes |
|---------|-----|------|-------|
| `standard` (default) | `-m68020 -msoft-float` | 256 KB | Any 68020+ |
| `minimal` | `-m68020 -msoft-float` | 128 KB | Stock A1200 (68EC020, 2 MB Chip). No `bsdsocket`, no `MICROPY_EMIT_68K` |
| `68020fpu` | `-m68020 -m68881` | 512 KB | 68020/30 + 68881/2 (A2630, A3000). No libgcc soft-float wraps |
| `68040` | `-m68040` | 1 MB | 68040 built-in FPU (A3640, A4000/040) |

Text-segment sizes: standard 356 KB, minimal 327 KB, 68020fpu 330 KB,
68040 339 KB.

**A500 isn't a target** — its 68000 lacks unaligned access. **Why 68040
is larger than 68020fpu:** Motorola dropped transcendentals (FSIN/COS/TAN/
ATAN/etc.) from the 68040; gcc emits libm calls instead of inlines, pulling
in ~9 KB. Alternative `-m68040 -m68881` would shrink it back and rely on
AmigaOS's FPSP (`68040.library`) to trap-and-emulate, but a binary that
uses FSIN on a 68040 without FPSP loaded (custom Workbench, stripped
startup, certain demos) gurus. The 9 KB is the price of self-containment.

`sys.implementation._machine` reports `"Amiga with 68EC020"` /
`"68020/68881"` / `"68040"`. `floatconv.c` `pow`/`tgamma` wraps apply on
all variants (math-library bugs, not FPU codegen).

FPU variants require bebbo to have FPU multilibs (`make all` provides them);
if `m68k-amigaos-gcc -m68881 -print-multi-lib` shows only a soft-float entry,
the link fails with `cannot find -lgcc`.

---

## Phase 28 — TLS/SSL via AmiSSL v5 (planned)

Upstream MicroPython ships axTLS (small, dated, no SNI) and mbedTLS (~250 KB
add). Neither fits the AmigaOS ethos when the platform already has a
perfectly serviceable TLS implementation — **AmiSSL** — as a shared library.
TLS code stays on the user's machine; we ship only the thin wrapper.

AmiSSL v5 is OpenSSL 3.x: TLS 1.3, ChaCha20-Poly1305, simpler
`TLS_client_method()`. v4 (OpenSSL 1.1.x) is EOL upstream and not worth a
deprecated-API codepath.

### How AmiSSL is opened

Unlike `bsdsocket.library`, AmiSSL has a master-indirection step. v5 has a
unified `OpenAmiSSLTags()` replacing the legacy v3/v4 triple:

```c
AmiSSLMasterBase = OpenLibrary("amisslmaster.library",
                               AMISSLMASTER_MIN_VERSION);
OpenAmiSSLTags(AMISSL_CURRENT_VERSION,                  // positional
               AmiSSL_UsesOpenSSLStructs, FALSE,        // opaque-only
               AmiSSL_GetAmiSSLBase,      &AmiSSLBase,
               AmiSSL_GetAmiSSLExtBase,   &AmiSSLExtBase,  // OS3 split
               AmiSSL_SocketBase,         (ULONG)SocketBase,
               AmiSSL_ErrNoPtr,           (ULONG)&errno,
               TAG_DONE);
```

- `AMISSL_CURRENT_VERSION` is positional, not a tag. Compiling against the
  v5 SDK produces a binary that *requires* AmiSSL v5 at runtime — that's
  the point of the master indirection.
- `AmiSSL_UsesOpenSSLStructs = FALSE` — MicroPython treats `SSL_CTX *` /
  `SSL *` / `X509 *` as opaque pointers.
- `AmiSSLExtBase` is OS3-only: v5's API surface is large enough they
  split it across two bases on m68k.
- `SocketBase` is supplied in the same call — `amiga_ssl_open()` must run
  *after* `amiga_socket_open()`.

Link against **`-lamisslstubs`** (function stubs needed for callbacks like
`SSL_CTX_set_verify(..., X509_free)`); don't use `-lamisslauto` (its
constructor-magic open fires before `SocketBase` is valid).

Headers: straight `#include <openssl/...>`, same as desktop.

### Callback ABI

OS3 callbacks under AmiSSL take args on the stack (not registers) and need
`STDARGS SAVEDS` annotations:

```c
STDARGS SAVEDS static int amiga_ssl_verify_cb(int preverify_ok,
                                              X509_STORE_CTX *ctx) { ... }
```

bebbo gcc has `__attribute__((stkparm))` and `__saveds`. Worth a
`AMISSL_CB` macro in `amiga_ssl.h`.

### Module shape

`ports/amiga/modssl.c` follows the upstream `ssl` API:

```python
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.verify_mode = ssl.CERT_REQUIRED
ctx.load_verify_locations("LIBS:amissl/certs/cacert.pem")
ws = ctx.wrap_socket(s, server_hostname="www.example.com")
```

`SSLContext` wraps `SSL_CTX *`; `SSLSocket` wraps `SSL *` + fd and
implements the stream protocol so it slots into `readline()`/`with`. Per
socket: `SSL_new` → `SSL_set_fd` → `SSL_set_tlsext_host_name` (SNI) →
`SSL_connect` → `SSL_read/write` → `SSL_shutdown` + `SSL_free`. Transient
`SSL_ERROR_WANT_READ/WRITE` → `MP_EAGAIN`; terminal errors raise `OSError`
with `ERR_error_string` text.

### Cert trust

Two paths:
1. `SSL_CTX_set_default_verify_paths()` picks up
   `LIBS:amissl/certs/cacert.pem` (Mozilla bundle the AmiSSL installer
   drops there).
2. `ctx.load_verify_locations(cafile=...)` / `cadata=...` for custom roots.

Don't ship a CA bundle in the binary — ~200 KB, gets stale fast.

### Variant gating + memory

```c
#define MICROPY_PY_AMIGA_SSL (MICROPY_PY_AMIGA_SOCKET)
```

`minimal` (no socket) → no SSL. If `amisslmaster.library` isn't installed,
`import ssl` raises `ImportError`.

OpenSSL 3.x is memory-hungry: `SSL_CTX` ~16 KB, each `SSL` ~48 KB, CA store
~200 KB. Two HTTPS connections push the 256 KB `standard` heap to the edge
— but AmiSSL allocates from system memory (`MEMF_ANY`), not MicroPython's
GC heap, so `-X heap=` doesn't help; the lever is total system Fast RAM.

### Deliberately *not* doing

- Server-side TLS — straightforward, but the Amiga isn't a webserver host.
- OpenSSL config-file integration — AmiSSL ships sane defaults.
- Asyncio-friendly handshake — `MICROPY_PY_ASYNCIO (0)`; revisit when
  asyncio lights up.
- Touching upstream `extmod/modssl_*.c` — AmiSSL-only, port-side module.

### Files

```
ports/amiga/modssl.c       — module def, SSLContext, SSLSocket
ports/amiga/amiga_ssl.c    — AmiSSL master open / init / cleanup
ports/amiga/amiga_ssl.h    — shared prototypes + base pointers
```

SDK ref:
[`jens-maus/amissl:dist/README-SDK`](https://github.com/jens-maus/amissl/blob/master/dist/README-SDK).
`Examples/httpget.c` and `Examples/https.c` are minimal working clients
worth mirroring.

### Open items

- **AmiSSL 4 fallback.** Land v5-only initially.
- **Cert pinning helpers** (`set_servername_callback`, raw-public-key APIs)
  — punt to a follow-up.
- **Async-friendly handshake** — tied to asyncio gating.
- **`urequests` / `mip`** — pure-Python, freeze into variant manifests
  once `ssl` lands.

---

## Other known limitations

| Issue | Status | Fix |
|-------|--------|-----|
| `try/except` in `@micropython.native` crashes | Known bug | Needs a 68k assembly NLR (`nlr68k.S`) saving D2–D7/A2–A5 in `nlr_buf_t` |
| `@micropython.viper` limited to 1 register local (D7) | `MAX_REGS_FOR_LOCAL_VARS = 1` | 68k-specific viper register allocator, or accept stack-based locals |

---

## Testing Strategy

1. **Emulator:** Amiberry confirmed working. FS-UAE, WinUAE are also options.
2. **CI:** Phase 11 — `.github/workflows/ports_amiga.yml` cross-compile check.
3. **Real hardware:** A1200 + accelerator, MiSTer Amiga core, etc.

### Option A: vamos on the host (recommended for iteration)

`vamos` is a userspace 68k/AmigaOS emulator from the `amitools` package.
Boots in ms, no GUI, integrates with `tests/run-tests.py`. Setup assumes
vamos installed at `~/vamos/` and activated via `pipenv`.

```sh
cd ~/vamos
pipenv run vamos --cpu 68020 \
    -V "mp:/path/to/micropython/tests" \
    --cwd mp:basics \
    -- /path/to/build/micropython string1.py
```

- `--cpu 68020` is **required** (vamos default is 68000, which faults on
  `m68020` instructions).
- `-V name:/host/path` mounts a host directory as an AmigaOS volume.
- `--cwd` sets cwd; with basename invocation, `sys.argv[0]`/`__file__`
  match host CPython exactly.
- `--` separates vamos options from binary args.

### `tools/amiga-vamos-run.sh` wrapper for run-tests.py

Mounts `tests/` as internal `mp:` volume, points cwd at the test's
directory, replaces script argument with basename, uses `--vols-base-dir`
so parallel workers don't collide on the auto `RAM:` volume, and `-q` so
vamos logs don't pollute captured stdout.

```sh
export MICROPY_MICROPYTHON="$(pwd)/tools/amiga-vamos-run.sh"
cd tests
./run-tests.py -d basics float io micropython misc
```

`AMIGA_VARIANT=minimal` / `=68040` selects build + `--cpu` flag.
`68020fpu` can't run under vamos (no 68881 emulation); the wrapper
detects and exits. Default variant is `standard`.

Exclude dirs that can't run on Amiga:

```sh
./run-tests.py -d basics float io micropython misc \
    -e "inlineasm|machine_|thread|extmod/ussl|extmod/uasync"
```

### Test-runner integration requirements

For `tests/run-tests.py` to work, the binary must:

- Accept `-X <option>` flags as no-ops (run-tests.py always emits
  `-X emit=bytecode`; macOS adds `-X realtime`).
- Return POSIX-style exit codes
  (`MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)`).
- Free `AllocVec`'d argv buffers before returning (vamos's orphan-memory
  check makes the process exit non-zero otherwise).

### Known vamos quirks

- bebbo's argv parser produces broken pointers under vamos with multi-arg
  invocations — `amiga_parse_args` parses `pr_Arguments` itself.
- `WaitForChar` returns 0 immediately (we use plain `FGetC`).
- `SetMode(fh, 1)` is a no-op (vamos already delivers one char at a time).

### Option B: inside Amiberry

For verifying real-AmigaOS-like behaviour. Mount the repo at `PY0:` (single
host-directory entry pointing at repo root, so `tools/` and `tests/` are
both reachable):

```sh
1> cd py0:tests
1> micropython basics/string_format.py
```

### Test suite summary

Snapshot under vamos via `tools/amiga-vamos-run.sh`, 2026-05-12:

| Directory | Files | Pass | Self-skip | Fail | Notes |
|-----------|------:|-----:|----------:|-----:|-------|
| `basics/`     | 574 | 490 | 83  | 1  | `struct1.py` (bebbo ABI alignment) |
| `float/`      | 68  | 54  | 11  | 3  | EXACT-mode precision at double-range edges |
| `io/`         | 16  | 12  | 3   | 1  | `argv.py` (vamos host-path rewriting) |
| `import/`     | 30  | 29  | 0   | 1  | `import_file.py` (vamos host-path rewriting) |
| `micropython/`| 108 | 43  | 18  | 47 | All native_*/viper_* — Phase 12 |
| `extmod/`     | 205 | 67  | 131 | 7  | vamos socket / select / time-quantum / vfs_userfs gaps |
| `misc/`       | 14  | 6   | 8   | 0  | Skips: settrace, sys_exc_info, cexample |
| `cmdline/`    | 25  | 9   | 2   | 14 | Unix-port-specific (REPL banner, `-v`, terminal editing) |
| `stress/`     | 13  | 12  | 0   | 1  | `bytecode_limit.py` parser memory pressure |

Aggregate: **722 pass / 256 self-skip / 75 fail** out of 1053 files.
Excluding the 47 Phase-12 native/viper failures, unix-port-specific cmdline
tests, vamos emulation gaps, and the `bytecode_limit.py` parser edge case,
**4 individual tests fail** — all real platform differences.

### Known test failures (real platform differences, not port bugs)

| Test | Cause |
|------|-------|
| `basics/struct1.py` | `struct.calcsize("97sI") == 102`, test expects 104. bebbo gcc on m68k uses 2-byte `int` alignment per the AmigaOS m68k ABI; CPython on x86 uses 4. Both platform-correct. |
| `float/float_parse*.py` | A few edge cases (very long mantissa with very negative exponent; `1e+300` vs `9.999...e+299` differing by 2 ULP; `1e4294967301` not detected as overflow) come out 1–2 ULP off. Bebbo's 80-bit long double soft-float has just enough precision loss that EXACT-mode parsing can't always nail the closest double. |
| `float/float_format_accuracy.py` | repr round-trip rate ~72% vs ≥99.7% expected. Same long-double precision tax. |

### Bebbo soft-float library bugs

bebbo gcc 6.5b on `-msoft-float` ships incorrect floating-point helpers in
libgcc / clib2 / libnix. `ports/amiga/floatconv.c` overrides each one (some
directly, some via `--wrap` because clib2 fat-packs them with `__muldf3`
and friends).

| Routine | Bug | Trigger |
|---------|-----|---------|
| `__floatunsidf`, `__floatundidf`, `__floatdidf` | High-bit-set values convert to garbage | `float("9"*51 + "e-39")`, `array.array('Q', [...])`, any `mp_obj_new_int_from_uint(>2^31)` → double |
| `__eqdf2`, `__nedf2`, `__ledf2`, `__gedf2`, `__ltdf2`, `__gtdf2` | NaN treated as ordered/equal | `==`/`!=`/`<=`/`>=` with NaN; `math.isclose`, set/dict NaN keys, `x != x` NaN check |
| `pow(-1, NaN)` (libnix) | Returns `1.0`, CPython expects NaN | `(-1) ** float('nan')` |
| `tgamma(-inf)` (libnix) | Returns `+inf`, CPython raises | `math.gamma(-inf)` |
| `__fixdfsi` (clib2) | Calls `IEEEDPFix` which under vamos aborts the whole emulator on NaN | `hash(float('nan'))` (gcc 6.5 emits `__fixdfsi` for `mp_float_hash`'s bit-level body after opt) |

### On-device batch runner (Amiberry)

No CPython to diff against, so relies on a `.exp` file next to every `.py`
test. Upstream only ships `.exp` where MicroPython output is *expected* to
differ; generate the rest first:

```sh
tools/amiga-gen-exp.py tests/basics tests/float tests/io \
    tests/import tests/micropython tests/misc tests/cmdline tests/stress
```

Without this, every test whose expected output contains `Error` (lots of
`TypeError` prints in `basics/`) gets flagged by the runner's `"Error" in
output` fallback.

Set AmigaDOS stack to at least 32 KB — the default (4–8 KB) is too small
for deep compile-time recursion (e.g. `try_except_break.py`), which shows
up as `Software Failure 8000000B` rather than a `RuntimeError`:

```
1> Stack 32768
1> cd py0:
1> micropython tools/amiga-runtests.py tests/basics
1> micropython tools/amiga-runtests.py tests/float T:my-results/
```

Result dir default `T:mp-test-results/` (RAM). On FAIL, `<dir>/<test>.py.out`
and `<dir>/<test>.py.exp` land there; on pass/skip, stale ones are deleted.

Generated `.exp` files aren't checked in — add `tests/**/*.py.exp` to
`.git/info/exclude` if `git status` chatter bothers you.
