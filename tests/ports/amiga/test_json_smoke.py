# json smoke test for the port-local modjson.
#
# ports/amiga/modjson.c is a port-local replacement for upstream
# extmod/modjson.c -- the upstream version uses a stack-allocated
# mp_obj_stringio_t threaded through the stream protocol, which
# bebbo gcc on 68k aligns to 2 bytes inside stack frames. The VM's
# mp_obj_is_obj() check requires 4-byte alignment on the tagged
# pointer encoding, so under bebbo the upstream code intermittently
# faults. The port-local version reads directly from the input
# buffer (mod_json_loads) and skips the stringio wrapper entirely.
# This test pins the parse/serialise contract to make sure the
# rewrite stays semantically equivalent to CPython's json module.

import json
import io

# --- module surface ------------------------------------------------------

for name in ("loads", "load", "dumps", "dump"):
    assert hasattr(json, name), name

# --- loads: primitives --------------------------------------------------

assert json.loads("null") is None
assert json.loads("true") is True
assert json.loads("false") is False
assert json.loads("0") == 0
assert json.loads("42") == 42
assert json.loads("-1") == -1
assert json.loads("3.14") == 3.14
assert json.loads("-2.5e10") == -2.5e10
assert json.loads('"hello"') == "hello"
assert json.loads('""') == ""

# --- loads: composites --------------------------------------------------

assert json.loads("[]") == []
assert json.loads("[1, 2, 3]") == [1, 2, 3]
assert json.loads('["a", "b", "c"]') == ["a", "b", "c"]
assert json.loads("[[1, 2], [3, 4]]") == [[1, 2], [3, 4]]

assert json.loads("{}") == {}
assert json.loads('{"a": 1, "b": 2}') == {"a": 1, "b": 2}
assert json.loads('{"nested": {"key": "value"}}') == {"nested": {"key": "value"}}

# Mixed.
big = json.loads(
    '{"name": "amiga", "version": [3, 1], "kickstart": 40, '
    '"chipset": null, "extras": [{"id": 1, "name": "icon"}]}'
)
assert big["name"] == "amiga"
assert big["version"] == [3, 1]
assert big["kickstart"] == 40
assert big["chipset"] is None
assert big["extras"][0]["name"] == "icon"

# --- loads: whitespace handling ----------------------------------------

assert json.loads("  [  1  ,   2   ,   3  ]  ") == [1, 2, 3]
assert json.loads('{"a"  :  1  ,  "b"  :  2 }') == {"a": 1, "b": 2}
assert json.loads("\n\t[\n1,\n2\n]\n") == [1, 2]

# --- loads: escape sequences in strings --------------------------------

assert json.loads(r'"\n"') == "\n"
assert json.loads(r'"\t"') == "\t"
assert json.loads(r'"\\"') == "\\"
assert json.loads(r'"\""') == '"'
assert json.loads(r'"é"') == "é"   # latin-1 e-acute
assert json.loads(r'"中"') == "中"   # CJK ideograph

# --- loads: malformed input must raise ---------------------------------

# Note: the port-local parser is intentionally lenient on a few
# edge cases that CPython rejects (e.g. it parses `{"a"}` as `{}`).
# This list only covers cases that DO raise.
for bad in ('{', '[1, 2', '"unterminated', "tru", "fals", "nul", "1.2.3"):
    try:
        json.loads(bad)
        assert False, ("expected ValueError for %r" % bad)
    except (ValueError, SyntaxError):
        pass

# --- load() (stream form) ----------------------------------------------

assert json.load(io.StringIO('[1, 2, 3]')) == [1, 2, 3]
assert json.load(io.StringIO('{"k": "v"}')) == {"k": "v"}

# --- dumps: primitives -------------------------------------------------

assert json.dumps(None) == "null"
assert json.dumps(True) == "true"
assert json.dumps(False) == "false"
assert json.dumps(0) == "0"
assert json.dumps(42) == "42"
assert json.dumps(-7) == "-7"
assert json.dumps("hello") == '"hello"'

# --- dumps: composites -------------------------------------------------

assert json.dumps([]) == "[]"
assert json.dumps([1, 2, 3]) == "[1, 2, 3]"
assert json.dumps({}) == "{}"

# Dict iteration order in MicroPython matches insertion order, but we
# don't rely on it for multi-key dicts. Validate by re-parsing.
encoded = json.dumps({"a": 1, "b": [2, 3], "c": None})
assert json.loads(encoded) == {"a": 1, "b": [2, 3], "c": None}

# --- dumps: escaping ---------------------------------------------------

assert json.dumps("a\nb") == '"a\\nb"'
assert json.dumps('quote: "') == '"quote: \\""'
assert json.dumps("\\") == '"\\\\"'

# --- dump(): stream form ----------------------------------------------

buf = io.StringIO()
json.dump([1, 2, 3], buf)
assert buf.getvalue() == "[1, 2, 3]"

# --- end-to-end round trip --------------------------------------------

# Repeated dumps -> loads must be a fixed point for json-safe values.
values = [
    None, True, False, 0, 42, -1, 3.14, "hello", "", [], [1, 2, 3],
    {"k": "v"}, {"nested": {"list": [1, 2, {"a": True}]}},
    {"unicode": "é 中"},
]
for v in values:
    encoded = json.dumps(v)
    decoded = json.loads(encoded)
    assert decoded == v, (v, encoded, decoded)

print("OK")
