# locale.library catalog lookup via amiga.catalog.
#
# Catalogs are AmigaOS's localisation mechanism: each app ships
# translated strings keyed by integer IDs. catalog.language() reports
# the system's preferred language.

from amiga import catalog

print("system language:", catalog.language())

# Pass built_in_language="german" so locale.library is willing to
# load the English file even when the requested language matches the
# built-in default (the AmigaOS convention).
try:
    cat = catalog.open(
        "Sys/monitors.catalog",
        language="english",
        built_in_language="german",
    )
except OSError as e:
    print("no Sys/monitors.catalog:", e)
    raise SystemExit

print("opened Sys/monitors.catalog")

# Lookups return the catalog string or the supplied default when the
# id isn't present. The default is what your app would have hardcoded.
for string_id in (1, 2, 3, 99):
    got = cat.lookup(string_id, "<fallback for id=%d>" % string_id)
    print("  id=%-3d : %r" % (string_id, got))

cat.close()

# Same shape with a context manager.
with catalog.open(
    "Sys/monitors.catalog",
    language="english",
    built_in_language="german",
) as cat:
    print("via with-statement:", cat.lookup(1, "<fallback>"))
