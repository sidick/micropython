# Phase 35 — `amiga.icon` step plan

Companion to the Phase 35 design block in
[docs/amiga.md](amiga.md#phase-35--amigaicon-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable PRs.

Phase 35 graduates `icon.library` access from the single
read-only `amiga.tooltype()` peek to a full `DiskObject` round-trip
surface, so Python can read, edit, create, and write `.info` files:

```python
from amiga import icon

dobj = icon.read("Work:Tools/Editor")
print(dobj.type, dobj.default_tool, dobj.stack_size)
dobj.stack_size = 16384
dobj.tooltypes["FONT"] = "topaz.font/8"
icon.write("Work:Tools/Editor", dobj)

new = icon.new(icon.WBPROJECT, default_tool="C:Ed",
               tooltypes={"WINDOW": "CON:0/0/640/256/Title"})
icon.write("Work:Notes", new)
```

## Phasing overview

```
Step 1: icon.read + DiskObject (read-only) + WB* constants
                        ↓
            Step 2: write + new + mutation
                        ↓
            Step 3: docs flip + smoke tests
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | `ports/amiga/modicon.c` registering `_icon`. `icon.read(path)` returns a `DiskObject` Python type. Read-only attrs `.type`, `.default_tool`, `.stack_size`, `.current_x`, `.current_y`, and a read-only `.tooltypes` mapping (FindToolType-backed). WB* constants on the module. Wired through `Makefile` + frozen `amiga.py`. | New C module + facade entry. | Under vamos: import works, alias `amiga.icon is _icon`, WB* constants are ints, missing path → OSError. Under Amiberry: `icon.read("PROGDIR:micropython")` returns a `DiskObject` whose `.type == "tool"` and whose tooltypes round-trip the SCRIPT= / HEAP= entries the runtime already consumes. |
| **2** | Mutation. Settable `.default_tool` / `.stack_size` / `.current_x` / `.current_y`; mutable `.tooltypes` mapping with `[k] = v`, `del [k]`, `in`, iteration. `icon.write(path, dobj)` via `PutDiskObject`. `icon.new(type, **kwargs)` via `GetDefDiskObject`. | Mutation surface complete. | Round-trip on `RAM:test`: create a fresh `WBPROJECT` icon, set `default_tool` + tooltypes, write, re-read, verify field equality. |
| **3** | Docs flip + comprehensive tests. | `docs/amiga.md` Phase 35 → ✅; `docs/amiga-testing.md` gains an `icon` subsection; `tests/ports/amiga/test_icon_smoke.py` covers the surface. | `make -C ports/amiga test` (or the vamos-based runner) is green. |

Each step is small: Step 1 is ~250 LOC C plus ~5 LOC Python, Step 2
adds another ~200 LOC C and ~30 LOC Python, Step 3 is paperwork.

---

## Step 1 — `icon.read` + DiskObject (read-only) + WB* constants

### Deliverables

- `ports/amiga/modicon.c` (~250 LOC). Module registered as `_icon`
  via `MP_REGISTER_MODULE(MP_QSTR__icon, ...)` (matching the
  `_intuition` / `_asl` convention).
- Module globals:
  - `read(path)` → `DiskObject`. Calls `GetDiskObject(path)`; raises
    `OSError(MP_ENOENT)` on NULL.
  - `WBDISK`, `WBDRAWER`, `WBTOOL`, `WBPROJECT`, `WBGARBAGE`,
    `WBDEVICE`, `WBKICK`, `WBAPPICON` — small-int constants from
    `<workbench/workbench.h>`.
  - `DiskObject` type symbol re-exported on the module (so `isinstance(d, icon.DiskObject)` works).
- `DiskObject` MicroPython type:
  - Holds `struct DiskObject *do` plus an `owned` flag (always True
    in Step 1; flipped off if the caller wants to wrap a non-owned
    `DiskObject *` in a future step).
  - Read-only attrs surfaced via `MP_TYPE_FLAG_NONE` + `attr` slot:
    - `.type` → str (`"disk"` / `"drawer"` / `"tool"` / `"project"`
      / `"garbage"` / `"device"` / `"kick"` / `"appicon"`; falls back
      to the raw int for unknown types).
    - `.default_tool` → str or None (None when `do_DefaultTool` is
      NULL or empty).
    - `.stack_size` → int (`do_StackSize`).
    - `.current_x`, `.current_y` → int (`do_CurrentX`, `do_CurrentY`).
    - `.tooltypes` → `DiskObjectTooltypes` mapping (read-only this
      step, mutable in Step 2).
  - Methods: `.close()` → `FreeDiskObject(do)` once. `__del__`
    forwards to `.close()`.
- `DiskObjectTooltypes` MicroPython type:
  - Wraps the parent DiskObject's `do_ToolTypes` pointer.
  - Step 1 surface: `__getitem__`, `__contains__`, `__iter__`, `__len__`,
    `keys()`, `values()`, `items()`, `get(k, default=None)`.
  - Step 2 adds `__setitem__` / `__delitem__`.
  - Returns/iterates **bytes** values (matching ARexx, where keeping
    AmigaOS encoding intact matters); keys are decoded as ASCII
    `str` since tooltype keys are conventionally upper-case ASCII.
  - Same encoding convention as `amiga.tooltype()`: a key with a `=`
    sign returns the bit after the `=`; a flag-style key (no `=`)
    returns the empty `b""`.
- `ports/amiga/Makefile`:
  - `SRC_C += modicon.c`
  - `SRC_QSTR += modicon.c`
- `ports/amiga/modules/amiga.py`:
  - `import _icon as icon  # re-exported as amiga.icon`

### Implementation notes

- Reuse the existing `amiga_icon_open()` helper from `main.c`
  rather than redefining `IconBase`. The header for that is already
  pulled into `modamiga.c`; declare `extern bool amiga_icon_open(void);`
  at the top of `modicon.c`.
- The `attr` slot for the DiskObject pattern follows `modssl.c`'s
  `ssl_context_attr`: dispatch on the requested attribute qstr,
  populate `dest[0]` for reads. Method/property `dest[1] != NULL`
  fall-through is left to the standard `mp_obj_generic_attr` path
  by calling `mp_obj_generic_load_attr_or_method`.
- Map `do_Type` to qstrs at conversion time via a small switch so
  the strings live in the qstr pool, not heap.
- Tooltype iteration: walk the NULL-terminated `STRPTR[]`. Each
  entry is conceptually `"KEY=VALUE"` or just `"KEY"`. For key
  extraction, split at the first `=`.

### Verification

Vamos smoke (`tests/ports/amiga/test_icon_smoke.py`):

```python
import _icon
import amiga

assert _icon is amiga.icon
for name in ("WBDISK", "WBDRAWER", "WBTOOL", "WBPROJECT",
             "WBGARBAGE", "WBDEVICE", "WBKICK", "WBAPPICON"):
    assert isinstance(getattr(_icon, name), int)

# A path that definitely doesn't exist → OSError, not crash.
try:
    _icon.read("RAM:does/not/exist")
except OSError:
    pass
else:
    raise AssertionError("expected OSError")

print("OK")
```

Amiberry interactive: `_icon.read("PROGDIR:micropython")` returns a
`DiskObject`. `dobj.type == "tool"`. `dobj.tooltypes["SCRIPT"]`
matches whatever the tooltype currently holds.

---

## Step 2 — write, new, and mutation

### Deliverables

- Mutable attributes on `DiskObject`:
  - `.default_tool = "C:Ed"` / `= None` — writes a copy through
    `AllocVec`, frees the old buffer (if Python-owned).
  - `.stack_size = 16384`
  - `.current_x = 16`, `.current_y = 24`
- `DiskObjectTooltypes` extended:
  - `__setitem__("FONT", "topaz.font/8")` — value may be `str` /
    `bytes` / `None` (None ⇒ flag-style, no `=`). Reallocates the
    underlying `STRPTR[]` if the key is new.
  - `__delitem__("FONT")` — shrinks the `STRPTR[]` array.
  - Frees freshly-allocated buffers on object teardown.
- `_icon.write(path, dobj)` → None. Calls `PutDiskObject(path, do)`;
  raises `OSError(MP_EIO)` on failure.
- `_icon.new(type, **kwargs)` → fresh `DiskObject`. Implementation:
  1. `GetDefDiskObject(type)` — supplies the default icon image
     from `ENV:sys/def_*.info`.
  2. Apply kwargs (`default_tool`, `stack_size`, `current_x`,
     `current_y`, `tooltypes` dict).
  3. Return as Python-owned (so `FreeDiskObject` runs on teardown).

### Implementation notes

- `do_ToolTypes` allocation policy: the original array from
  `GetDiskObject` is owned by `icon.library`; after the first
  mutation we copy it into an `AllocVec`-owned array we manage
  ourselves. Track ownership with a per-DiskObject flag so teardown
  knows whether to call `FreeVec` on `do_ToolTypes` and its strings.
- For `do_DefaultTool`: same pattern. The original is `icon.library`
  -owned; assignment switches to our own `AllocVec` buffer and
  flips an owns-default-tool flag.
- `PutDiskObject` cares about `do_Type` being valid and the image
  data being present; `GetDefDiskObject` gets us a sane image. For
  callers who want to build a DiskObject without an image, document
  that the result is unwritable; we won't synthesize a placeholder.

### Verification

```python
new = icon.new(icon.WBPROJECT, default_tool="C:Ed",
               tooltypes={"FOO": "bar", "FLAG": None})
icon.write("RAM:test", new)
back = icon.read("RAM:test")
assert back.default_tool == "C:Ed"
assert back.tooltypes["FOO"] == b"bar"
assert back.tooltypes["FLAG"] == b""
new.close(); back.close()
```

---

## Step 3 — Docs flip + tests

### Deliverables

- `docs/amiga.md` Phase 35 status → ✅; section gains the
  full surface matrix (which calls / which type / what they return).
- `docs/amiga-testing.md` short `icon` subsection covering
  `icon.read`, `icon.write`, `icon.new`, and the tooltype mapping.
- `tests/ports/amiga/test_icon_smoke.py` expanded:
  - Module + alias + constants.
  - Missing-path OSError.
  - Round-trip against `RAM:` (icon.library is available under
    vamos — it just operates on the `mp:` ↔ `RAM:` mapping).
  - Tooltype set/get/del round-trip on a fresh `new()` DiskObject.

---

## Cross-cutting concerns

- **`icon.library` version.** We use the same v33 (OS 2.0+)
  baseline as `main.c`'s `amiga_icon_open`. `GetDefDiskObject` is
  v36+ — if the host lacks it, `new()` should raise `OSError(MP_ENOSYS)`
  rather than crash. Step 2 adds the runtime guard.
- **Ownership flags.** Each `DiskObject` Python object tracks
  whether it owns `do_DefaultTool` and `do_ToolTypes`, because the
  default `GetDiskObject` path returns icon.library-owned memory
  that must not be `FreeVec`'d. Mutation transitions ownership to
  Python and the teardown path frees accordingly.
- **Workbench refresh.** Out of scope for Phase 35 — `PutDiskObject`
  writes the `.info` file; refreshing an open Workbench window is
  `workbench.library`'s `UpdateWorkbench` which we don't ship.
  Users who want the icon to reappear in an open window can close
  and reopen the drawer, or call out to ARexx with
  `WORKBENCH UPDATE`.

---

## Out-of-scope items reaffirmed

- Editing the icon's image data (`do_Gadget->GadgetRender` /
  `SelectRender`).
- App icons (`AddAppIcon`) — `workbench.library`, not
  `icon.library`.
- NewIcon / GlowIcon / OS3.5+ ColorIcon extended IFF chunks.
- Drag-and-drop / IDCMP integration.
