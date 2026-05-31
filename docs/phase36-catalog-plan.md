# Phase 36 — `amiga.catalog` step plan

Companion to the Phase 36 design block in
[docs/amiga.md](amiga.md#phase-36--amigacatalog-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable PRs.

Phase 36 wraps `locale.library`'s catalog lookup so localized apps
can read their translated strings, plus surfaces the system's
preferred language:

```python
from amiga import catalog

with catalog.open("MyApp.catalog", version=1) as cat:
    print(cat.lookup(1, "Default English string"))
    print(cat.lookup(2, "Cancel"))

print(catalog.language())  # 'english' / 'german' / 'français'
```

## Phasing overview

```
Step 1: _catalog C module + Catalog type + language() + vamos smoke
                        ↓
              Step 2: docs flip + closing tests
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | `ports/amiga/modcatalog.c` registering `_catalog`. `open(name, version=0, language=None)` → `Catalog`. `Catalog.lookup(id, default)` → str. `Catalog.close()` / `__enter__` / `__exit__` / `__del__`. Module-level `language()`. Wired through `Makefile` + frozen `amiga.py`. | New C module + facade entry. | Under vamos: import works, alias `amiga.catalog is _catalog`, missing catalog → OSError, `language()` returns a non-empty string when vamos exposes a Locale. Under Amiberry: `catalog.open("workbench.catalog")` round-trip with `lookup` against a known string id. |
| **2** | Docs flip + comprehensive tests. | `docs/amiga.md` Phase 36 → ✅; `docs/amiga-testing.md` gains a `catalog` subsection; `tests/ports/amiga/test_catalog_smoke.py` covers the surface. | Amiberry verification of `language()` + `lookup` against a system catalog. |

Step 1 is ~150 LOC C; Step 2 is paperwork.

---

## Step 1 — `_catalog` module + Catalog type + language() + smoke

### Deliverables

- `ports/amiga/modcatalog.c` (~150 LOC). Module registered as
  `_catalog` via `MP_REGISTER_MODULE(MP_QSTR__catalog, ...)`,
  matching the `_intuition` / `_asl` / `_icon` convention.
- Module globals:
  - `open(name, version=0, language=None)` → `Catalog`. Calls
    `OpenCatalogA(NULL, name, [OC_Version, OC_Language?, TAG_DONE])`.
    Raises `OSError(ENOENT)` on NULL return.
  - `language()` → `str`. `OpenLocale(NULL)` → first non-NULL
    `loc_PrefLanguages[0]`. Falls back to `"english"` if no
    preference, returns immediately if `locale.library` can't open.
  - `Catalog` type symbol re-exported on the module.
- `Catalog` MicroPython type:
  - Wraps `struct Catalog *cat`. NULL after `.close()`.
  - `.lookup(id, default)` — `GetCatalogStr(cat, id, default)`.
    Returns the catalog string if present, the default otherwise.
    Matches the AmigaOS contract (no exception on miss).
  - `.close()` — `CloseCatalog`. Idempotent.
  - `__del__` / `__exit__` forward to `.close()`.
  - `__enter__` returns self for `with` use.
- `ports/amiga/Makefile`:
  - `SRC_C += modcatalog.c`
  - `SRC_QSTR += modcatalog.c`
- `ports/amiga/modules/amiga.py`:
  - `import _catalog as catalog`

### Implementation notes

- `LocaleBase` is opened lazily on first call (`OpenLibrary
  ("locale.library", 38)`). No explicit close — `locale.library`
  is a system-wide library that AmigaOS reaps at process exit;
  this matches the intuition / icon / asl pattern.
- `OpenCatalogA` is the tag-list form; build a small stack array:
  ```c
  struct TagItem tags[4];
  int n = 0;
  tags[n].ti_Tag = OC_Version;  tags[n].ti_Data = version;     n++;
  if (language != NULL) {
      tags[n].ti_Tag = OC_Language; tags[n].ti_Data = (ULONG)language; n++;
  }
  tags[n].ti_Tag = TAG_DONE;
  ```
- `GetCatalogStr` accepts a NULL catalog and returns the default
  in that case; we don't need to guard.
- The default-string lifetime contract: `GetCatalogStr` returns
  either the catalog's own string (held by `locale.library`) or
  the caller's default pointer. The catalog-string case is safe
  to copy via `mp_obj_new_str` because the lifetime tracks the
  Catalog object the Python caller owns.

### Verification

Vamos smoke (`tests/ports/amiga/test_catalog_smoke.py`):

```python
import _catalog
import amiga

assert _catalog is amiga.catalog
assert callable(_catalog.open)
assert callable(_catalog.language)

# language() should always return a non-empty str.
lang = _catalog.language()
assert isinstance(lang, str) and len(lang) > 0

# A catalog that definitely doesn't exist -> OSError.
try:
    _catalog.open("nonexistent_phase36.catalog")
except OSError:
    pass
else:
    raise AssertionError("expected OSError")

print("OK")
```

Amiberry interactive verification:

```python
>>> from amiga import catalog
>>> catalog.language()
'english'
>>> with catalog.open("workbench.catalog", version=44) as cat:
...     print(cat.lookup(1, "fallback"))
...
<some workbench string>
```

---

## Step 2 — Docs flip + closing tests

### Deliverables

- `docs/amiga.md` Phase 36 status → ✅; section gains a full
  surface matrix (`open` / `lookup` / `close` / `language`).
- `docs/amiga-testing.md` gains a `catalog` subsection with
  Amiberry REPL examples covering `language()` and `lookup`.
- `tests/ports/amiga/test_catalog_smoke.py` expanded:
  - Module + alias + `Catalog` type re-export.
  - Missing-catalog OSError path.
  - `language()` returns str.
  - With-statement / `__enter__` / `__exit__` round trip.
  - Closed-object `.lookup` returns the default (`GetCatalogStr`
    forwards a NULL catalog to the caller's default; no exception
    — matches the AmigaOS contract).

Step 1 added a `built_in_language=` kwarg beyond the original
scope so on-target round-trip testing works on a stock English
Workbench. AmigaOS `OpenCatalog` refuses to open when the
requested `language` matches the catalog's built-in language
(nothing to load); the kwarg surfaces `OC_BuiltInLanguage` so
callers can force a translation file lookup anyway.

---

## Cross-cutting concerns

- **`locale.library` version.** v38 (OS 2.1) is the baseline.
  Earlier Kickstarts don't ship it; `open()` raises
  `OSError(EIO)` cleanly in that case.
- **String encoding.** Catalog strings are byte sequences in
  whatever code set the catalog declares. We surface them as
  Python `str` (latin-1 trip-safe), matching the rest of the
  port's string handling. A future change could expose
  `cat_CodeSet` if anyone needs strict UTF-8 conversion.
- **Vamos coverage.** vamos doesn't ship a real
  `locale.library`; the smoke test only confirms the surface
  doesn't crash, not actual catalog lookups. Amiberry covers
  the round trip.

---

## Out-of-scope items reaffirmed

- Writing catalogs (`flexcat`-style compilation). That's a
  build-time tool, not a runtime API.
- Conversion of locale-specific date / number / currency formats
  — separate phase if needed; would wrap `FormatString` etc.
- Multi-catalog merging / fallback chains — `OpenCatalog` already
  picks the right language automatically.
- Exposing `Locale` directly (it's the system Locale, not a
  per-application object; `language()` covers the one field
  callers actually want).
