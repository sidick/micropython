# Phase 31 — ASL file requester step plan

Companion to the Phase 31 design block in
[docs/amiga.md](amiga.md#phase-31--asl-file-requester-planned).
That section answers *what* and *why*; this file is the *step-by-step
ship plan* — how to chunk the work into landable PRs.

Phase 31 lands a small C module that wraps `asl.library`'s
`AslRequest(ASL_FileRequest, ...)` — the native AmigaOS file
chooser dialog, modal and aware of every mounted volume / assign:

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

The single-pick form returns the full path as a `str`; multi-select
returns a `list[str]`; cancel returns `None`. Drawer + file are
joined via `AddPart()` so the volume-separator (`:`) lands correctly
no matter what the user typed.

The module is shared across all three variants — `asl.library`
ships with AmigaOS 2.0+ (v36), no external SDK, and the size cost
is small (~2 KB text).

## Phasing overview

```
Step 1: single-pick file_request → Step 2: save / drawers_only / multi
                                                       ↓
                                             Step 3: docs + smoke
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | `modasl.c` opens `asl.library` lazily, exposes `file_request(title, initial_drawer, initial_file, pattern)` — single-pick, no flags. Latin-1 codec on the strings; `AddPart()` to join drawer + file into the full path. Returns `str` or `None`. | New `_asl` C module wired into the build, registered with `MP_REGISTER_MODULE(MP_QSTR__asl, ...)`. | A no-arg `file_request()` under Amiberry — confirms the dialog renders, the OK path returns a path, Cancel returns None. |
| **2** | Add `save=False` (ASLFR_DoSaveMode), `drawers_only=False` (ASLFR_DrawersOnly), `multi=False` (ASLFR_DoMultiSelect). multi changes the return type to `list[str]`; iterate `fr_ArgList` joining each entry against `fr_Drawer` with `AddPart`. | Same C file extended; `amiga.py` adds `import _asl as asl`. | Save dialog (`save=True` + `initial_file="foo.txt"`) and a `multi=True` pick under Amiberry. |
| **3** | Docs flip, manifest verification, arg-shape tests. | `docs/amiga.md` Phase 31 → ✅, `docs/amiga-testing.md` gains a short ASL subsection. | vamos-runnable arg-shape / module-alias test; Amiberry visual confirmation noted as interactive. |

Each step is small (~80 LOC C + a one-line Python alias). Step 3
is paperwork.

---

## Step 1 — single-pick `file_request`

### Deliverables

- `ports/amiga/modasl.c` — new C source.
- Lazy `AslBase = OpenLibrary("asl.library", 36)` cached in a
  static; opened on first call. Same lifecycle pattern as Phase 30:
  no explicit close (the library is system-wide; AmigaOS reaps it
  on process exit). Wire `amiga_asl_close()` into main.c shutdown
  for symmetry — cheap.
- `ports/amiga/Makefile` adds `modasl.c` to `SRC_C` and `SRC_QSTR`.
- The C entry:
  ```c
  static mp_obj_t mod_asl_file_request(
      size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args);
  ```
  Defined as `MP_DEFINE_CONST_FUN_OBJ_KW` with an `mp_arg_t` table
  so kwargs work cleanly and step 2 can extend without breaking
  callers.
  - Tags built into a `struct TagItem[]` on the stack (max ~12
    slots covering every Step 1/2 kwarg + TAG_DONE).
  - Call `AllocAslRequestTags(ASL_FileRequest, tags)` → returns
    a `struct FileRequester *` or NULL.
  - Call `AslRequest(req, NULL)` → returns `BOOL` (TRUE if user
    picked, FALSE if cancelled).
  - On TRUE: walk `req->fr_Drawer` + `req->fr_File`, join via
    `AddPart(buf, drawer, sizeof buf)` then
    `AddPart(buf, file, sizeof buf)`. Buffer is **1024 bytes** —
    larger than the 512 used by `amiga.match` / WBStartup-arg
    handling, because long-name filesystems (SFS / PFS3 / FFS2)
    allow ~105-byte filenames per component, so a deeply-nested
    path can plausibly exceed 512. 1024 covers the worst case
    with comfortable headroom and the stack cost is negligible.
  - Return `mp_obj_new_str(buf, strlen(buf))`.
  - On FALSE: return `mp_const_none`.
  - Always `FreeAslRequest(req)` in a clean-up path.

### Kwargs supported in Step 1

| Kwarg | Tag | Type | Default |
|---|---|---|---|
| `title` | `ASLFR_TitleText` | str | `""` (no title) |
| `initial_drawer` | `ASLFR_InitialDrawer` | str | `""` |
| `initial_file` | `ASLFR_InitialFile` | str | `""` |
| `pattern` | `ASLFR_InitialPattern` + `ASLFR_DoPatterns=TRUE` | str | `""` (no pattern, no filter) |

`pattern` implies `ASLFR_DoPatterns=TRUE`. Empty `pattern` leaves
the filter off entirely.

### Why `AllocAslRequestTags` rather than the legacy varargs form

`AllocAslRequest(type, ...)` walks varargs from `va_list`; same
caller-frame issue we hit in Phase 30 with `EasyRequest`. The
`Tags` variant takes an explicit `TagItem` array, which is what
we'd construct anyway.

### Verification

Visual check under Amiberry from the interactive REPL:

```python
>>> from amiga import asl
>>> asl.file_request(title="Pick a script", initial_drawer="Sys:")
'Sys:Prefs/Workbench'   # if you picked Sys:Prefs/Workbench
>>> asl.file_request()
None                     # if you clicked Cancel
>>> asl.file_request(pattern="#?.py")
'Work:scripts/foo.py'    # only .py files visible in the list
```

### Error handling

| Failure | Behaviour |
|---|---|
| `asl.library` won't open | `OSError(MP_ENOENT)` from `file_request` first call |
| `AllocAslRequestTags` returns NULL | `OSError(MP_ENOMEM)` |
| Non-string kwargs | `mp_obj_str_get_data` raises `TypeError` automatically |
| Path components contain `\0` | Truncated at the embedded NUL (AmigaOS C-string limit). Document; don't pre-validate. |
| User Cancels | `None` (not an error — explicit "no choice" outcome) |

---

## Step 2 — `save`, `drawers_only`, `multi`

### Deliverables

Same `modasl.c`, extended kwargs:

| Kwarg | Tag | Effect |
|---|---|---|
| `save=False` | `ASLFR_DoSaveMode=TRUE` | Save dialog: editable filename field instead of file picker. |
| `drawers_only=False` | `ASLFR_DrawersOnly=TRUE` | List/pick drawers; the "Files" pane is hidden. Result is just the drawer path. |
| `multi=False` | `ASLFR_DoMultiSelect=TRUE` | User can shift-click multiple files. Result type changes to `list[str]`. |

`multi=True` post-processing:
- After `AslRequest` returns TRUE, walk `req->fr_NumArgs` /
  `req->fr_ArgList` (a `struct WBArg[]`).
- For each `WBArg`, ignore `wa_Lock` (always 0 from ASL) and use
  `wa_Name` as the file component, joined against
  `req->fr_Drawer` via `AddPart` into a fresh 512-byte buffer.
- Append each full path to a Python list.
- Return the list.

`multi=False` + `drawers_only=True` returns just `req->fr_Drawer`
(no file component to join in).

Validation:
- `multi=True` + `save=True` → `ValueError`: nonsensical. ASL
  itself would silently ignore one or the other, but we'd rather
  the user know.
- `drawers_only=True` + `pattern="..."` → ignore `pattern`
  silently (matches ASL's own behaviour).

### `amiga.py` alias

```python
import _asl as asl  # noqa: F401  (re-exported as amiga.asl)
```

So `from amiga import asl` and `amiga.asl.file_request(...)` both
work, matching the `_amiga` → `amiga`, `_intuition` → `amiga.intuition`
convention from Phases 17 and 30.

### Verification

```python
>>> asl.file_request(title="Save as", save=True, initial_file="out.txt")
'Ram Disk:out.txt'

>>> asl.file_request(multi=True, title="Pick scripts", pattern="#?.py")
['Work:scripts/foo.py', 'Work:scripts/bar.py']

>>> asl.file_request(drawers_only=True, title="Pick a folder")
'Work:scripts'
```

---

## Step 3 — Docs + tests

### Deliverables

- `docs/amiga.md` Phase 31 status → ✅; the section gains a "Status
  — done" block summarising the kwargs/return-type matrix.
- `docs/amiga-testing.md` gains an ASL subsection under the
  Amiberry runner: interactive-only verification with REPL snippets
  for save / multi / drawers_only.
- `tests/ports/amiga/test_asl_smoke.py` — argument-validation +
  module-alias chain test, runnable under vamos. Covers:
  - `_asl is amiga.asl`
  - `multi=True` + `save=True` raises `ValueError`
  - Non-string kwargs raise `TypeError`
  - On vamos the `OpenLibrary("asl.library", 36)` may succeed
    against vamos's stub or raise `OSError(ENOENT)` — accept either.

### Variant gating

All three shipped variants include the module — `asl.library` is
part of AmigaOS 2.0+ and there's no external SDK to fetch. Size
cost: ~2 KB text.

---

## Cross-cutting concerns

- **Modal blocking.** `AslRequest` is fully modal — the call doesn't
  return until the user clicks OK or Cancel. Matches the design
  intent. ASL has an async hook mechanism (`ASLFR_IntuiMsgFunc`)
  for receiving IDCMP messages mid-request; out of scope.
- **Stack-hungry — runs on a 32 KB scratch stack via `StackSwap`.**
  The default AmigaShell stack (often 4 KB) is too small for ASL's
  directory-listing / font-loading code — the dialog *renders*
  fine, but the post-pick code path trips a CHK exception
  (`0x80000006`) inside ASL. Doing the swap inside `file_request`
  means callers don't need to remember `Stack 32768` at the shell.
  Only the bare `AllocAslRequest` / `AslRequest` calls run on the
  scratch stack; path-building and `mp_obj_new_str` happen back on
  the original stack so MicroPython's GC stack-scan range
  (`gc_stack_top` in main.c) stays correct.
- **Screen independence.** Same as Phase 30: ASL renders on the
  default public screen, opening its own if none is available.
  No Workbench dependency.
- **Path joining via `AddPart`.** AmigaOS volumes use `:` as the
  separator (`Work:scripts/foo.py`), subdirs use `/`. Naive
  concatenation `drawer + "/" + file` breaks on drawer values
  like `"Work:"` (would yield `Work:/foo.py`). `AddPart` from
  `dos.library` handles every case correctly.
- **Latin-1 codec for strings.** Same rationale as Phase 30
  (AmigaOS Topaz is CP1252-ish). Filenames with non-ASCII bytes
  pass through as-is.
- **Pre-allocated 1024-byte path buffers.** Long-name filesystems
  (SFS, PFS3, FFS2) allow ~105-byte filenames per component, so a
  deeply-nested path on a modern volume can plausibly approach
  500+ bytes. 1024 covers that with headroom; stack cost is
  negligible. `AddPart` truncates safely on overflow if the
  caller does somehow exceed it, and we treat that overflow as
  the caller's problem (no pre-validation).
- **Memory lifecycle.** `AllocAslRequestTags` returns a request
  struct that must be `FreeAslRequest`'d. Tag and string buffers
  are caller-owned stack memory; ASL copies what it needs during
  the call. Once `AslRequest` returns, only the `fr_Drawer` /
  `fr_File` / `fr_ArgList` fields of the request struct hold
  live data (those are inside the request itself), and they all
  go away on `FreeAslRequest`.

---

## Out-of-scope items reaffirmed

- Font / screen / draw-mode / palette requesters — `asl.library`
  supports them but the file requester is the one with practical
  use from a scripted REPL. Adding `font_request()` etc. is a
  follow-on phase if needed.
- Custom hooks / per-entry filter callbacks
  (`ASLFR_FilterFunc` / `ASLFR_AcceptPattern`) — `pattern` covers
  the 95% case; callbacks would need a Hook trampoline.
- Modal-but-with-progress / async surfaces — needs `BuildAslRequest`
  + IDCMP wiring.
- Returning extra metadata (file size, date) — ASL doesn't expose
  these; would require a separate `dos.library Examine` pass
  per file, which the caller can do already.
