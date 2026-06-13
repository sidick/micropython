MicroPython AmigaOS examples
============================

Short, self-contained examples of the Amiga-specific modules
documented in `ports/amiga/README.md`.

Each script runs from the AmigaShell with the binary at
`ports/amiga/build-standard/micropython` (or wherever you installed
it on the target):

    1> micropython examples/amiga/hello.py

The Intuition / ASL / icon-write examples are best run on a real
Amiga or Amiberry; vamos has no UI and partial library coverage.
The Shell-only examples (volume listing, platform info, ARexx)
work fine on either.

| File | Demonstrates |
|------|--------------|
| `hello.py` | Bare REPL probe. Prints `sys.implementation` and `platform.amiga_info()`. |
| `platform_info.py` | Full `platform.*` surface plus the `amiga.cpu/fpu/chipset/...` accessors. |
| `intuition_requester.py` | Modal dialogs via `amiga.intuition.easy_request` / `message` / `auto_request`. |
| `asl_file_picker.py` | The `asl.library` file requester through `amiga.asl.file`. |
| `icon_round_trip.py` | Reads `Sys:Prefs`, edits tooltypes, writes back to `RAM:`. |
| `catalog_lookup.py` | `locale.library` catalog lookup plus the system language. |
| `rexx_server.py` | ARexx inbound port + `serve` dispatcher with three commands. |
| `rexx_client.py` | Outbound ARexx via `amiga.rexx` and the persistent `RexxClient`. |
| `library_displaybeep.py` | `amiga.Library` proxy + `.fd` trampoline (calls `DisplayBeep`). |
| `volume_listing.py` | `amiga.volumes`, `amiga.assigns`, `amiga.match` plus `os.walk`. |
| `gadtools_demo.py` | Live GadTools gadget gallery on the Workbench screen via `amiga.Library` + `amiga.taglist`. |

Most examples gracefully handle the "feature missing on this host"
case (no ARexx daemon, no graphics.library under vamos, etc.) by
catching `OSError` and printing a short note.

Adding your own `.fd` files for `amiga.Library`
-----------------------------------------------

The `library_displaybeep.py` example calls `intuition.library` and
`exec.library` through the `amiga.Library` proxy. Their function
signatures (LVO offsets and the D0–D7 / A0–A5 argument-register
mapping for each call) come from `.fd` files that ship with the
AmigaOS NDK and were pre-parsed into the frozen
`_amiga_fd.LIBRARIES` table at build time, so most stock libraries
Just Work out of the box.

For third-party libraries -- or for stock libraries built against a
newer NDK than the one bundled -- drop a `.fd` file in either
location at runtime and `amiga.Library` will pick it up:

    PROGDIR:fd/<name>_lib.fd
    PROGDIR:fd/<name>.fd
    LIBS:fd/<name>_lib.fd
    LIBS:fd/<name>.fd

Both `<name>_lib.fd` (the bebbo / Aminet convention) and bare
`<name>.fd` are accepted. `<name>` is the library name with its
`.library` / `.device` / `.resource` suffix dropped:

    intuition.library  →  intuition_lib.fd  or  intuition.fd
    serial.device      →  serial_lib.fd     or  serial.fd

`PROGDIR:` is AmigaDOS's auto-assign to the directory of the
running binary, so dropping `fd/` next to `micropython` is the
zero-config option. `LIBS:` is the system-wide library assign and
lets all `micropython` invocations share the same `.fd` set.

The frozen NDK table is consulted *first* -- a runtime file only
fires on a lookup miss. If a stock library's `.fd` is wrong (for
example a newer revision added arguments), give your file a
slightly different name and pass `signatures=` to the `Library`
constructor to bypass the frozen table entirely:

    from amiga import library_from_signatures, _parse_fd
    with open("PROGDIR:fd/myproprietary.fd") as f:
        sigs = _parse_fd(f.read())
    lib = library_from_signatures("myproprietary.library", 0, sigs)
    lib.MyFunction(...)

### `.fd` file format quick reference

`.fd` is a plain-text format (one function per line) used by every
AmigaOS toolchain. The parser in `amiga.py` accepts the common
subset:

    ##base _MyLibBase
    ##bias 30
    ##public
    OpenThing(name,flags)(a0,d0)
    CloseThing(handle)(a0)
    ##private
    GetInternalState()()                    ; not exposed to Python
    ##public
    Beep()()
    ##end

* `##bias 30` sets the LVO of the *first* listed function to -30
  (matching the AmigaOS library jump-vector convention; subsequent
  functions get -36, -42, ... at 6-byte strides).
* `##private` and `##public` toggle whether following functions
  are exposed; private functions still occupy LVO slots so the
  bias accounting stays correct.
* The function line is `Name(arg1,arg2,...)(reg1,reg2,...)`. Args
  go into the listed registers in order. The parser supports the
  D0–D7 / A0–A5 registers only; A6 (the library base) is wired up
  automatically by the trampoline. Functions targeting A7 (stack)
  or A6 (cia.resource's special base-register ABI) are skipped.

If your `.fd` file declares functions whose arg / register counts
disagree (e.g. mathieeedoub*_lib.fd's IEEE-double convention),
the affected lines are dropped silently -- the trampoline can't
encode those without per-function help.
