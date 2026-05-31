# Phase 35 wiring smoke -- exercises the `_icon` module and the
# `amiga.icon` facade alias. Runs under vamos: there's no Workbench so
# the round-trip path needs a host-side .info file. Use Amiberry for
# end-to-end coverage.

import _icon
import amiga

# Module registration + alias chain.
assert _icon is amiga.icon, "amiga.icon should alias _icon"
assert callable(_icon.read)
assert callable(_icon.write)
assert callable(_icon.new)

# All do_Type values surfaced as small ints, matching workbench.h.
for name, expected in (
    ("WBDISK", 1), ("WBDRAWER", 2), ("WBTOOL", 3), ("WBPROJECT", 4),
    ("WBGARBAGE", 5), ("WBDEVICE", 6), ("WBKICK", 7), ("WBAPPICON", 8),
):
    value = getattr(_icon, name)
    assert isinstance(value, int), (name, value)
    assert value == expected, (name, value, expected)

# DiskObject type re-exported so isinstance() works.
assert isinstance(_icon.DiskObject, type)

# A path that definitely doesn't exist -> OSError, not a crash.
try:
    _icon.read("RAM:nonexistent_phase35_smoke")
except OSError:
    pass
else:
    raise AssertionError("expected OSError for missing path")

# _icon.write requires a DiskObject -- TypeError for anything else.
try:
    _icon.write("RAM:foo", "not a DiskObject")
except TypeError:
    pass
else:
    raise AssertionError("expected TypeError on non-DiskObject")

# _icon.new with an unsupported type code surfaces as OSError(EINVAL).
# GetDefDiskObject returns NULL for unknown types.
try:
    _icon.new(99)
except OSError:
    pass
else:
    raise AssertionError("expected OSError for unknown type code")

# _icon.new tooltypes kwarg validation: must be a dict, values must be
# str/bytes/None. These run before any AmigaOS call so they work on
# any host -- under vamos GetDefDiskObject may not have an icon file
# to read, in which case OSError is also acceptable.
try:
    _icon.new(_icon.WBPROJECT, tooltypes=[("FOO", "bar")])
except (TypeError, OSError):
    pass
else:
    raise AssertionError("expected TypeError/OSError on non-dict tooltypes")

print("OK")
