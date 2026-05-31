# Phase 33 — `platform.amiga_info()` step plan

Companion to the Phase 33 design block in
[docs/amiga.md](amiga.md#phase-33--platformamiga_info-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable PRs.

Phase 33 ships six tiny C accessors on `amiga.*` and a frozen
`platform.py` that wraps them with the CPython-shaped API:

```python
>>> import platform
>>> platform.system()
'AmigaOS'
>>> platform.machine()
'68020'
>>> platform.amiga_info()
'CPU: 68020 | FPU: 68881 | Chipset: AGA | Kickstart: 45.57 | Chip: 1856KB | Fast: 14336KB'
```

Modelled on OoZe1911's port for surface compatibility — anything
written against their `platform` should run unchanged on ours.

## Phasing overview

```
Step 1: amiga.* CPU/FPU/chipset/kickstart/mem accessors
                                          ↓
                              Step 2: frozen platform.py + docs/smoke
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | Six new C entries in `modamiga.c`: `amiga.cpu()`, `amiga.fpu()`, `amiga.chipset()`, `amiga.kickstart()`, `amiga.chipmem()`, `amiga.fastmem()`. Each is a one-shot probe into `SysBase->AttnFlags`, `GfxBase->ChipRevBits0`, or `AvailMem`. | New entries in `amiga_module_globals_table`. | From the REPL: `amiga.cpu()`, `amiga.fpu()`, etc. return the expected strings/ints under Amiberry. |
| **2** | Frozen `platform.py` (under `ports/amiga/modules/`) exposing CPython's `system()` / `machine()` / `processor()` / `version()` / `release()` / `python_implementation()` / `python_version()` / `platform()` / `node()` plus the `amiga_info()` convenience. Docs flip + vamos arg-shape test. | New module; `docs/amiga.md` Phase 33 → ✅, `docs/amiga-testing.md` gains a short platform subsection. | `import platform; platform.amiga_info()` returns the expected formatted string. |

Each step is small (~80 LOC C, ~50 LOC Python frozen).

---

## Step 1 — Six `amiga.*` accessors

### Deliverables

- `amiga.cpu()` → str. Reads `SysBase->AttnFlags`, picks the
  highest CPU bit set (`AFF_68060` > `AFF_68040` > `AFF_68030` >
  `AFF_68020` > `AFF_68010` > else `"68000"`).
- `amiga.fpu()` → str. Same flags: `AFF_FPU40` ("68040"),
  `AFF_68882` ("68882"), `AFF_68881` ("68881"), else `"none"`.
- `amiga.chipset()` → str. Reads `GfxBase->ChipRevBits0` (opens
  `graphics.library` v36 lazily; cached). `"AGA"` if `GFXG_AGA`
  set, `"ECS"` if `GFXG_ECS_DENISE` or `GFXG_ECS_AGNUS`,
  `"OCS"` otherwise.
- `amiga.kickstart()` → str. Formats
  `SysBase->LibNode.lib_Version` and `lib_Revision` as
  `"VV.RR"`. (Tuple form is already available via the existing
  `amiga.os_version()`; this is the string convenience.)
- `amiga.chipmem()` → int. `AvailMem(MEMF_CHIP)` — *currently free*
  bytes, not total. Matches OoZe1911's semantics for surface
  compatibility.
- `amiga.fastmem()` → int. `AvailMem(MEMF_FAST)` — same caveat.

### `graphics.library` lifecycle

`amiga.chipset()` is the only one that needs a new library open.
Lazy global pattern matches Phase 30 / 32:

```c
static struct GfxBase *GfxBaseAccessor = NULL;

static void amiga_gfx_ensure_open(void) {
    if (GfxBaseAccessor == NULL) {
        GfxBaseAccessor = (struct GfxBase *)OpenLibrary(
            (CONST_STRPTR)"graphics.library", 36);
        if (GfxBaseAccessor == NULL) {
            mp_raise_OSError(MP_ENOENT);
        }
    }
}
```

Closed in a new `amiga_gfx_close()` wired into `main.c`'s
shutdown path (matches `amiga_asl_close` / `amiga_intuition_close`).

### Verification

REPL on target:

```python
>>> import amiga
>>> amiga.cpu(), amiga.fpu(), amiga.chipset(), amiga.kickstart()
('68020', '68881', 'AGA', '45.57')
>>> amiga.chipmem(), amiga.fastmem()
(1900544, 14680064)
```

Vamos check: vamos's emulated CPU/FPU bits surface via
`SysBase->AttnFlags`, so these all return *something*; the test
just confirms the shape.

---

## Step 2 — Frozen `platform.py` + docs/smoke

### Deliverables

- `ports/amiga/modules/platform.py` — frozen module exposing:
  - `system()` → `"AmigaOS"`
  - `machine()` → `amiga.cpu()`
  - `processor()` → `amiga.cpu()`
  - `version()` → `"Kickstart " + amiga.kickstart()`
  - `release()` → `amiga.kickstart()`
  - `python_implementation()` → `"MicroPython"`
  - `python_version()` → `sys.implementation.version` formatted
  - `platform()` → `"AmigaOS-<kickstart>-<cpu>-MicroPython_<pyver>"`
  - `node()` → `"Amiga"` (no NodeName concept on AmigaOS)
  - `amiga_info()` → the formatted single-line dump
- `tests/amiga/test_platform_smoke.py` — vamos-runnable:
  - `platform.system()` returns `"AmigaOS"`
  - `platform.machine()` is a str
  - `platform.amiga_info()` returns a str containing all six fields
- `docs/amiga.md` Phase 33 → ✅, "Status — done" block with the
  full accessor / module-function matrix.
- `docs/amiga-testing.md` short platform subsection.

### Variant gating

All three shipped variants include the accessors + the frozen
module — there's nothing per-variant to gate. Per-variant
text-segment growth: ~600 bytes.

---

## Cross-cutting concerns

- **"Currently free" vs "total" memory.** OoZe1911's port reports
  `AvailMem(MEMF_*)` which is current free, not installed total.
  Matches user mental model ("how much do I have right now") and
  matches their port exactly so surface compatibility is preserved.
  Document the semantics.
- **CPU/FPU detection via AttnFlags.** Reflects what AmigaOS
  detected at boot, which can differ from compile-time CPU
  targeting (a `standard` binary built for `-m68020 -msoft-float`
  running on a 68040 reports `"68040"` / `"68040"`).
- **Latin-1 strings.** Hard-coded chipset names are ASCII; CPU
  / FPU strings are ASCII. No codec concerns.
- **No caching.** Strings are cheap to recompute; memory values
  shift continuously so caching would be wrong. `chipset` opens
  `graphics.library` lazily (cached for the process lifetime),
  but the chipset bits themselves are re-read every call.

---

## Out-of-scope items reaffirmed

- Full CPython `platform` parity (`uname()`, `linux_distribution`,
  `mac_ver`, etc.) — not meaningful on AmigaOS.
- Reporting *installed* memory totals (separate from currently
  free) — would need walking the memory list manually.
- Per-CPU feature flags (MMU, cache state) — not user-facing
  enough to be worth the surface.
- Reporting graphics modes / monitor / RTG board details — out
  of scope; that's an Intuition / `graphics.library` deeper dive
  best left to a future phase.
