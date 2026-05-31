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

Most examples gracefully handle the "feature missing on this host"
case (no ARexx daemon, no graphics.library under vamos, etc.) by
catching `OSError` and printing a short note.
