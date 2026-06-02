# Phase 39 -- btree (Berkeley DB persistent K/V) surface.
#
# Standard MicroPython btree module, opened over an in-memory
# io.BytesIO so the test doesn't depend on the filesystem -- the same
# stream protocol drives a real VfsAmiga file in production use.

import btree
import io

# --- module surface ------------------------------------------------------

assert hasattr(btree, "open")
for cnst in ("INCL", "DESC"):
    assert hasattr(btree, cnst), cnst

# --- open, put, get ------------------------------------------------------

stream = io.BytesIO()
db = btree.open(stream, pagesize=512)

db[b"foo1"] = b"bar1"
db[b"foo2"] = b"bar2"
db[b"foo3"] = b"bar3"

assert db[b"foo1"] == b"bar1"
assert db[b"foo2"] == b"bar2"
assert db[b"foo3"] == b"bar3"

# get() with default.
assert db.get(b"foo1") == b"bar1"
assert db.get(b"missing") is None
assert db.get(b"missing", b"dflt") == b"dflt"

# Missing key via __getitem__ raises KeyError.
try:
    _ = db[b"missing"]
    assert False, "expected KeyError"
except KeyError:
    pass

# --- containment + del --------------------------------------------------

assert b"foo1" in db
assert b"missing" not in db

del db[b"foo2"]
assert b"foo2" not in db
try:
    del db[b"foo2"]
    assert False, "expected KeyError on second delete"
except KeyError:
    pass

# --- buffer-protocol keys/values ----------------------------------------

# btree accepts anything that implements the buffer protocol for both
# key and value, including memoryview slices.
mv = memoryview(b"keyABCvalXY")
db[mv[:6]] = mv[6:]
assert db[b"keyABC"] == b"valXY"

# --- iteration / keys / values / items ---------------------------------

# Keys come back in sorted (ASCII) order.
all_keys = list(db.keys())
assert all_keys == sorted(all_keys), all_keys

# items() yields each key paired with its value. (Can't run keys() and
# values() concurrently to cross-check -- btree shares a single cursor
# across all iterations on the same DB object.)
items = list(db.items())
assert [k for k, _ in items] == all_keys, items
for k, v in items:
    expected_v = db[k]
    assert v == expected_v, (k, v, expected_v)

# items(start, end) is a half-open range over the key space.
ranged = list(db.items(b"foo", b"foo9"))
ranged_keys = [k for k, _ in ranged]
assert b"foo1" in ranged_keys, ranged_keys
assert b"foo3" in ranged_keys, ranged_keys
assert b"keyABC" not in ranged_keys, ranged_keys

# items(None, None, btree.DESC) iterates in reverse order.
desc_keys = [k for k, _ in db.items(None, None, btree.DESC)]
assert desc_keys == list(reversed(sorted(db.keys()))), desc_keys

# Iteration via `for k in db` yields keys.
loop_keys = [k for k in db]
assert sorted(loop_keys) == sorted(db.keys())

# --- explicit put() / flush() / close() ---------------------------------

db.put(b"explicit", b"value")
assert db[b"explicit"] == b"value"

db.flush()
db.close()
stream.close()

# --- closed-db operations raise -----------------------------------------

stream = io.BytesIO()
db = btree.open(stream)
db[b"x"] = b"1"
db.close()
try:
    _ = db[b"x"]
    assert False, "expected error after close"
except (OSError, ValueError):
    # MicroPython raises ValueError("database closed") from
    # check_btree_is_open(); older builds raised OSError. Accept both.
    pass
stream.close()

print("OK")
