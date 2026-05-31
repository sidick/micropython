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
| 11 | CI build workflow | ✅ |
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
| 28 | TLS/SSL via AmiSSL v5 | ✅ |
| 29 | `urequests` frozen HTTP/HTTPS client | ✅ |
| 30 | Intuition requester dialogs (`amiga.intuition`) | ✅ |
| 31 | ASL file requester (`amiga.asl`) | ✅ |
| 32 | ARexx polish (`rexx_exists` / `rexx_list` / persistent `RexxClient`) | ✅ |
| 33 | `platform.amiga_info()` + frozen `platform.py` | ✅ |
| 34 | Frozen `os.py` extensions + AmigaOS-aware `os.path` | planned |
| 35 | `amiga.icon` — `.info` file read / write / manipulation | ✅ |
| 36 | `amiga.catalog` — `locale.library` catalog lookup | ✅ |
| 37 | `amiga.datatypes` — `datatypes.library` file recognition | planned (low priority) |

### Non-goals (initially)

- Full Workbench GUI / Intuition windows — Phases 30/31 add modal
  requesters only; arbitrary window/widget surfaces stay out of scope
- 68000 alignment-safe build — target 68020+ first

### Surface-compatibility policy with other Amiga Python ports

Two other Python-on-Amiga efforts exist and surface in design
conversations:

- **OoZe1911/micropython-amiga-port** — a parallel MicroPython
  AmigaOS 3.x port. Different deltas (depth of `amiga.*` C
  surface, the Phase 5 native emitter, three-variant CPU/FP
  matrix, dynamic heap, the library proxy + `.fd` trampoline)
  but a converging top-level shape (`amiga.intuition`,
  `amiga.asl`, `urequests`, `platform`, ARexx surface).
- **OS4 Python** — a port of *CPython 2.x* to AmigaOS 4 /
  PowerPC. Six Amiga-specific module names documented on
  [wiki.amigaos.net](https://wiki.amigaos.net/wiki/AmigaOS_Manual:_Python_Modules_and_Packages):
  `amiga`, `arexx`, `asl`, `catalog`, `icon`, `installer`.

**Policy: surface-compatible where it's cheap, not byte-compatible.**

- Match their **module names** (`amiga.intuition`, `amiga.asl`,
  `amiga.icon`, `amiga.catalog`, etc.) so users moving scripts
  between ports find familiar import paths.
- Match their **function signatures** where the underlying
  AmigaOS API fits cleanly. Phases 30–36 mirror OoZe / OS4
  shapes deliberately.
- Where their choice doesn't fit OS3 / Python 3 / MicroPython
  (Posix-style `chmod` translation in Phase 34; OS4-interface-
  based library calls; CPython-2 stdlib expectations), pick the
  shape that fits **our** target and document the divergence in
  the per-phase out-of-scope list.

**Don't promise true compatibility:**

- OS4 Python is CPython 2.x with the full CPython stdlib and
  C-extension ecosystem. We're MicroPython on OS3 / 68k. Any
  non-trivial OS4 Python script will hit Python-2 idioms or
  stdlib modules we can't ship; mirroring AmigaOS-specific module
  names doesn't change that.
- OoZe1911 has feature deltas we've deliberately chosen not to
  match (`smtplib` + `email` module, OS-side path conventions
  baked deeper than ours). When their choice is reasonable, we
  copy. When ours is materially better (the library proxy, native
  emitter, timer-backed timing), we keep ours.

The win from surface-compat is *low friction* — someone with an
`import amiga.icon; icon.read(...)` script from OS4 has a chance
of running it without edits. The win from *not* over-committing
is avoiding broken promises about strict portability.

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
└── variants/{standard,68020fpu,68040}/
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

## Phase 11 — CI build workflow ✅

`.github/workflows/ports_amiga.yml`, manually triggered via
`workflow_dispatch` with a `ref` input (branch / tag / commit, default
`amiga-port`). One job runs inside `stefanreinauer/amiga-gcc:latest`,
builds mpy-cross once, then builds all four variants sequentially with
`make -j$(nproc)` inside each. Each variant's binary is uploaded as a
separate artifact named
`micropython-amiga-<variant>-<ref>-<sha>`.

`tools/amiga-build.sh` mirrors the workflow exactly for local container
builds — same image, same commands, same output paths — so local and CI
binaries are bit-identical (see *Toolchain*).

**Required submodule.** `make -C ports/amiga submodules` runs before the
build; the frozen-content rule unconditionally checks for
`lib/micropython-lib/README.md` whenever `FROZEN_MANIFEST` is set, even
though `manifest.py` only freezes `ports/amiga/modules`.

**Timing.** Image pull dominates at ~87 s; the rest (checkout, submodule,
mpy-cross, four variant builds, four uploads) is ~45 s — total wall time
~130 s, billed compute matches.

Possible follow-ups (deferred):

- **Image tarball cache.** `docker save | gzip` + `actions/cache` would
  drop repeat-run wall time from ~130 s to ~40 s. Not worth the
  complexity for a manual-trigger workflow.
- **Release flow.** When the input `ref` is a tag, also publish a draft
  GitHub release with the four binaries attached.
- **AmiSSL SDK.** Phase 28 will need the SDK layered into the build
  environment — see Phase 28 "SDK provisioning" subsection for options.

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
`AMIGA_VARIANT=68040` selects the build.

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
| `standard` (default) | `-m68020 -msoft-float` | 256 KB | Any 68020+, no FPU. Default for stock A1200 / unaccelerated 68030 |
| `68020fpu` | `-m68020 -m68881` | 512 KB | 68020/30 + 68881/2 (A2630, A3000). No libgcc soft-float wraps |
| `68040` | `-m68040` | 1 MB | 68040 built-in FPU (A3640, A4000/040) |

Text-segment sizes: standard 356 KB, 68020fpu 330 KB, 68040 339 KB.

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

## Phase 28 — TLS/SSL via AmiSSL v5 ✅

Landed in six commits, end-to-end verified against `www.python.org`
on 2026-05-31 with `CERT_REQUIRED` chain validation
(`HTTP/1.1 200 OK`, 53 912 B HTML). The implementation log lives in
[docs/phase28-ssl-plan.md](phase28-ssl-plan.md) — measured heap
costs, the AmigaOS capath trailing-slash gotcha, and the modern-CDN
TLS 1.3 limitation tracked as a follow-up. The section below is
the design and rationale — *what* and *why*; the companion doc
covers *how it shipped*.

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
ctx.load_verify_locations(capath="AmiSSL:certs/")
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
   the `AmiSSL:certs/` c_rehash dir (the hashed CAs the AmiSSL installer
   drops there).
2. `ctx.load_verify_locations(cafile=...)` / `cadata=...` for custom roots.

Don't ship a CA bundle in the binary — ~200 KB, gets stale fast.

### Variant gating + memory

```c
#define MICROPY_PY_AMIGA_SSL (MICROPY_PY_AMIGA_SOCKET)
```

All three shipped variants link AmiSSL. If `amisslmaster.library` isn't
installed at runtime, `import ssl` raises `ImportError`.

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

### SDK provisioning (CI + container build)

`stefanreinauer/amiga-gcc:latest` ships only vbcc-side AmiSSL stubs
(`proto/amisslmaster.h`, `proto/amissl.h`, `inline/amissl_protos.h`
under `…/vbcc/…`). The GCC tree has no `<openssl/*>` headers, no
`<libraries/amissl[master].h>`, and no `libamisslstubs.a`/
`libamisslauto.a`. Phase 28 needs the AmiSSL v5 SDK layered in. Three
options when we get there:

1. **Extend the container image.** Custom Dockerfile
   `FROM stefanreinauer/amiga-gcc:latest` that fetches the SDK from
   <https://github.com/jens-maus/amissl/releases>, unpacks headers into
   `/opt/amiga/m68k-amigaos/include/` and libs into
   `/opt/amiga/m68k-amigaos/lib/`. Push to GHCR; CI workflow and
   `tools/amiga-build.sh` both point at it. Cleanest, one-time setup.
2. **Workflow step.** Download + unpack the SDK on every CI run.
   Simpler setup, but adds 10–30 s per run and a network dependency on
   GitHub releases.
3. **Vendored in-tree.** Drop the headers + stubs under
   `ports/amiga/amissl-sdk/`. No external deps; downside is shipping a
   few MB of third-party code in the repo.

Pick when Phase 28 actually starts; option 1 is the current preference.

### Open items

- **AmiSSL 4 fallback.** Land v5-only initially.
- **Cert pinning helpers** (`set_servername_callback`, raw-public-key APIs)
  — punt to a follow-up.
- **Async-friendly handshake** — tied to asyncio gating.
- **`urequests` / `mip`** — pure-Python, freeze into variant manifests
  once `ssl` lands. (Phase 29 picks up `urequests`.)

---

## Phase 29 — `urequests` frozen HTTP/HTTPS client ✅

Shipped in five steps over commits `3455810e4` (Step 1),
`de03cc93b` (Steps 2–4), and the doc-flip in this section.
End-to-end verified under Amiberry against
`www.example.com` (HTTP + gzip), `httpbin.org` (HTTP +
chunked + POST echo for both `json=` and `data=dict`), and
`www.python.org` (HTTPS, 48 887 B of HTML through the Phase 28
TLS stack). See [docs/phase29-urequests-plan.md](phase29-urequests-plan.md)
for the step-by-step log.


Convenience client built on top of Phase 9 (`socket`) and Phase 28
(`ssl`). Same shape as upstream `micropython-lib`'s `requests`:

```python
import urequests
r = urequests.get("https://www.python.org/")
print(r.status_code)
print(r.text[:200])
r.close()
```

### Scope

- `urequests.get / post / put / delete / patch / head`
- `Response` with `.status_code`, `.reason`, `.headers`, `.text`,
  `.content`, `.json()`, `.close()`
- HTTP/1.0 wire format (`Connection: close`), HTTP/1.1 read tolerance
- `https://` via the existing `ssl.SSLContext` setup
  (`CERT_REQUIRED` + `set_default_verify_paths()` against the AmiSSL
  trust store)
- Optional gzip decompression via the existing `deflate` module
- POSTing `data=` (urlencoded), `json=` (auto-serialised), `headers=` overrides

### Out of scope

- Persistent / keep-alive connections — needs HTTP/1.1 keep-alive
  state-machine, deferred
- Sessions / cookies — deferred
- Streaming uploads / `multipart/form-data` — deferred
- HTTP/2, async — deferred (no asyncio yet)

### Known limitation

Inherits the Phase 28 AmiSSL ↔ modern-CDN issue
([phase28-ssl-plan](phase28-ssl-plan.md)): `urequests.get` against
TLS-1.3-eager hosts (Cloudflare, GitHub) will succeed the handshake
then fail the write. Direct origins that still negotiate TLS 1.2 by
default work.

### Files

```
ports/amiga/modules/urequests.py        — frozen pure-Python module
docs/phase29-urequests-plan.md          — step plan
tests/ports/amiga/test_urequests_smoke.py     — on-target smoke (Amiberry, AmiSSL installed)
```

All three shipped variants include it.

---

## Phase 30 — Intuition requester dialogs ✅

`amiga.intuition` C sub-module exposing `intuition.library`'s
`EasyRequest` family. Three entry points:

```python
from amiga import intuition

idx = intuition.easy_request("Title", "Body line one.\nBody line two.",
                             ["Yes", "No", "Cancel"])
ok  = intuition.auto_request("Replace existing file?", yes="Yes", no="No")
intuition.message("Done.", button="OK")
```

### Scope

- `easy_request(title, body, buttons)` → int (0-based button index;
  rightmost button is always the "cancel" / 0 convention)
- `auto_request(body, yes="Yes", no="No")` → bool, two-button wrapper
- `message(body, button="OK")` → None, single-button notice
- printf-escape safety (raw user strings are passed as `%s` args,
  not as the format string)
- Latin-1 codec for the title/body so non-ASCII text renders
  cleanly under AmigaOS Topaz/CP-1252 displays

### Out of scope

- Arbitrary window/widget surfaces — see "Non-goals"
- Custom gadgets, file/font/screen pickers (Phase 31 handles file)
- Non-blocking / async requesters — modal only

### Dependencies

- `intuition.library` opened lazily on first call (cached base)
- Uses Phase 17's library-calling infrastructure as fallback if
  preferred over explicit C, but a direct C module is probably
  simpler given how concentrated the API surface is

### Files

```
ports/amiga/modintuition.c              — C module (~180 LOC)
ports/amiga/modules/amiga.py            — adds `import _intuition as intuition`
tests/ports/amiga/test_intuition_smoke.py     — vamos arg-shape smoke test
docs/phase30-intuition-plan.md          — step plan
```

Variants: all four. ~1.5 KB text per variant, no network/SSL deps.

### Status — done

Three Python entry points, all backed by a single `EasyRequestArgs`
call through `intuition.library` v36+:

| Function | Returns |
|---|---|
| `intuition.easy_request(title, body, buttons)` | int — 0-based leftmost button index |
| `intuition.auto_request(body, yes="Yes", no="No")` | bool — True iff Yes clicked |
| `intuition.message(body, button="OK")` | None |

Translation: AmigaOS conventionally numbers buttons rightmost-is-0
(`EasyRequest` returns 0 for the right gadget, 1 for the next, …,
N-1 for the leftmost). The module flips that to Python's natural
0-based-leftmost so `buttons[easy_request(...)]` does the right thing.

`es_TextFormat` is hard-coded to `"%s"` and the body is passed as the
single varargs arg, so a `%` in the body is rendered literally rather
than interpreted as a printf directive.

Vamos has a no-op `EasyRequest` stub — useful for confirming the call
threads through cleanly but not for end-to-end visual checks. Those
need Amiberry or real hardware. Verified working under Amiberry
2026-05-31; intuition.library opens its own screen if no public
screen is currently up, so the requester appears regardless of
whether Workbench is loaded.

---

## Phase 31 — ASL file requester ✅

`amiga.asl` C sub-module wrapping `asl.library`'s
`AslRequest(ASL_FileRequest, ...)`. Native Amiga file chooser
dialog usable from both CLI and Workbench launches.

```python
from amiga import asl

path = asl.file_request(title="Pick a script",
                        initial_drawer="Work:scripts/",
                        pattern="#?.py")
if path is None:
    print("cancelled")

# Save dialog
out = asl.file_request(title="Save as", save=True,
                       initial_file="output.txt")

# Multi-select returns list
paths = asl.file_request(multi=True)

# Drawer-only
drawer = asl.file_request(drawers_only=True)
```

### Scope

- `file_request(...)` keyword args:
  `title`, `initial_drawer`, `initial_file`, `pattern`,
  `save=False`, `multi=False`, `drawers_only=False`
- Returns `str` for single-pick, `list[str]` for `multi=True`,
  `None` for user-cancelled
- Paths joined via `dos.library AddPart()` so volume separators
  come out right on AmigaOS
- Latin-1 codec for filenames

### Out of scope

- Font / screen / draw mode / palette requesters — file is the only
  one with clear practical use from a scripted REPL
- Custom hooks / per-entry filter callbacks — `pattern` is enough

### Dependencies

- `asl.library` opened lazily on first call (cached base)
- Same C-or-Phase-17 trade-off as Phase 30

### Files

```
ports/amiga/modasl.c                    — C module (~280 LOC)
ports/amiga/modules/amiga.py            — adds `import _asl as asl`
tests/ports/amiga/test_asl_smoke.py           — vamos arg-shape smoke test
docs/phase31-asl-plan.md                — step plan
```

Variants: all three shipped variants. ~1.2 KB text cost per variant.

### Status — done

One entry point, behaviour driven entirely by kwargs:

| Kwargs                                                | Returns      |
|-------------------------------------------------------|--------------|
| `(title, initial_drawer, initial_file, pattern)`      | `str` — full path of the picked file |
| `save=True`                                           | `str` — full path (editable filename gadget) |
| `drawers_only=True`                                   | `str` — drawer path (no file component) |
| `multi=True`                                          | `list[str]` — one full path per shift-clicked file |
| user clicked Cancel                                   | `None`       |
| `multi=True` + `save=True`                            | `ValueError` |

`asl.library` is famously stack-hungry: a default ~4 KB shell stack
trips a CHK exception (`0x80000006`) on the post-pick code path
even though the dialog renders fine. `file_request` runs the bare
`AllocAslRequest` + `AslRequest` calls on a 32 KB scratch stack
via `StackSwap` so callers don't have to remember `Stack 32768`
at the shell prompt; the path-build and string allocation happen
back on the original stack so the GC's stack-scan range stays
correct.

Path buffer is 1024 bytes (vs the 512 used by older `modamiga.c`
surfaces) to comfortably accommodate long-name filesystems
(SFS / PFS3 / FFS2).

---

## Phase 32 — ARexx polish ✅

Three additions to the existing ARexx surface (`amiga.rexx_send`,
`amiga.rexx_open`/`recv`/`reply` from Phase 18):

```python
import amiga

# Phase 32 additions:
if amiga.rexx_exists("IBROWSE"):
    rc, html = amiga.rexx_send("IBROWSE", "QUERY ITEM=URL")

for port in amiga.rexx_list():
    print(port)

# Persistent client -- caches the reply MsgPort across sends so a
# tight loop doesn't pay CreateMsgPort/DeleteMsgPort per call.
with amiga.RexxClient("DOPUS.1") as ib:
    ib.send("LISTER NEW")
    rc, result = ib.send("LISTER QUERY 1 PATH", check=False)
```

### Scope

- `amiga.rexx_exists(name)` → `bool`. Thin wrapper around
  `FindPort(name)` with `Forbid()` / `Permit()` braces.
- `amiga.rexx_list()` → `list[str]`. Walks `SysBase->PortList`,
  returning every public port's `ln_Name`.
- `amiga.RexxClient(host)` class:
  - Opens a reply `MsgPort` in `__init__`; reuses it across calls.
  - `.send(command, check=True)` returns the same shape as
    `amiga.rexx(host, command)`.
  - `.close()` / `__exit__` / `__del__` tear down the reply port.
  - Tracked in an at-exit cleanup chain so a forgotten close
    doesn't leak the MsgPort on process exit.

### Out of scope

- Migrating the existing `amiga.rexx_*` surface into an
  `amiga.rexx.*` sub-module — breaking change for callers and
  the inbound server side (Phase 18) is already deeply embedded
  in the module flat namespace
- Reshaping the one-shot `amiga.rexx_send` / `amiga.rexx` helpers
  to use the client class internally — the per-call MsgPort
  cost is invisible for a single send; the polish is for tight
  loops

### Dependencies

- `rexxsyslib.library` (already opened lazily by Phase 18
  `amiga.rexx_send`)

### Files

```
ports/amiga/modamiga.c                  — five new C entries
                                          (rexx_exists, rexx_list,
                                          rexx_client_open/close/send)
ports/amiga/modules/amiga.py            — RexxClient Python facade
tests/ports/amiga/test_rexx_polish.py         — vamos arg-shape smoke
docs/phase32-arexx-polish-plan.md       — step plan
```

Variants: all three. ~1.4 KB text per variant.

### Status — done

Five new entry points alongside the Phase 18 ARexx surface:

| Entry                                | Returns      |
|--------------------------------------|--------------|
| `amiga.rexx_exists(name)`            | `bool` (Forbid-fenced FindPort) |
| `amiga.rexx_list()`                  | `list[str]` (Forbid-fenced walk of `SysBase->PortList`) |
| `amiga.RexxClient(host)`             | persistent client; `.send(cmd, check=True)`, context manager, `__del__` cleanup |
| `amiga.rexx_client_open` / `_close` / `_send` | low-level primitives that back the class |

The class's reply `MsgPort` is registered in a 16-slot static
chain at construction. `amiga_rexx_shutdown` (already wired into
`main.c`'s cleanup path) walks the chain on process exit and
`DeleteMsgPort`s anything still live, so a script that forgot
`close()` doesn't leak the port. The send body was factored out
of `amiga_rexx_send_fn` into `amiga_rexx_send_via_port` so the
one-shot and persistent paths share the wire format / Ctrl+C
latch / argstring handling.

---

## Phase 33 — `platform.amiga_info()` ✅

Frozen CPython-shaped `platform.py` module surfacing AmigaOS
identity. Modeled on OoZe1911's port:

```python
>>> import platform
>>> platform.system()
'AmigaOS'
>>> platform.machine()
'68020'
>>> platform.amiga_info()
'CPU: 68020 | FPU: 68881 | Chipset: AGA | Kickstart: 45.57 | Chip: 1856KB | Fast: 14336KB'
```

### Scope

- Six new C accessors on the `amiga` module:
  - `amiga.cpu()` → `str` (e.g. `"68020"`, `"68040"`)
  - `amiga.fpu()` → `str` (e.g. `"none"`, `"68881"`, `"68040"`)
  - `amiga.chipset()` → `str` (`"OCS"` / `"ECS"` / `"AGA"`)
  - `amiga.kickstart()` → `str` (e.g. `"45.57"`)
  - `amiga.chipmem()` → `int` (bytes available)
  - `amiga.fastmem()` → `int` (bytes available)
- Frozen `platform.py` wrapping the above with the CPython API
  shape (`system()`, `machine()`, `processor()`, `version()`,
  `release()`, `python_implementation()`, `python_version()`,
  `platform()`, `node()`) plus the convenience `amiga_info()`.

### Out of scope

- Full CPython `platform` parity (`uname()`, `linux_distribution()`,
  etc.) — beyond what's meaningful on AmigaOS
- Caching the strings; they're cheap to recompute and the input
  state can change (memory free shifts continuously)

### Files

```
ports/amiga/modamiga.c                  — six accessor functions
ports/amiga/modules/platform.py         — frozen facade
tests/ports/amiga/test_platform_smoke.py      — vamos smoke
docs/phase33-platform-plan.md           — step plan
```

Variants: all three. ~1.6 KB text per variant (six C accessors +
the frozen module + the lazy graphics.library hook).

### Status — done

| `amiga.*` accessor   | `platform.*` wrapper          | Returns |
|----------------------|-------------------------------|---------|
| `amiga.cpu()`        | `platform.machine` / `processor` | str — highest `AttnFlags` CPU bit (`"68000"` .. `"68060"`) |
| `amiga.fpu()`        | `platform.fpu`                | str — `"none"` / `"68881"` / `"68882"` / `"68040"` |
| `amiga.chipset()`    | `platform.chipset`            | str — `"OCS"` / `"ECS"` / `"AGA"` (lazy `graphics.library`) |
| `amiga.kickstart()`  | `platform.release` (and `version` prefix) | str — `"VV.RR"` |
| `amiga.chipmem()`    | `platform.chipmem`            | int — bytes currently free in Chip RAM |
| `amiga.fastmem()`    | `platform.fastmem`            | int — bytes currently free in Fast RAM |

Plus the CPython-standard `platform.system()` → `"AmigaOS"`,
`node()` → `"Amiga"`, `python_implementation()` /
`python_version()`, and the convenience
`platform.platform()` ("AmigaOS-`<kick>`-`<cpu>`-MicroPython_`<pyver>`")
and `platform.amiga_info()`
("CPU: 68020 | FPU: 68881 | Chipset: AGA | Kickstart: 45.57 |
Chip: 1856KB | Fast: 14336KB").

Values reflect runtime AmigaOS state, not compile-time targeting:
a `standard` (68020 / soft-float) binary running on a 68040
reports `"68040"` / `"68040"` for `cpu()` / `fpu()`.

---

## Phase 34 — Frozen `os` extensions + AmigaOS-aware `os.path` (planned)

Fill the remaining `os` / `os.path` gap that surfaces when comparing
our port to OoZe1911's:

```python
import os
os.makedirs("Work:scripts/sub/dir", exist_ok=True)
for root, dirs, files in os.walk("Work:"):
    print(root, len(files))
os.chmod("DH0:foo", 0)              # AmigaDOS protection bits
mask = os.getprotect("DH0:foo")     # readable / writable / archive / ...

os.path.join("Work:", "scripts", "foo.py")   # 'Work:scripts/foo.py'
os.path.abspath("foo.py")                    # uses cwd, knows about volumes
os.path.isabs("Sys:Prefs")                   # True
os.path.normpath("Work:scripts/../bin")      # 'Work:bin'
```

`extmod/modos.c` registers `os` as `MP_REGISTER_EXTENSIBLE_MODULE`, so
a frozen `ports/amiga/modules/os.py` automatically merges into the C
namespace: the C-side `chdir` / `getcwd` / `listdir` / `mkdir` /
`remove` / `rename` / `rmdir` / `stat` / `statvfs` (plus our Phase 20
`getenv` / `putenv` / `unsetenv`) stay live, and the frozen file adds
the rest.

### Scope

- `os.chmod(path, mask)` and `os.getprotect(path)` as new C entries
  on `modos.c`, backed by `SetProtection()` / `Examine()` from
  `dos.library`. `mask` is the AmigaDOS bit set (`FIBF_*`), not a
  Posix mode — the bits map one-to-one with the on-disk encoding so
  round-tripping is lossless.
- `FIBF_*` constants exposed on `os.*` for readability
  (`FIBF_READ`, `FIBF_WRITE`, `FIBF_EXECUTE`, `FIBF_DELETE`,
  `FIBF_ARCHIVE`, `FIBF_PURE`, `FIBF_SCRIPT`, `FIBF_HIDDEN`).
- Frozen `os.py` adds pure-Python `makedirs(name, exist_ok=False)`
  and `walk(top, topdown=True)` — same shapes as CPython, aware of
  AmigaOS volume separators.
- Frozen `_ospath.py` aliased as `os.path` providing
  `join` / `split` / `splitext` / `basename` / `dirname` / `exists` /
  `isfile` / `isdir` / `isabs` / `abspath` / `normpath`, all aware
  of `Volume:dir/file` syntax (volume separator `:` once, dir
  separator `/`).

### Out of scope

- Translating Posix mode bits to AmigaDOS bits for `chmod` —
  ambiguous (no clean rwx ↔ rwed mapping). Users wanting Posix shape
  call `chmod` with the AmigaDOS mask directly.
- `os.path.expanduser` (no `~` concept on AmigaOS), `expandvars`
  (would need to walk `dos.library GetVar` — separate phase if
  needed)
- `os.walk` follow-symlinks / on-error callback — keep minimal
  shape; users can wrap if needed
- Full CPython `os.path` parity (`commonpath`, `relpath`, etc.)
  — easy to add later if a real call site needs them

### Dependencies

- `dos.library` (already opened for Phase 20+); `SetProtection` /
  `Examine` are existing entry points

### Files

```
ports/amiga/modos.c                     — chmod + getprotect C entries
                                          + FIBF_* constants
ports/amiga/modules/os.py               — frozen extension (makedirs,
                                          walk, `import _ospath as path`)
ports/amiga/modules/_ospath.py          — frozen AmigaOS-aware path
                                          helpers
tests/ports/amiga/test_os_smoke.py            — vamos smoke
docs/phase34-os-path-plan.md            — step plan
```

Variants: all three.

---

## Phase 35 — `amiga.icon` ✅

`amiga.icon` is a C sub-module wrapping `icon.library` for full
`.info` file manipulation. Goes beyond the read-only
`amiga.tooltype()` we already had (which only looks up *one*
tooltype on the *launched* tool icon).

```python
from amiga import icon

# Read a .info file (path without the .info suffix, AmigaOS convention)
dobj = icon.read("Work:Tools/Editor")
print(dobj.type)            # 'tool' / 'project' / 'drawer' / ...
print(dobj.default_tool)    # 'C:Ed' or similar for project icons
print(dobj.stack_size)      # 8192

# Tooltypes are exposed as a dict-shaped accessor.
print(dobj.tooltypes["WINDOW"])
dobj.tooltypes["FONT"] = "topaz.font/8"
dobj.tooltypes["FLAG"] = None              # bare flag tooltype (no '=')
del dobj.tooltypes["OLD_KEY"]

# Position on the parent drawer.
dobj.current_x, dobj.current_y = 16, 24

# Write back.
icon.write("Work:Tools/Editor", dobj)

# Create a fresh project icon from scratch.
new = icon.new(icon.WBPROJECT, default_tool="C:Ed",
               tooltypes={"WINDOW": "CON:0/0/640/256/Title"})
icon.write("Work:Notes", new)
new.close()
```

### Surface

| Call | Returns | Notes |
|------|---------|-------|
| `icon.read(path)` | `DiskObject` | `GetDiskObject` wrapper. `OSError(ENOENT)` if the `.info` doesn't exist. |
| `icon.write(path, dobj)` | `None` | `PutDiskObject`. `OSError(EIO)` on failure. Doesn't refresh the parent Workbench window (out of scope — that's `workbench.library`'s `UpdateWorkbench`). |
| `icon.new(type, **kwargs)` | `DiskObject` | `GetDefDiskObject(type)` for the system default image. Kwargs `default_tool` / `stack_size` / `current_x` / `current_y` / `tooltypes` applied in one pass. `OSError(EINVAL)` if the type code isn't recognised. |
| `icon.WBDISK / WBDRAWER / WBTOOL / WBPROJECT / WBGARBAGE / WBDEVICE / WBKICK / WBAPPICON` | int | Raw `do_Type` values from `<workbench/workbench.h>`. |
| `icon.DiskObject` | type | Re-exported so `isinstance(d, icon.DiskObject)` works. |

`DiskObject` attributes:

| Attr | Type | R/W | Notes |
|------|------|-----|-------|
| `.type` | `str` | R | `"disk"` / `"drawer"` / `"tool"` / `"project"` / `"garbage"` / `"device"` / `"kick"` / `"appicon"`. Falls back to the raw int for any future code. |
| `.default_tool` | `str` or `None` | R/W | `None` clears (icon.library treats NULL/empty as "none"). |
| `.stack_size` | `int` | R/W | `do_StackSize`. |
| `.current_x`, `.current_y` | `int` | R/W | Icon position on the parent drawer. |
| `.tooltypes` | `DiskObjectTooltypes` | R/W via methods | Dict-shaped mapping (see below). |
| `.close()` / `__del__` | — | — | Releases the underlying allocation. Idempotent. |

`DiskObjectTooltypes` mapping methods:

| Op | Notes |
|----|-------|
| `tt[k]` | Returns the value as `bytes` (empty `b""` for flag-style entries). `KeyError` if absent. |
| `tt[k] = v` | `v` is `str` / `bytes` (writes `"KEY=VALUE"`) or `None` (writes flag-style `"KEY"`). `TypeError` for anything else. |
| `del tt[k]` | `KeyError` if absent. |
| `k in tt` / `len(tt)` / `iter(tt)` | Standard dict semantics; iteration yields keys as `str`. |
| `tt.keys()` / `.values()` / `.items()` / `.get(k, default)` | List-returning helpers (no view objects). |

Ownership: a freshly-read DiskObject has icon.library-owned
`do_DefaultTool` / `do_ToolTypes`; the first mutation deep-copies
them into Python-owned `AllocVec` buffers so subsequent edits are
free. `.close()` swaps the original pointers back in before
`FreeDiskObject` so the library's teardown only sees memory it
allocated.

### Out of scope

- Editing the icon's image data (`do_Gadget->GadgetRender` /
  `SelectRender`) — that's a substantial surface (planar image
  munging, palette handling); separate phase if ever needed.
- App icons (`AddAppIcon`) — that's
  `workbench.library`, not `icon.library`.
- NewIcon / GlowIcon / OS3.5+ ColorIcon extended IFF chunks —
  the basic `do_Gadget` planar icon is enough for round-tripping.
- Workbench-window refresh after `icon.write` — caller can drive
  that via ARexx (`WORKBENCH UPDATE`) if they need it.

### Dependencies

- `icon.library` (lazy open, same pattern as Phases 30 / 31 / 33).
  Already partially used internally for `amiga.tooltype` /
  `amiga.wb_selected_files` via the cached DiskObject in main.c —
  Phase 35 exposes the broader surface.

### Files

```
ports/amiga/modicon.c                   — C module + DiskObject type
ports/amiga/modules/amiga.py            — `import _icon as icon`
tests/ports/amiga/test_icon_smoke.py          — vamos arg-shape + RAM: round trip
docs/phase35-icon-plan.md               — step plan
```

Variants: all three. ~5 KB text per variant (DiskObject + tooltype
mapping protocol + write + new).

---

## Phase 36 — `amiga.catalog` ✅

`amiga.catalog` wraps `locale.library`'s catalog lookup so
localized apps can read their translated strings, plus surfaces
the system's preferred language.

```python
from amiga import catalog

print(catalog.language())          # 'english' / 'german' / 'français'

# Stock-Workbench tip: pass built_in_language= so locale.library
# actually loads a translation file rather than short-circuiting
# on "you already have English built in".
with catalog.open("MyApp.catalog", version=1,
                  language="english",
                  built_in_language="german") as cat:
    print(cat.lookup(1, "Default English string"))
    print(cat.lookup(2, "Cancel"))
```

### Surface

| Call | Returns | Notes |
|------|---------|-------|
| `catalog.open(name, version=0, language=None, built_in_language=None)` | `Catalog` | `OpenCatalogA(NULL, name, [OC_Version, OC_BuiltInLanguage?, OC_Language?, TAG_DONE])`. `OSError(ENOENT)` if `locale.library` returns NULL (catalog not found, or language matches `built_in_language` so nothing to load). `OSError(EIO)` if `locale.library` itself can't open. |
| `catalog.language()` | `str` | First preferred language from `Locale->loc_PrefLanguages[0]`. Returns `"english"` if no preference is set or `locale.library` is unavailable. |
| `catalog.Catalog` | type | Re-exported so `isinstance(c, catalog.Catalog)` works. |

`Catalog` methods:

| Op | Notes |
|----|-------|
| `cat.lookup(id, default)` | `GetCatalogStr(cat, id, default)`. Returns the catalog string or the `default` if `id` is absent. Defaults to AmigaOS contract: never raises on a missing entry. A closed catalog also returns the default (the NULL is forwarded straight into `GetCatalogStr`). |
| `cat.close()` / `__del__` | `CloseCatalog`. Idempotent. |
| `with catalog.open(...) as cat:` | `__enter__` / `__exit__` close the catalog on block exit. |

`built_in_language=` kwarg note: AmigaOS `OpenCatalog` short-circuits
when the requested `language` matches the catalog's built-in
language — there's nothing to load, so it returns NULL. To force a
translation lookup even when asking for the "same" language as the
binary's built-in strings (e.g. when probing English-only catalogs
on an English Workbench), pass a different code via
`built_in_language`. The plan-doc walk-through has an example.

### Out of scope

- Writing catalogs (`flexcat`-style compilation). That's a build-
  time tool, not a runtime API.
- Conversion of locale-specific date / number / currency formats
  — separate phase if needed; would wrap `FormatString` etc.
- Multi-catalog merging / fallback chains — `OpenCatalog` already
  picks the right language automatically.
- Exposing `Locale` directly. `language()` covers the one field
  callers actually want; the rest of `struct Locale` is a
  read-only system snapshot that would invite lifetime confusion.

### Dependencies

- `locale.library` v38+ (OS 2.1). Lazy open; no explicit close.

### Files

```
ports/amiga/modcatalog.c                — C module + Catalog type
ports/amiga/modules/amiga.py            — `import _catalog as catalog`
tests/ports/amiga/test_catalog_smoke.py — vamos arg-shape smoke
docs/phase36-catalog-plan.md            — step plan
```

Variants: all three. ~1 KB text per variant.

---

## Phase 37 — `amiga.datatypes` (planned, low priority)

Wrap `datatypes.library` (V39+) for universal file recognition.
`datatypes.library` is AmigaOS's plug-in registry of file-format
handlers (`.datatype` modules in `SYS:Classes/DataTypes/`); any
running system knows about every datatype that's been installed
on it.

```python
from amiga import datatypes

# "What is this file?" — works for everything the system has a
# datatype for: PNG/JPEG/GIF/IFF ILBM, 8SVX/AIFF/WAV, ASCII/IFF FTXT,
# anim, etc.
info = datatypes.recognize("Work:Photos/holiday.jpg")
# {'group': 'picture', 'type': 'jpeg', 'name': 'JPEG (JFIF)'}

# Slightly more: full DTA_* attribute dump.
attrs = datatypes.info("Work:Photos/holiday.jpg")
# {'BaseName': 'JPEG', 'GroupID': 'pict', 'Width': 1280, 'Height': 720, ...}
```

### Scope

- `amiga.datatypes.recognize(path)` → dict with `group`, `type`,
  `name`. Uses `ObtainDataTypeA(DTST_FILE, lock, NULL)` then
  `GetDTAttrs(DTA_BaseName, DTA_GroupID, DTA_Name)`, releases via
  `ReleaseDataType`.
- `amiga.datatypes.info(path)` → dict. Same as `recognize` but
  also queries common attrs (`DTA_NominalHoriz` / `_NominalVert` for
  pictures, `DTA_Duration` / `_SampleLength` for sound, etc.) so
  callers can do quick metadata reads without `NewDTObject`.
- `amiga.datatypes.groups()` → list of installed group IDs (one of
  `"sound"` / `"picture"` / `"text"` / `"anim"` / `"system"` / `"document"`).
  Iterates the `dtl_*` chain in `DataTypesList`.

### Out of scope

- `NewDTObjectA` / `DoDTMethodA(DTM_DRAW)` rendering — requires a
  `RastPort` and a target `BitMap`, plus colour-map handling for
  CLUT formats. That's a much larger surface (graphics.library
  BitMap wrappers + intuition Screen/Window plumbing) and the
  payoff for a MicroPython workload is narrow.
- Save-as-other-format (`DTM_WRITE`) — same render-pipeline
  problem in reverse.
- Audio playback (`DTM_TRIGGER` on sound datatypes) — needs AHI on
  modern hosts.
- BOOPSI object exposure — `NewDTObject` returns an
  `Object *` that callers normally drive via `SetDTAttrs` /
  `GetDTAttrs`/`DoDTMethod`. Surfacing that as a Python type is a
  whole separate phase (one BOOPSI superclass per Python class).

### Dependencies

- `datatypes.library` v39+. Lazy open. The library is V39+ (OS 3.0)
  baseline — earlier Kickstarts simply skip the import (we already
  require V37 for things like `lib_call`, so V39 is a small bump
  but worth a clean `OSError(MP_ENOSYS)` if missing).

### Files

```
ports/amiga/moddatatypes.c              — C module
ports/amiga/modules/amiga.py            — adds `import _datatypes as datatypes`
tests/ports/amiga/test_datatypes_smoke.py     — vamos arg-shape smoke
docs/phase37-datatypes-plan.md          — step plan (TBD)
```

Variants: all three. ~2 KB text per variant.

### Why "low priority"

`recognize` / `info` is genuinely useful but rarely on the critical
path for a MicroPython script — file-type sniffing is more often
done by reading the first few bytes (`b"\x89PNG"`, `b"\xff\xd8"`).
The big win is universal sniffing across formats the system has
been taught about (third-party PNG / JPEG / WebP `.datatype`s), and
that lives or dies by which datatypes are installed on the target
host. Land Phase 35 / 36 first; pick up Phase 37 if a real caller
shows up.

---

## Other known limitations

| Issue | Status | Fix |
|-------|--------|-----|
| `try/except` in `@micropython.native` crashes | Known bug | Needs a 68k assembly NLR (`nlr68k.S`) saving D2–D7/A2–A5 in `nlr_buf_t` |
| `@micropython.viper` limited to 1 register local (D7) | `MAX_REGS_FOR_LOCAL_VARS = 1` | 68k-specific viper register allocator, or accept stack-based locals |
| `amiga.assigns()` resolved-target buffer is 256 bytes (`modamiga.c` line 265), tight for deeply-nested assigns on long-name filesystems | Surfaced during Phase 31 buffer audit | Bump to 1024 if a real-world case hits it; assign targets are usually short. |
| REPL history file path is 256 bytes (`amiga_history.c` `AMIGA_HISTORY_PATH_MAX`), tight for unusual install locations | Surfaced during Phase 31 buffer audit | Bump to 1024; history file lives in `ENVARC:` by convention so unlikely to hit. |

---

## Testing

Three paths, each for a different stage:

- **vamos** (host, headless) — day-to-day iteration; widest automated
  coverage via `tests/run-tests.py` driven through
  `tools/amiga-vamos-run.sh`. Current snapshot under the `standard`
  variant: ~722 pass / ~256 self-skip / ~75 fail out of 1053 files, of
  which only 4 are real platform differences (m68k struct alignment,
  long-double precision); the rest are Phase-12 native/viper gaps,
  Unix-port-specific cmdline tests, or vamos emulation gaps.
- **Amiberry** (full emulation) — anything needing Kickstart: Workbench
  launch, `icon.library`, 68881 FPU (the `68020fpu` variant), persistent
  REPL history, real AmiSSL. On-device runner `tools/amiga-runtests.py`
  walks a test dir and diffs against `.exp` files pre-generated on the
  host via `tools/amiga-gen-exp.py`.
- **CI** — `.github/workflows/ports_amiga.yml` (Phase 11). Cross-compile
  gate; produces release-style artefacts. No test execution.

Full runbook — install paths, variant selection, exclude lists, suite
snapshot, known-good failures, soft-float library bugs, on-device runner
setup — lives in [docs/amiga-testing.md](amiga-testing.md).
