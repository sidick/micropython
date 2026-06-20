# _amiga introspection surface smoke test.
#
# Covers the dos.library / exec.library / pattern-matching accessors
# that the platform module and assorted helper code rely on:
#   os_version, heap_info, exists, volumes, assigns, disk_info,
#   match, imatch.
# vamos returns stub names ("DosListName:") for volumes() / assigns()
# entries and crashes inside dos.library Info() if we actually ask for
# disk_info() on a real volume, so the test pins the shape (right
# types, sensible values) without depending on the values themselves
# being meaningful under vamos.

import _amiga

# --- os_version ---------------------------------------------------------

ver = _amiga.os_version()
assert isinstance(ver, tuple) and len(ver) == 2, ver
major, minor = ver
assert isinstance(major, int) and isinstance(minor, int)
# Kickstart 3.0 = V37, the port's documented minimum. Anything below
# would not be running this test at all.
assert major >= 37, "Kickstart older than V37 should not have booted us"

# --- heap_info ----------------------------------------------------------

info = _amiga.heap_info()
assert isinstance(info, tuple) and len(info) == 3, info
total, free, chunks = info
assert isinstance(total, int) and total > 0
assert isinstance(free, int) and 0 <= free <= total
assert isinstance(chunks, int) and chunks >= 1

# --- exists --------------------------------------------------------------

assert _amiga.exists("mp:") is True
assert _amiga.exists("mp:run-tests.py") is True
assert _amiga.exists("mp:no_such_path_here.xyz") is False

# Empty / nonsense input doesn't crash -- worst case it returns False.
assert _amiga.exists("") is False or _amiga.exists("") is True  # either is acceptable

# --- volumes / assigns shape -------------------------------------------

vols = _amiga.volumes()
assert isinstance(vols, list), type(vols)
# At least one volume is always mounted (RAM: on any live machine,
# whatever vamos hands us under the emulator).
assert len(vols) >= 1
for v in vols:
    assert isinstance(v, str), v
    # AmigaDOS volume names end with a colon.
    assert v.endswith(":"), v

asg = _amiga.assigns()
assert isinstance(asg, dict), type(asg)
for k, v in asg.items():
    assert isinstance(k, str) and k.endswith(":"), k
    assert isinstance(v, str), v

# --- disk_info error path -----------------------------------------------

# A nonexistent volume name must raise OSError. The success path drives
# dos.library's Info() packet which vamos doesn't implement cleanly, so
# we leave that to Amiberry / real-hardware testing.
try:
    _amiga.disk_info("DefinitelyNotAVolume:")
    assert False, "expected OSError"
except OSError:
    pass

# --- match (one-shot list) ----------------------------------------------

# match() takes an AmigaDOS pattern (e.g. "#?.md") and returns the list
# of full paths that match. Returns [] for "no matches", not an error.
matched = _amiga.match("mp:no_such_dir_xyz/#?.py")
assert matched == [], matched

# Exact file match returns a one-element list.
matched = _amiga.match("mp:run-tests.py")
assert matched == ["mp:run-tests.py"], matched

# Pattern with at least one match: every result must be a string and
# the pattern's extension must show up at the end of each path.
md_files = _amiga.match("mp:#?.md")
assert isinstance(md_files, list)
assert len(md_files) >= 1, md_files
for p in md_files:
    assert isinstance(p, str)
    assert p.endswith(".md"), p

# --- imatch (lazy iterator) --------------------------------------------

# imatch() returns an iterator yielding the same paths one at a time.
# Useful when the match set is too large to materialise eagerly.
it = _amiga.imatch("mp:#?")
# Drain the iterator and check we got the same paths the eager call
# would have produced (modulo ordering, which AmigaDOS doesn't promise).
lazy_paths = list(it)
eager_paths = _amiga.match("mp:#?")
assert sorted(lazy_paths) == sorted(eager_paths), (
    len(lazy_paths),
    len(eager_paths),
)

# imatch on a no-match pattern produces an empty iterator.
empty = list(_amiga.imatch("mp:no_such_dir_xyz/#?"))
assert empty == [], empty

# Early break must clean up cleanly (no leftover anchor state).
# We don't have a way to check the FreeVec from Python, but the
# subsequent imatch call below would fail if the previous anchor
# leaked.
it = _amiga.imatch("mp:#?")
first = next(it)
del it  # GC must run MatchEnd + FreeVec for us.
followup = _amiga.match("mp:#?")
assert isinstance(followup, list) and len(followup) >= 1

print("OK")
