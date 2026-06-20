# Phase 34 wiring smoke -- exercises the frozen os.py / _ospath.py
# extensions. Runs under vamos and on real hardware.

import os

# ---------- Module surface ----------
assert hasattr(os, "chmod"), "chmod missing"
assert hasattr(os, "getprotect"), "getprotect missing"
for name in (
    "FIBF_READ",
    "FIBF_WRITE",
    "FIBF_EXECUTE",
    "FIBF_DELETE",
    "FIBF_ARCHIVE",
    "FIBF_PURE",
    "FIBF_SCRIPT",
    "FIBF_HOLD",
):
    assert hasattr(os, name), name
    assert isinstance(getattr(os, name), int), name

# Standard CPython surface inherited from the C-side os module.
assert callable(os.mkdir)
assert callable(os.listdir)
assert callable(os.stat)
assert callable(os.getcwd)

# Phase 34 additions.
assert callable(os.makedirs)
assert callable(os.walk)

# os.path is the AmigaOS-aware _ospath.
import _ospath

assert os.path is _ospath, "os.path should be _ospath"

# ---------- _ospath.isabs ----------
assert _ospath.isabs("Sys:")
assert _ospath.isabs("Sys:Prefs")
assert _ospath.isabs("Work:scripts/foo.py")
assert _ospath.isabs(":foo")  # colon-at-zero is the current-volume form
assert not _ospath.isabs("foo")
assert not _ospath.isabs("foo/bar")
assert not _ospath.isabs("")

# ---------- _ospath.join ----------
assert _ospath.join("Work:", "scripts") == "Work:scripts", _ospath.join("Work:", "scripts")
assert _ospath.join("Work:", "scripts", "foo.py") == "Work:scripts/foo.py"
assert _ospath.join("Work:scripts", "foo.py") == "Work:scripts/foo.py"
assert _ospath.join("foo", "bar") == "foo/bar"
assert _ospath.join("Work:", "bin", "C:tool") == "C:tool"  # absolute resets
assert _ospath.join("") == ""
assert _ospath.join("", "foo") == "foo"

# ---------- _ospath.split / splitext ----------
assert _ospath.split("Work:scripts/foo.py") == ("Work:scripts", "foo.py")
assert _ospath.split("Work:foo.py") == ("Work:", "foo.py")
assert _ospath.split("foo.py") == ("", "foo.py")
assert _ospath.split("Work:") == ("Work:", "")

assert _ospath.splitext("Work:scripts/foo.py") == ("Work:scripts/foo", ".py")
assert _ospath.splitext("foo") == ("foo", "")
assert _ospath.splitext("Work:.profile") == ("Work:.profile", "")

# ---------- _ospath.basename / dirname ----------
assert _ospath.basename("Work:scripts/foo.py") == "foo.py"
assert _ospath.basename("Work:") == ""
assert _ospath.dirname("Work:scripts/foo.py") == "Work:scripts"
assert _ospath.dirname("Work:") == "Work:"

# ---------- _ospath.normpath ----------
assert _ospath.normpath("Work:scripts/../bin") == "Work:bin"
assert _ospath.normpath("Work:scripts/./foo.py") == "Work:scripts/foo.py"
assert _ospath.normpath("Work:scripts/../bin/./tool") == "Work:bin/tool"
# clamp at volume boundary -- can't go above Work:
assert _ospath.normpath("Work:..") == "Work:"
# pure relative
assert _ospath.normpath("foo/../bar") == "bar"
assert _ospath.normpath(".") == "."

# ---------- _ospath.abspath ----------
# Just confirm it doesn't crash; the result depends on cwd.
result = _ospath.abspath("foo")
assert isinstance(result, str) and len(result) > 0

# Absolute path passes through unchanged.
assert _ospath.abspath("Work:foo") == "Work:foo"

# ---------- _ospath.exists / isfile / isdir ----------
# mp: itself (the vamos-mounted tests dir) should exist and be a dir.
assert _ospath.exists("mp:")
assert _ospath.isdir("mp:")
assert not _ospath.isfile("mp:")

# A path that definitely doesn't exist.
assert not _ospath.exists("RAM:nonexistent_phase34")
assert not _ospath.isfile("RAM:nonexistent_phase34")
assert not _ospath.isdir("RAM:nonexistent_phase34")

# ---------- os.getprotect ----------
# Lookup on the tests dir itself; on vamos this returns 0 (allow all).
try:
    mask = os.getprotect("mp:")
    assert isinstance(mask, int), mask
    print("mp: protection = %d" % mask)
except OSError as e:
    print("mp: getprotect:", e)

# Missing path -> OSError, not segfault.
try:
    os.getprotect("RAM:phase34_definitely_missing")
except OSError:
    pass
else:
    raise AssertionError("expected OSError for missing path")

# ---------- os.makedirs / os.walk on RAM: ----------
# Vamos's RAM: -> host T: dir; make sure we can create + walk.
try:
    os.makedirs("RAM:phase34/a/b/c")
except OSError as e:
    print("makedirs skipped:", e)
else:
    assert _ospath.isdir("RAM:phase34/a/b/c")
    # exist_ok suppresses the second call's EEXIST.
    os.makedirs("RAM:phase34/a/b/c", exist_ok=True)

    # Write a small file inside.
    with open("RAM:phase34/a/b/file.txt", "w") as f:
        f.write("hi")
    assert _ospath.isfile("RAM:phase34/a/b/file.txt")

    # walk yields the tree.
    seen = []
    for dirpath, dirs, files in os.walk("RAM:phase34"):
        seen.append((dirpath, sorted(dirs), sorted(files)))
    # Should at minimum include the root with `a` as a subdirectory.
    roots = [t[0] for t in seen]
    assert "RAM:phase34" in roots, roots

    # Cleanup.
    os.remove("RAM:phase34/a/b/file.txt")
    os.rmdir("RAM:phase34/a/b/c")
    os.rmdir("RAM:phase34/a/b")
    os.rmdir("RAM:phase34/a")
    os.rmdir("RAM:phase34")

print("OK")
