MicroPython port to AmigaOS 3.x
===============================

This is a port of MicroPython to AmigaOS 3.x running on Motorola 68k
processors (68020 and later). It produces a regular AmigaDOS executable
that runs from the Shell or by double-clicking from the Workbench. The
port targets Kickstart 3.0+ (V37+) and is built with the bebbo
m68k-amigaos GCC toolchain.

Supported features include:

- REPL with line editing, history (persisted across runs via `ENVARC:`),
  Ctrl+C interrupt handling, and Workbench / Shell launch.
- 68k native code emitter (`@micropython.native`, `@micropython.viper`).
- Frozen modules baked into the binary plus on-disk imports via the
  AmigaDOS Volume:dir/file path convention.
- Dynamic heap growth via `AllocVec` / `FreeVec` against `MEMF_ANY`.
- Pythonic file I/O over AmigaOS `dos.library`, with the standard
  `os` surface (`chdir`, `getcwd`, `listdir`, `mkdir`, `remove`,
  `rename`, `rmdir`, `stat`, `statvfs`) plus port-specific
  `os.chmod` / `os.getprotect` and the `FIBF_*` protection-bit
  constants.
- AmigaOS-aware `os.path` (`join`, `split`, `splitext`, `basename`,
  `dirname`, `isabs`, `abspath`, `normpath`, `exists`, `isfile`,
  `isdir`) honouring `Volume:dir/file` semantics.
- The `amiga` module surfacing direct AmigaOS API access:
  - `amiga.Library` proxy with a `.fd`-driven trampoline that turns
    a library handle into Python-callable methods.
  - `amiga.intuition` — modal requesters (`easy_request`,
    `auto_request`, `message`) wrapping `intuition.library`'s
    `EasyRequestArgs`.
  - `amiga.asl` — file requester wrapping `asl.library`.
  - `amiga.icon` — `.info` file read / write / manipulation via
    `icon.library` (`DiskObject` with tooltype dict).
  - `amiga.catalog` — `locale.library` catalog lookup and the
    system language preference.
  - ARexx integration: inbound via `amiga.rexx_open` / `serve`,
    outbound via `amiga.rexx` / `RexxClient`, plus
    `rexx_exists` / `rexx_list` for introspection.
  - Volume / assign introspection (`amiga.volumes`, `amiga.assigns`)
    and AmigaDOS pattern matching (`amiga.match`).
  - `timer.device`-backed `time.ticks_ms` / `sleep_ms`.
  - Environment-variable integration: `os.getenv` / `putenv` /
    `unsetenv` map to AmigaDOS `GetVar` / `SetVar`, sharing the
    `ENV:` store with the AmigaShell.
- `socket` networking via `bsdsocket.library` (AmiTCP, MiamiDx,
  Roadshow, etc.).
- TLS / SSL via AmiSSL v5 (variant-gated; bundled with the standard
  variant, omitted from the size-optimised variants).
- `urequests` frozen HTTP / HTTPS client.
- `platform` module with CPython-shaped identity (`system`,
  `machine`, `release`, `python_version`, `platform`) plus
  `platform.amiga_info()` for a one-line AmigaOS summary
  (CPU / FPU / chipset / Kickstart / chip / fast memory).

The port produces three build variants:

| Variant      | Target               | TLS  | Notes |
|--------------|----------------------|------|-------|
| `standard`   | 68020 + soft-float   | yes  | Default. Runs on any 68020+. |
| `68020fpu`   | 68020 + 68881 FPU    | yes  | Hardware float for accelerator boards with FPU. |
| `68040`      | 68040 + FPU          | yes  | Tuned for 68040/68060 systems. |

Building
--------

The MicroPython cross-compiler must be built first; it pre-compiles
the frozen modules to bytecode:

    $ make -C mpy-cross

This is run from the top-level repository directory. All other
commands below run from `ports/amiga/`.

The build requires the bebbo `m68k-amigaos-gcc` toolchain
([https://github.com/bebbo/amiga-gcc](https://github.com/bebbo/amiga-gcc)).
Follow bebbo's `make all` instructions to install into `/opt/amiga`,
which is the default `CROSS_COMPILE` prefix used by the port's
Makefile.

To build the default `standard` variant:

    $ make

The binary appears as `build-standard/micropython`. Other variants
are selected with `VARIANT=`:

    $ make VARIANT=68020fpu
    $ make VARIANT=68040

For a reproducible build that mirrors CI exactly, use the
`tools/amiga-build.sh` Docker wrapper (runs the same
`stefanreinauer/amiga-gcc:latest` image CI uses):

    $ tools/amiga-build.sh                   # all three variants
    $ tools/amiga-build.sh standard          # just one
    $ tools/amiga-build.sh standard 68040    # several
    $ tools/amiga-build.sh clean             # clean all build dirs

The TLS-enabled variants need the AmiSSL v5 SDK headers, fetched
automatically by `tools/amiga-build.sh`. For a manual build outside
the Docker wrapper, set `AMISSL_SDK` to point at an extracted v5
SDK (or pass `MICROPY_PY_AMIGA_SSL=0` to disable TLS).

Deploying
---------

Copy the built binary to any AmigaOS volume. The `C:` assign is the
conventional location for command-line tools so they're on the
Shell's path:

    1> copy mypy/build-standard/micropython c:micropython
    1> protect c:micropython rwed

`protect rwed` clears the default deny bits so the file is
readable, writable, executable, and deletable -- AmigaDOS
protection bits are inverted relative to Unix (set bit ⇒ denied).

For Workbench launch, ship a `micropython.info` icon alongside the
executable. Tooltypes drive launch behaviour:

- `SCRIPT=<path>` — Python script to execute on launch.
- `HEAP=<bytes>` — initial garbage-collected heap size.
- `MAXHEAP=<bytes>` — upper bound for dynamic heap growth.

Under emulation, mount the source tree (or a copy of the binary)
as a hard drive partition in your emulator's configuration. The
port is regularly tested under Amiberry on macOS and Linux with
a standard Workbench 3.1 install plus the Roadshow TCP stack
and AmiSSL.

Running
-------

From the AmigaShell:

    1> micropython
    MicroPython v1.27.0 on 2026-05-31; AmigaOS 3.x with m68k
    Use Ctrl-D to exit, Ctrl-E for paste mode
    >>> import platform
    >>> platform.amiga_info()
    'CPU: 68020 | FPU: 68881 | Chipset: AGA | Kickstart: 45.57 | Chip: 1856KB | Fast: 14336KB'

Pass a script as the first argument to run it directly:

    1> micropython hello.py
    Hello from AmigaOS!

`PROGDIR:` is added to `sys.path` automatically so a script can
`import` modules from the same directory as the executable.

For Workbench launch, set the `SCRIPT=` tooltype on the icon
to the path of the script to run; output and errors are sent to
a `CON:` console that opens automatically.

Testing
-------

Two paths are supported:

- **vamos** (host, headless) — fastest iteration, runs the standard
  MicroPython test runner against a host-mounted volume. Suitable
  for the core language and most port-specific smoke tests. See
  `tools/amiga-vamos-run.sh`.
- **Amiberry** (full Kickstart emulation) — covers anything that
  needs real `intuition.library` UI, real ARexx daemon, TLS
  against the AmiSSL v5 install, the persistent REPL history
  file, and the 68881 FPU codegen (vamos has no 68881 emulation).

Port-specific smoke tests live under `tests/ports/amiga/`. The
full runbook -- vamos exclude list, Amiberry on-device runner,
known-good failures, soft-float library bugs -- lives in
[`docs/amiga-testing.md`](../../docs/amiga-testing.md).

Limitations
-----------

- The 68k native code emitter doesn't yet support `try` / `except`
  blocks inside `@micropython.native` functions (the 68k NLR
  rework is tracked as a planned phase in `docs/amiga.md`).
- `@micropython.viper` is limited to a single register local.
- TLS / SSL requires the AmiSSL v5 install at runtime (AmigaShell
  `Assign AmiSSL: SYS:AmiSSL` is the standard placement).
- The 68000 (with software emulation of multiplication, etc.) is
  not a build target; the port assumes 68020 alignment-safe access
  and at least one MMU-friendly memory window.

Further reading
---------------

- [`docs/amiga.md`](../../docs/amiga.md) — implementation log,
  phase history, design notes for each subsystem.
- [`docs/amiga-testing.md`](../../docs/amiga-testing.md) — full
  testing runbook covering vamos, Amiberry, and CI.
