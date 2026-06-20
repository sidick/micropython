# _amiga memory-access primitives smoke test.
#
# Pins the raw read/write surface that Library / TagList / Struct all
# build on top of: alloc_vec / free_vec plus peek_b/w/l/bytes and
# poke_b/w/l/bytes. m68k is big-endian, so the byte-level layout is
# fixed (most significant byte at the lowest address).

import _amiga

# --- module surface ------------------------------------------------------

for name in (
    "alloc_vec",
    "free_vec",
    "peek_b",
    "peek_w",
    "peek_l",
    "peek_bytes",
    "poke_b",
    "poke_w",
    "poke_l",
    "poke_bytes",
    "MEMF_ANY",
    "MEMF_PUBLIC",
    "MEMF_CHIP",
    "MEMF_FAST",
    "MEMF_CLEAR",
):
    assert hasattr(_amiga, name), name

# --- alloc_vec / free_vec round trip -----------------------------------

buf = _amiga.alloc_vec(64, _amiga.MEMF_ANY | _amiga.MEMF_CLEAR)
assert isinstance(buf, int), type(buf)
assert buf != 0, "alloc_vec(64) returned NULL"

# MEMF_CLEAR zeros the whole allocation -- check at four offsets.
for off in (0, 16, 32, 60):
    assert _amiga.peek_b(buf + off) == 0, "MEMF_CLEAR byte at off=%d" % off
assert _amiga.peek_l(buf) == 0
assert _amiga.peek_l(buf + 60) == 0

# --- byte / word / long round trip -------------------------------------

_amiga.poke_b(buf, 0xAB)
assert _amiga.peek_b(buf) == 0xAB

_amiga.poke_w(buf + 2, 0x1234)
assert _amiga.peek_w(buf + 2) == 0x1234

_amiga.poke_l(buf + 4, 0xDEADBEEF)
assert _amiga.peek_l(buf + 4) == 0xDEADBEEF

# --- big-endian byte order (m68k) --------------------------------------

# Four poke_b writes at consecutive offsets must compose into the
# expected peek_l value with the MSB at the lowest address.
_amiga.poke_b(buf + 8, 0xDE)
_amiga.poke_b(buf + 9, 0xAD)
_amiga.poke_b(buf + 10, 0xBE)
_amiga.poke_b(buf + 11, 0xEF)
assert _amiga.peek_l(buf + 8) == 0xDEADBEEF, "expected big-endian m68k layout"

# Same shape for word.
_amiga.poke_b(buf + 12, 0x12)
_amiga.poke_b(buf + 13, 0x34)
assert _amiga.peek_w(buf + 12) == 0x1234

# Inverse: a poke_l followed by four peek_b reads.
_amiga.poke_l(buf + 16, 0xCAFEF00D)
assert _amiga.peek_b(buf + 16) == 0xCA
assert _amiga.peek_b(buf + 17) == 0xFE
assert _amiga.peek_b(buf + 18) == 0xF0
assert _amiga.peek_b(buf + 19) == 0x0D

# --- peek_bytes / poke_bytes ------------------------------------------

payload = b"Hello, AmigaOS!"
_amiga.poke_bytes(buf + 24, payload)
assert _amiga.peek_bytes(buf + 24, len(payload)) == payload

# Zero-length read returns empty bytes.
assert _amiga.peek_bytes(buf + 0, 0) == b""

# Round-trip a full buffer's worth of zero bytes.
zeros = bytes(32)
_amiga.poke_bytes(buf + 0, zeros)
assert _amiga.peek_bytes(buf, 32) == zeros

# Non-ASCII / high-bit bytes survive untouched.
hi = bytes(range(248, 256))
_amiga.poke_bytes(buf + 40, hi)
assert _amiga.peek_bytes(buf + 40, 8) == hi

# --- poke_b / poke_w value masking ------------------------------------

# Out-of-range arguments are truncated to the low N bits; this is what
# lets callers do `poke_b(addr, signed_byte)` without an explicit & 0xff.
_amiga.poke_b(buf + 0, 256)
assert _amiga.peek_b(buf + 0) == 0x00
_amiga.poke_b(buf + 1, -1)
assert _amiga.peek_b(buf + 1) == 0xFF
_amiga.poke_w(buf + 4, 0x10000)
assert _amiga.peek_w(buf + 4) == 0x0000

# --- cleanup -----------------------------------------------------------

_amiga.free_vec(buf)
# Freeing NULL is documented as a no-op (mirrors AllocVec's FreeVec(NULL)).
_amiga.free_vec(0)

# --- MEMF flags are real bits ------------------------------------------

# The constants are AmigaOS bit masks; PUBLIC/CHIP/FAST/CLEAR are
# distinct non-overlapping bits in the real header. Just check no two
# accidentally collapse to the same value.
flags = {
    "ANY": _amiga.MEMF_ANY,
    "PUBLIC": _amiga.MEMF_PUBLIC,
    "CHIP": _amiga.MEMF_CHIP,
    "FAST": _amiga.MEMF_FAST,
    "CLEAR": _amiga.MEMF_CLEAR,
}
seen = {}
for name, val in flags.items():
    assert isinstance(val, int), (name, val)
    assert val not in seen, (name, val, seen[val])
    seen[val] = name

print("OK")
