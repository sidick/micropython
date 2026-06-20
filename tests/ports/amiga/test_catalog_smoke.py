# Phase 36 wiring smoke -- exercises the `_catalog` module and the
# `amiga.catalog` facade alias. Runs under vamos and on real hardware.
# Vamos has no real locale.library that can find catalog files, so
# the open() round-trip block soft-passes there; Amiberry covers it
# end-to-end against Sys/monitors.catalog.

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

# ---------- Error paths ----------
try:
    _catalog.open("nonexistent_phase36.catalog")
except OSError:
    pass
else:
    raise AssertionError("expected OSError for missing catalog")

try:
    _catalog.open()
except TypeError:
    pass
else:
    raise AssertionError("expected TypeError on missing name")

try:
    _catalog.open("foo.catalog", language=42)
except (TypeError, OSError):
    pass
else:
    raise AssertionError("expected TypeError/OSError for non-str language")

try:
    _catalog.open("foo.catalog", built_in_language=42)
except (TypeError, OSError):
    pass
else:
    raise AssertionError("expected TypeError/OSError for non-str built_in_language")

# ---------- Try opening a known system catalog ----------
# Sys/monitors.catalog ships with most Workbench installs. Passing
# built_in_language="german" forces locale.library to actually load
# the English file rather than short-circuit on the matching
# built-in language.
try:
    cat = _catalog.open(
        "Sys/monitors.catalog",
        language="english",
        built_in_language="german",
    )
except OSError:
    print("Sys/monitors.catalog unavailable -- skipping round trip")
    print("OK")
    raise SystemExit

# lookup with a definitely-absent id -> returns the default string.
got = cat.lookup(99999, "FALLBACK")
assert got == "FALLBACK", got
assert isinstance(got, str)

# lookup return type is str.
assert isinstance(cat.lookup(1, "default_1"), str)

# isinstance via re-exported type.
assert isinstance(cat, _catalog.Catalog)

# Close + double-close.
cat.close()
cat.close()

# Closed catalog: lookup forwards NULL into GetCatalogStr which
# returns the default. Not a ValueError -- matches AmigaOS contract.
got = cat.lookup(1, "after_close")
assert got == "after_close", got

# Context manager.
with _catalog.open(
    "Sys/monitors.catalog",
    language="english",
    built_in_language="german",
) as cat2:
    s = cat2.lookup(99999, "WITH_FALLBACK")
    assert s == "WITH_FALLBACK"

print("OK")
