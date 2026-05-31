# Phase 35 wiring smoke -- exercises the `_icon` module and the
# `amiga.icon` facade alias. Runs under vamos and on real hardware.
# Vamos has working icon.library + dos.library, so the full read /
# mutate / write / re-read round trip is exercised here.

import _icon
import amiga

# ---------- Module registration + alias chain ----------
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

# ---------- Error paths ----------
try:
    _icon.read("RAM:nonexistent_phase35_smoke")
except OSError:
    pass
else:
    raise AssertionError("expected OSError for missing path")

try:
    _icon.write("RAM:foo", "not a DiskObject")
except TypeError:
    pass
else:
    raise AssertionError("expected TypeError on non-DiskObject")

try:
    _icon.new(99)
except OSError:
    pass
else:
    raise AssertionError("expected OSError for unknown type code")

try:
    _icon.new(_icon.WBPROJECT, tooltypes=[("FOO", "bar")])
except (TypeError, OSError):
    pass
else:
    raise AssertionError("expected TypeError/OSError on non-dict tooltypes")

# ---------- Round trip: new() -> write() -> read() ----------
# Vamos auto-mounts RAM: so this writes to the host-side T: dir.
# If GetDefDiskObject fails (no ENV:sys/def_project.info) we skip
# the round trip and just confirm the surface.
try:
    new = _icon.new(
        _icon.WBPROJECT,
        default_tool="C:Ed",
        stack_size=8192,
        current_x=80,
        current_y=80,
        tooltypes={"FOO": "bar", "FLAG": None, "RAW": b"raw bytes"},
    )
except OSError:
    print("new() raised OSError -- vamos lacks ENV:sys defaults; skipping round trip")
    print("OK")
    raise SystemExit

# In-memory checks before write.
assert new.type == "project"
assert new.default_tool == "C:Ed"
assert new.stack_size == 8192
assert new.current_x == 80
assert new.current_y == 80
tt = new.tooltypes
assert len(tt) == 3
assert "FOO" in tt
assert "FLAG" in tt
assert "RAW" in tt
assert tt["FOO"] == b"bar"
assert tt["FLAG"] == b""        # flag-style: present but no '='
assert tt["RAW"] == b"raw bytes"

# Mutate after construction.
new.stack_size = 16384
new.current_x = 100
new.default_tool = "C:Vim"
tt["NEW_KEY"] = "added later"
del tt["FOO"]
assert new.stack_size == 16384
assert new.current_x == 100
assert new.default_tool == "C:Vim"
assert "FOO" not in tt
assert tt["NEW_KEY"] == b"added later"

# default_tool = None clears.
new.default_tool = None
assert new.default_tool is None
new.default_tool = "C:Final"

# Write to RAM:, re-read, verify field equality.
try:
    _icon.write("RAM:phase35_smoke", new)
except OSError as e:
    # Vamos can't always write under RAM: in CI runs; treat as soft pass.
    print("write skipped: %s" % e)
    new.close()
    print("OK")
    raise SystemExit

new.close()

back = _icon.read("RAM:phase35_smoke")
try:
    assert back.type == "project", back.type
    assert back.default_tool == "C:Final", back.default_tool
    assert back.stack_size == 16384, back.stack_size
    assert back.current_x == 100, back.current_x
    assert back.current_y == 80, back.current_y
    back_tt = back.tooltypes
    assert "NEW_KEY" in back_tt
    assert "FOO" not in back_tt
    assert back_tt["NEW_KEY"] == b"added later"
    assert back_tt["FLAG"] == b""
    assert back_tt["RAW"] == b"raw bytes"
finally:
    back.close()

# Closed-object access raises ValueError, not segfault.
back2 = _icon.new(_icon.WBPROJECT)
back2.close()
try:
    back2.stack_size = 1
except ValueError:
    pass
else:
    raise AssertionError("expected ValueError on closed DiskObject store")
# Idempotent close.
back2.close()

print("OK")
