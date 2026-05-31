# Phase 36 wiring smoke -- exercises the `_catalog` module and the
# `amiga.catalog` facade alias. Runs under vamos and on real hardware.
# Vamos has no real locale.library, so the open() round trip is best
# done on Amiberry; vamos verifies surface only.

import _catalog
import amiga

# ---------- Module registration + alias chain ----------
assert _catalog is amiga.catalog, "amiga.catalog should alias _catalog"
assert callable(_catalog.open)
assert callable(_catalog.language)
assert isinstance(_catalog.Catalog, type)

# ---------- language() ----------
# Always returns a non-empty str. Falls back to "english" if
# locale.library can't open or has no preferred language set.
lang = _catalog.language()
assert isinstance(lang, str), lang
assert len(lang) > 0, "language() should not return empty string"

# ---------- Error path: missing catalog ----------
try:
    _catalog.open("nonexistent_phase36.catalog")
except OSError:
    pass
else:
    raise AssertionError("expected OSError for missing catalog")

# ---------- Error path: open requires a name ----------
try:
    _catalog.open()
except TypeError:
    pass
else:
    raise AssertionError("expected TypeError on missing name")

# ---------- Error path: language kwarg must be str or None ----------
try:
    _catalog.open("foo.catalog", language=42)
except (TypeError, OSError):
    pass
else:
    raise AssertionError("expected TypeError/OSError for non-str language")

# ---------- Try opening a known system catalog ----------
# When this succeeds (Amiberry / real hw with localised Workbench),
# round-trip the surface. When it fails (vamos / English Workbench
# without translations), the OSError is the expected smoke result.
try:
    cat = _catalog.open("workbench.catalog", version=0)
except OSError:
    print("workbench.catalog unavailable -- skipping round trip")
    print("OK")
    raise SystemExit

# lookup with a definitely-absent id -> returns the default string.
got = cat.lookup(99999, "FALLBACK")
assert got == "FALLBACK", got
assert isinstance(got, str)

cat.close()
# Double-close is idempotent.
cat.close()

# With-statement support.
with _catalog.open("workbench.catalog") as cat2:
    s = cat2.lookup(99999, "WITH_FALLBACK")
    assert s == "WITH_FALLBACK"

print("OK")
