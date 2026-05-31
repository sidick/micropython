# Phase 30 — Intuition requester dialogs step plan

Companion to the Phase 30 design block in
[docs/amiga.md](amiga.md#phase-30--intuition-requester-dialogs-planned).
That section answers *what* and *why*; this file is the *step-by-step
ship plan* — how to chunk the work into landable PRs.

Phase 30 lands a small C module that wraps `intuition.library`'s
`EasyRequestArgs()` family. Three Python entry points:

```python
from amiga import intuition

idx = intuition.easy_request("Title",
                             "Are you sure?\nThis can't be undone.",
                             ["Yes", "No", "Cancel"])
ok  = intuition.auto_request("Replace existing file?", yes="Yes", no="No")
intuition.message("Done.", button="OK")
```

`easy_request` returns the **0-based leftmost** index of the
clicked button, so the call above gives `0` for *Yes*, `1` for
*No*, `2` for *Cancel*. (`auto_request` and `message` translate
that back into `bool` / `None` for the common cases.)

The module is shared across all four variants — `intuition.library`
ships with every AmigaOS install, there's no external SDK, and the
size cost is small (~1.5 KB text).

## Phasing overview

```
Step 1: C module skeleton + easy_request → Step 2: auto_request + message
                                                          ↓
                                                Step 3: docs + smoke
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | `modintuition.c` opens `intuition.library` lazily, exposes `easy_request(title, body, buttons)`. Latin-1 codec on the three strings; printf-safety via `es_TextFormat = "%s"` + body as the single arg. Returns 0-based leftmost int. | New `_intuition` C module wired into the build, registered with `MP_REGISTER_MODULE(MP_QSTR__intuition, ...)`. | A 3-button `easy_request` under Amiberry — click each button, confirm right return value. |
| **2** | `auto_request(body, yes="Yes", no="No")` and `message(body, button="OK")` wrappers (both still in C — they're tiny and avoid the Python round-trip). `auto_request` returns `bool`; `message` returns `None`. | Same C file extended; `amiga.py` adds `import _intuition as intuition` so `from amiga import intuition` and `amiga.intuition.message(...)` both work. | A 2-button `auto_request` and a 1-button `message` under Amiberry. |
| **3** | Docs flip, manifest verification, no-arg & arg-shape tests. | `docs/amiga.md` Phase 30 → ✅, `docs/amiga-testing.md` gains a short Intuition subsection ("interactive — needs visual check under Amiberry"). | Tests against argument validation (empty buttons list, non-string, etc.) — pure-Python, runnable under vamos. |

Each step is small (50–100 LOC of C plus a Python alias). Step 3 is
purely paperwork.

---

## Step 1 — `easy_request`

### Deliverables

- `ports/amiga/modintuition.c` — new C source.
- Lazy `IntuitionBase = OpenLibrary("intuition.library", 36)` cached
  in a static; opened on first call. No close on exit needed (the
  library is system-wide; AmigaOS reaps it when the process dies).
  Optionally wire `amiga_intuition_close()` into `main.c`'s shutdown
  for symmetry — cheap.
- `ports/amiga/Makefile` adds `modintuition.c` to `SRC_C` and to
  `SRC_QSTR` (the new function names are qstrs).
- The C entry:
  ```c
  static mp_obj_t mod_intuition_easy_request(
      mp_obj_t title_in, mp_obj_t body_in, mp_obj_t buttons_in);
  ```
  - Decode all three strings as Latin-1 (use
    `mp_obj_str_get_data` for the raw bytes; on AmigaOS the
    console + Topaz font is CP1252-ish, so Latin-1 is the safe
    middle ground). bytes-typed args pass through untouched.
  - `buttons_in` is iterable; collect labels into a single
    `AllocVec`'d buffer joined by `|`. Empty list → `TypeError`.
  - Populate a stack `struct EasyStruct`:
    ```c
    struct EasyStruct es = {
        sizeof(struct EasyStruct),
        0,
        title_buf,          // es_Title
        (CONST_STRPTR)"%s", // printf-safe body
        gadget_buf,         // joined labels
    };
    ```
  - Call `EasyRequestArgs(NULL, &es, NULL, &body_arg)` where
    `body_arg` is `(STRPTR)body_buf` — a single-element args
    block matching the `%s`.
  - Translate AmigaOS rightmost-is-0 to 0-based-leftmost:
    `(r == 0) ? (N - 1) : (r - 1)` where N is the button count.
  - Free the joined-labels buffer; return the int.

### Why `EasyRequestArgs` rather than the varargs `EasyRequest`

`EasyRequest`'s varargs pull `va_arg` from `va_list`, which is
caller-frame-dependent on m68k cdecl. We'd need to build the
arg block by hand anyway. `EasyRequestArgs` documents the args
pointer explicitly, which is what we already have to do.

### Verification

Visual check under Amiberry from the interactive REPL:

```python
>>> from amiga import intuition
>>> intuition.easy_request("Title", "Pick one.", ["Apple", "Banana", "Cancel"])
```

→ requester pops on the Workbench screen.

- Click *Apple* → `0`
- Click *Banana* → `1`
- Click *Cancel* (or close-window equivalent) → `2`

Repeat with a single-button list `["OK"]` — returns 0 either way
since rightmost == leftmost when N=1.

### Error handling

| Failure | Behaviour |
|---|---|
| `intuition.library` won't open (impossible on stock AmigaOS, but...) | `OSError(MP_ENOENT)` from `easy_request` first call |
| Empty `buttons` list | `TypeError("at least one button required")` |
| Non-string / non-bytes in `buttons` / `title` / `body` | `mp_obj_str_get_data` raises `TypeError` automatically |
| Label or title contains `\0` | Truncated at the embedded NUL (AmigaOS C-string limit). Document; don't pre-validate. |
| Label contains `\n` | Pass through. `EasyRequest` lays gadgets out by `|`, no `\n` handling. |
| Body contains `\n` | Renders as a line break (intentional — Intuition splits the body on `\n`). |
| Body contains `%` | Safe — `es_TextFormat = "%s"` makes the body a literal arg, not a format string. |

---

## Step 2 — `auto_request` + `message`

### Deliverables

- Both live in the same `modintuition.c` as Step 1 — implemented
  as thin wrappers around the same `easy_request_impl` static
  function so the title/format/gadget-join logic is shared.
- `auto_request(body, yes="Yes", no="No")`:
  - Title defaults to empty string (just the body shown).
  - Labels: `[yes, no]`. `EasyRequest` returns 1 if Yes clicked,
    0 if No clicked → our translation gives `0` / `1`.
  - Return `True` if the Yes button was picked, `False` otherwise.
- `message(body, button="OK")`:
  - Title empty.
  - One label.
  - Return `None`. Don't bother propagating the return code.
- `ports/amiga/modules/amiga.py` gains:
  ```python
  import _intuition as intuition  # exposes amiga.intuition.*
  ```
  Now `from amiga import intuition` and `amiga.intuition.message(...)`
  both work.

### Verification

```python
>>> intuition.auto_request("Replace existing file?")
True   # user clicked Yes
False  # user clicked No

>>> intuition.message("Job done.")
# requester pops, user clicks OK, returns None
```

### Surface coverage notes

- Three-arg shape used by `easy_request` doesn't translate cleanly
  into `MP_DEFINE_CONST_FUN_OBJ_3` if we want kwargs; use
  `MP_DEFINE_CONST_FUN_OBJ_KW` with a `mp_arg_t` table so
  `easy_request(title, body, buttons)` positional + future kwargs
  (e.g. `idcmp=...`) work without breaking the API.

---

## Step 3 — Docs + tests

### Deliverables

- `docs/amiga.md` Phase 30 status → ✅; the section gains a short
  "Status — done" block listing the three functions and noting
  the AmigaOS rightmost-is-0 → 0-based-leftmost translation.
- `docs/amiga-testing.md` gains an Intuition subsection under
  Amiberry runner: "Interactive only — needs visual click-through
  to verify return values."
- A tiny `tests/amiga/test_intuition_argshape.py` that exercises
  the argument validation without actually opening a requester
  (mock out `_intuition.easy_request` and check the wrappers
  pass the right labels). Runs under vamos.

### Variant gating

All four variants ship the module — `intuition.library` is part
of every AmigaOS install and there's no external SDK to fetch.
Size cost: ~1.5 KB text.

---

## Cross-cutting concerns

- **Modal blocking.** `EasyRequestArgs` is fully blocking — the
  Python call doesn't return until the user clicks a button. This
  matches the design intent ("modal only"). If a future caller
  wants non-blocking, that needs `BuildEasyRequestArgs` +
  `SysReqHandler` — separate phase.
- **Screen independence.** `intuition.library` renders on the
  default public screen if one is up, and opens its own if not —
  so the requester appears regardless of whether Workbench is
  loaded. `OSError(EIO)` only fires if `EasyRequestArgs` itself
  returns `-1` (genuine intuition-side failure, very rare).
- **Latin-1 codec choice.** AmigaOS's stock Topaz font and the
  console driver are largely CP1252-compatible for the printable
  range; treating Python strings as Latin-1 covers
  `é/ñ/ü/£/€-ish` characters that scripts are likely to want.
  UTF-8 input bytes go through a `.decode("utf-8")` →
  `.encode("latin-1", "replace")` pipeline so non-Latin-1
  codepoints render as `?` rather than corrupt bytes.
- **`\n` in body.** Intuition's `EasyRequest` body honours `\n` as
  a line break — useful for multi-line prompts. Document. Title
  and gadget labels don't honour `\n` (`\n` in a label gets
  truncated at the line break by AmigaOS rendering).
- **Title length.** Long titles get clipped by the window's title
  bar. Don't pre-validate length; let AmigaOS handle.

---

## Out-of-scope items reaffirmed

- Arbitrary windows / widgets / event loops — `easy_request` is
  the only blocking-modal Intuition surface we need.
- Custom gadgets, font requesters, screen pickers, palette
  requesters — separate APIs, separate phases.
- File requester — that's `asl.library`, Phase 31.
- Non-blocking / async requesters — needs `BuildEasyRequest` +
  `SysReqHandler`, deferred.
- Hooking the IDCMP signal to get richer button events — the
  return value from `EasyRequestArgs` is enough for our use case.
