# MicroPython AmigaOS Port — Testing Guide

Three paths to test the port, each suited to a different stage of work:

| Method | When |
|--------|------|
| **vamos** (host-side, headless) | Day-to-day iteration; widest automated test coverage |
| **Amiberry** (full emulation) | Real-AmigaOS behaviour — Workbench launch, `icon.library`, 68881 FPU, persistent history, anything `serial.device`-shaped |
| **CI** (cross-compile only) | Confirming a branch builds in the canonical container; produces release-style artefacts |

For day-to-day work, vamos. For anything that needs Kickstart, an emulator. CI gives you the same binary you'd ship.

---

## 1. Vamos (recommended for iteration)

`vamos` is the userspace 68k/AmigaOS emulator from the `amitools` package.
It boots in milliseconds, has no GUI, and integrates with the standard
`tests/run-tests.py` harness. Setup here assumes vamos is installed at
`~/vamos/` and activated via `pipenv`.

### Direct invocation

```sh
cd ~/vamos
pipenv run vamos --cpu 68020 \
    -V "mp:/path/to/micropython/tests" \
    --cwd mp:basics \
    -- /path/to/build/micropython string1.py
```

- `--cpu 68020` is **required** — vamos defaults to 68000, which faults
  on `m68020` instructions.
- `-V name:/host/path` mounts a host directory as an AmigaOS volume.
- `--cwd` sets cwd; with basename invocation, `sys.argv[0]` / `__file__`
  match host CPython exactly.
- `--` separates vamos options from the binary's args.

### Driving `run-tests.py` via `tools/amiga-vamos-run.sh`

`tools/amiga-vamos-run.sh` is a `MICROPY_MICROPYTHON` wrapper. It mounts
`tests/` as the internal `mp:` volume, points vamos's cwd at the test's
directory, replaces the script argument with its basename, uses a private
`--vols-base-dir` so parallel workers don't collide on the auto `RAM:`
volume, and `-q` so vamos logs don't pollute captured stdout.

```sh
export MICROPY_MICROPYTHON="$(pwd)/tools/amiga-vamos-run.sh"
cd tests
./run-tests.py -d basics float io micropython misc
```

Exclude directories that can't run on Amiga (no socket-server harness
under vamos, no asyncio in the port, no inline asm for non-68k targets,
etc.):

```sh
./run-tests.py -d basics float io micropython misc \
    -e "inlineasm|machine_|thread|extmod/ussl|extmod/uasync"
```

### Variant selection

`AMIGA_VARIANT=minimal|standard|68040` picks build + matching `--cpu`
flag. Default is `standard`.

```sh
AMIGA_VARIANT=68040 ./run-tests.py basics/string1.py
```

`68020fpu` cannot run under vamos — it builds 68881 instructions and
vamos has no 68881/68882 emulation. The wrapper detects this and exits
with an explanation; test that variant under Amiberry or real hardware.

### Test-runner integration requirements

For `run-tests.py` to work against the Amiga binary, the port must:

- Accept `-X <option>` flags as no-ops (`run-tests.py` always emits
  `-X emit=bytecode`; macOS adds `-X realtime`).
- Return POSIX-style exit codes
  (`MICROPY_PYEXEC_ENABLE_EXIT_CODE_HANDLING (1)`).
- Free `AllocVec`'d argv buffers before returning — vamos's
  orphan-memory check makes the process exit non-zero otherwise.

All three are already in place.

### Known vamos quirks

- **Bebbo argv parser is broken** under vamos for multi-arg invocations;
  `amiga_parse_args` parses `pr_Arguments` itself.
- **`WaitForChar` returns 0 immediately** under vamos — the port uses
  plain `FGetC` for console reads.
- **`SetMode(fh, 1)` is a no-op** under vamos (it already delivers one
  char at a time).
- **No 68881 emulation** — see variant selection above.

### REPL under vamos: `tools/amiga-vamos-repl.sh`

Vamos doesn't translate `SetMode(stdin, 1)` to `tcsetattr` on the host
TTY, so an interactive REPL launched naked sees the host shell's cooked
mode (cursor keys echoed as `^[[D`, bytes only delivered on newline).
`tools/amiga-vamos-repl.sh` flips the host TTY into raw / no-echo /
no-isig before launching vamos and restores the original mode on exit.

```sh
tools/amiga-vamos-repl.sh                       # standard variant
AMIGA_VARIANT=68040 tools/amiga-vamos-repl.sh
```

Pipe input is unaffected; only interactive sessions need the wrapper.

### Suite snapshot — 2026-05-12

Run via `tools/amiga-vamos-run.sh` against the `standard` variant:

| Directory | Files | Pass | Self-skip | Fail | Notes |
|-----------|------:|-----:|----------:|-----:|-------|
| `basics/`     | 574 | 490 | 83  | 1  | `struct1.py` (bebbo ABI alignment) |
| `float/`      | 68  | 54  | 11  | 3  | EXACT-mode precision at double-range edges |
| `io/`         | 16  | 12  | 3   | 1  | `argv.py` (vamos host-path rewriting) |
| `import/`     | 30  | 29  | 0   | 1  | `import_file.py` (vamos host-path rewriting) |
| `micropython/`| 108 | 43  | 18  | 47 | All `native_*` / `viper_*` — Phase 12 |
| `extmod/`     | 205 | 67  | 131 | 7  | vamos socket / select / time-quantum / vfs_userfs gaps |
| `misc/`       | 14  | 6   | 8   | 0  | Skips: settrace, sys_exc_info, cexample |
| `cmdline/`    | 25  | 9   | 2   | 14 | Unix-port-specific (REPL banner, `-v`, terminal editing) |
| `stress/`     | 13  | 12  | 0   | 1  | `bytecode_limit.py` parser memory pressure |

Aggregate: **722 pass / 256 self-skip / 75 fail** out of 1053 files.
Excluding Phase-12 native/viper failures, Unix-port-specific cmdline
tests, vamos emulation gaps, and the `bytecode_limit.py` parser edge
case, **4 individual tests fail** — all real platform differences.

### Known failures that are not port bugs

| Test | Cause |
|------|-------|
| `basics/struct1.py` | `struct.calcsize("97sI") == 102`, test expects 104. bebbo gcc on m68k uses 2-byte `int` alignment per the AmigaOS m68k ABI; CPython on x86 uses 4. Both platform-correct. |
| `float/float_parse*.py` | Very long mantissa with very negative exponent; `1e+300` vs `9.999…e+299` differing by 2 ULP; `1e4294967301` not detected as overflow — 1–2 ULP off. Bebbo's 80-bit long-double soft-float loses just enough precision that EXACT-mode parsing can't always nail the closest double. |
| `float/float_format_accuracy.py` | repr round-trip rate ~72 % vs ≥ 99.7 % expected. Same long-double precision tax. |

### Bebbo soft-float library bugs

bebbo gcc 6.5b on `-msoft-float` ships incorrect floating-point helpers
in libgcc / clib2 / libnix. `ports/amiga/floatconv.c` overrides each one
(some directly, some via `--wrap` because clib2 fat-packs them with
`__muldf3` and friends). Keep this list in mind when triaging arithmetic
oddities:

| Routine | Bug | Trigger |
|---------|-----|---------|
| `__floatunsidf`, `__floatundidf`, `__floatdidf` | High-bit-set values convert to garbage | `float("9"*51 + "e-39")`, `array.array('Q', [...])`, any `mp_obj_new_int_from_uint(>2³¹)` → double |
| `__eqdf2`, `__nedf2`, `__ledf2`, `__gedf2`, `__ltdf2`, `__gtdf2` | NaN treated as ordered/equal | `==` / `!=` / `<=` / `>=` with NaN; `math.isclose`, set/dict NaN keys, `x != x` |
| `pow(-1, NaN)` (libnix) | Returns `1.0`, CPython expects NaN | `(-1) ** float('nan')` |
| `tgamma(-inf)` (libnix) | Returns `+inf`, CPython raises | `math.gamma(-inf)` |
| `__fixdfsi` (clib2) | Calls `IEEEDPFix` which under vamos aborts the whole emulator on NaN | `hash(float('nan'))` (gcc 6.5 emits `__fixdfsi` for `mp_float_hash`'s bit-level body after opt) |

---

## 2. Amiberry (full-system emulation)

Use Amiberry when behaviour needs Kickstart — Workbench launch, the icon
/ datatypes / intuition libraries, 68881 FPU, persistent REPL history in
`S:`, anything where AmigaDOS shell quirks matter, anything where vamos
"close enough" isn't.

The current Amiberry test loop relies on a startup hook: `S:user-startup`
runs `py0:boot` if present (where `py0:` is the repo root mounted as an
Amiberry hard drive), and `boot` redirects its own output to
`py0:boot.log` that the host can read directly. This works for one-shot
scripted runs but is custom tooling.

A serial-pipe replacement — so `tests/run-tests.py --target=pyboard`
could drive the emulated Amiga the same way it drives an rp2 or esp32 —
was scoped on 2026-05-30 and is currently blocked on the Amiberry side.
With `serial_port=TCP://...`, **Amiga → host is byte-perfect 8-bit
clean** (`Open("SER:")` + `Write` flows verbatim, all 256 byte values
plus CR/LF/CSI survive), but **host → Amiga delivers zero bytes** to
the emulated `serial.device`'s RX path. Confirmed with two independent
probes:

1. A MicroPython probe using `amiga.library("dos.library").Read` /
   `WaitForChar` on `SER:`.
2. A pure-C probe (bebbo clib2, no MicroPython, no `bsdsocket`, no
   signal hooks) — same `Open` / `Write` / `WaitForChar` / `Read`
   sequence, byte-for-byte identical failure capture.

Both probes wrote a READY marker (host saw it), then `WaitForChar(5s)`
× 6 returned 0 every time while the host bridge confirmed
`s.sendall(...)` of test pulses over the established TCP connection.
Reported to the Amiberry author; if/when `readser` is fixed, the
design sketch — port-side `-X serial` flag plus an `execpty:` launcher
that bridges Amiberry's TCP to a host PTY — is straightforward to
implement.

> **Tip for reproducing the probe.** Use
> `serial_port=TCP://127.0.0.1:1234/wait` rather than the bare form.
> With `/wait`, Amiberry blocks at startup until a TCP client connects,
> eliminating the race where the bridge connects after the Amiga has
> already written its first marker and Amiberry has dropped the bytes
> for lack of a peer.

### On-device test runner: `tools/amiga-runtests.py`

The on-device runner walks a single test directory, runs each `.py` test
via `amiga.execute("micropython … >T:…")`, and compares the captured
output against the matching `.py.exp` file. No CPython to diff against
on the Amiga side, so it relies entirely on the `.exp` files.

#### Step 1 — generate the `.exp` reference set on the host

Upstream only ships `.exp` files where MicroPython output is *expected*
to differ from CPython. Generate the rest first:

```sh
tools/amiga-gen-exp.py tests/basics tests/float tests/io \
    tests/import tests/micropython tests/misc tests/cmdline tests/stress
```

Existing `.exp` files are never overwritten — many of them use the
`########` wildcard or regex matching that the test framework depends on.
Delete a specific `.exp` and re-run if you need to regenerate one.

`.exp` files are not checked in. Add `tests/**/*.py.exp` to
`.git/info/exclude` if the chatter in `git status` bothers you.

#### Step 2 — boot Amiberry with at least 32 KB AmigaDOS stack

The default 4–8 KB stack is too small for the deep compile-time
recursion a handful of tests trigger (e.g. `try_except_break.py`); a
stack overflow there manifests as `Software Failure 8000000B` (Line F
trap) rather than a clean Python `RuntimeError`.

```
1> Stack 32768
1> cd py0:
1> micropython tools/amiga-runtests.py tests/basics
1> micropython tools/amiga-runtests.py tests/float T:my-results/
```

The second argument is the result directory (default `T:mp-test-results/`).
On `FAIL`, `<dir>/<test>.py.out` (captured stdout) and `<dir>/<test>.py.exp`
(expected reference) land there. On pass / skip, stale artefacts from a
previous run are deleted.

`T:` lives in RAM under AmigaOS — a clean reboot wipes the artefacts.

### What it skips

The runner skips test classes the port can't pass for structural
reasons, matching the equivalent feature-check skips in `run-tests.py`:

- `int_big*` / `*_intbig.py` — no arbitrary-precision ints
  (`MICROPY_LONGINT_IMPL_LONGLONG`).
- `*_endian.py` — big-endian platform, most tests assume little-endian.
- `native_*` / `viper_*` — Phase 12 (`@micropython.native` call support
  is the next emitter rework).
- `cmd_*` / `repl_*` — need `# cmdline:` directives, regex `.exp`
  matching, and PTY interaction; only `tests/run-tests.py` can drive
  those.
- A small frozenset of one-off tests that hit disabled extmod features
  (`weakref_*`, `fun_code`, `attrtuple2.py`, etc.).

### Result format and CRLF

The Amiga port emits `\r\n` on stdout even when redirected to a file —
this is `mp_hal_stdout_tx_strn_cooked` doing the AmigaShell-compatible
thing. The on-device runner strips trailing `\r` line by line before
comparing against the LF `.exp` reference, so the `.exp` files generated
by `amiga-gen-exp.py` stay platform-neutral. Any host-side diff against
captured output needs the same normalisation:

```sh
diff <(tr -d '\r' < captured.out) reference.exp
```

### Variants that need Amiberry, not vamos

- **`68020fpu`** — needs 68881; vamos has no 68881/68882 emulation.
- **Workbench launch** — `WBStartup`, `icon.library`, tooltype handling.
  Vamos has no `icon.library`.
- **AmiSSL** (Phase 28) — needs Kickstart and a real
  `amisslmaster.library` install (see SSL section below).

### SSL tests (Phase 28)

TLS support is on in `standard`, `68020fpu`, `68040`; off in
`minimal` (no `bsdsocket.library` → no SSL). Exercising `ssl` on
target needs:

- **AmiSSL v5 installed.** `amisslmaster.library` must be reachable
  via `LIBS:`. The conventional install puts it at
  `SYS:AmiSSL/Libs/amisslmaster.library` with the CA bundle in a
  c_rehash dir at `SYS:AmiSSL/certs/`. AmiSSL's own installer adds
  `Assign AmiSSL: SYS:AmiSSL` and
  `Assign LIBS: AmiSSL:Libs ADD` to `S:user-startup`.
- **AmiSSL assigns in scope at micropython launch.** Watch the
  ordering in `user-startup`: if the boot hook runs `py0:boot`
  *before* the AmiSSL assigns, the boot script must add them
  itself or `import ssl` will raise on `SSLContext()`.

Recommended verify path:

```python
import ssl
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.verify_mode = ssl.CERT_REQUIRED
ctx.set_default_verify_paths()   # preferred over load_verify_locations
```

`set_default_verify_paths()` sidesteps two AmigaOS-specific
gotchas with manual cert loading:

1. **Trailing-slash capath fails.** `load_verify_locations(capath=
   "AmiSSL:certs/")` concatenates internally to a double-slash that
   AmigaDOS interprets as parent-dir reference, and lookups silently
   miss. Pass `"AmiSSL:certs"` (no trailing slash) if you want to
   use it explicitly.
2. **AmiSSL c_rehash uses the old (pre-OpenSSL-1.0.0) subject hash
   algorithm.** New-hash filenames silently miss in lookups even
   though byte-identical files exist under their old-hash names
   in the same directory.

#### Known limitation: TLS 1.3 against modern CDNs

`CERT_REQUIRED` HTTPS against TLS-1.3-eager fronts (Cloudflare,
GitHub) completes the handshake (cert chain verifies fine) but the
subsequent `ws.write` returns `EPIPE` / broken pipe with zero
bytes sent. Forcing TLS 1.2 doesn't help — those servers reject
TLS 1.2. Hosts that still negotiate TLS 1.2 by default
(`www.python.org`, etc.) work cleanly in both directions.

Tracked in `docs/phase28-ssl-plan.md` as a Phase 28 follow-up
(likely AmiSSL's post-handshake state isn't fully ready for
`SSL_write` when the server pushes a NewSessionTicket
immediately after `Finished`).

#### Upstream extmod/ssl* and tls* tests

`tools/amiga-runtests.py` does not gate these out, so they run
when the on-device runner walks `extmod/`. A handful require
features we don't yet expose (`ssl.wrap_socket` module-level
legacy form, `cadata=` parameter, server-mode handshake against a
canned cert pair) and will fail; the `SSLContext`-shape tests
(`ssl_sslcontext.py`, `ssl_sslcontext_verify_mode.py` etc.) should
pass.

---

## 3. CI (cross-compile only)

`.github/workflows/ports_amiga.yml` runs on `workflow_dispatch` with a
`ref` input (branch / tag / commit, default `amiga-port`). One job
inside `stefanreinauer/amiga-gcc:latest` builds `mpy-cross` once, then
all four variants sequentially. Each binary uploads as a separate
artifact named `micropython-amiga-<variant>-<ref>-<sha>`.

CI does not run the test suite — it's a cross-compile gate plus an
artefact producer. Functional testing happens locally against vamos /
Amiberry.

### Local mirror: `tools/amiga-build.sh`

Runs the same image with the same commands, so local and CI binaries are
bit-identical:

```sh
tools/amiga-build.sh                   # all four variants
tools/amiga-build.sh standard          # one
tools/amiga-build.sh standard 68040    # several
tools/amiga-build.sh clean             # clean all build dirs
```

Files are written as the host user (via `--user`), not root.

---

## Quick reference

```sh
# Full vamos sweep against the standard variant
cd ports/amiga && make
export MICROPY_MICROPYTHON="$(pwd)/../../tools/amiga-vamos-run.sh"
cd ../../tests
./run-tests.py -d basics float io import micropython misc \
    -e "inlineasm|machine_|thread|extmod/ussl|extmod/uasync"

# Interactive REPL under vamos
tools/amiga-vamos-repl.sh

# Generate .exp files for on-device runner
tools/amiga-gen-exp.py tests/basics tests/float tests/io tests/import \
    tests/micropython tests/misc tests/cmdline tests/stress

# Then boot Amiberry, `Stack 32768`, `cd py0:`, and
# `micropython tools/amiga-runtests.py tests/basics`

# CI-identical build all variants
tools/amiga-build.sh
```
