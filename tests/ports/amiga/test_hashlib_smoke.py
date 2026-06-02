# Phase 39 -- hashlib MD5 + SHA1 surface.
#
# The Amiga port wires the upstream extmod modhashlib axtls path so that
# md5 and sha1 join the already-present sha256 in the hashlib module.
# This test pins each algorithm against well-known reference vectors and
# also covers the streaming update() / final digest() lifecycle.

import hashlib

# --- module surface ------------------------------------------------------

for algo in ("md5", "sha1", "sha256"):
    assert hasattr(hashlib, algo), algo

# --- one-shot constructor digests ---------------------------------------

# RFC 1321 MD5 test vector "" (empty string).
assert hashlib.md5(b"").digest() == bytes.fromhex(
    "d41d8cd98f00b204e9800998ecf8427e"
)
# RFC 1321 MD5 test vector "abc".
assert hashlib.md5(b"abc").digest() == bytes.fromhex(
    "900150983cd24fb0d6963f7d28e17f72"
)
# RFC 1321 MD5 test vector "message digest".
assert hashlib.md5(b"message digest").digest() == bytes.fromhex(
    "f96b697d7cb7938d525a2f31aaf161d0"
)

# FIPS 180-1 SHA-1 test vector "" (empty string).
assert hashlib.sha1(b"").digest() == bytes.fromhex(
    "da39a3ee5e6b4b0d3255bfef95601890afd80709"
)
# FIPS 180-1 SHA-1 test vector "abc".
assert hashlib.sha1(b"abc").digest() == bytes.fromhex(
    "a9993e364706816aba3e25717850c26c9cd0d89d"
)
# FIPS 180-1 SHA-1 test vector "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq".
assert hashlib.sha1(
    b"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq"
).digest() == bytes.fromhex("84983e441c3bd26ebaae4aa1f95129e5e54670f1")

# FIPS 180-2 SHA-256 test vectors (sanity-check the algorithm still works
# after the hashlib rewire).
assert hashlib.sha256(b"").digest() == bytes.fromhex(
    "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"
)
assert hashlib.sha256(b"abc").digest() == bytes.fromhex(
    "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad"
)

# --- streaming update() lifecycle ---------------------------------------

# Hash split across multiple update() calls must equal one-shot result.
m = hashlib.md5()
m.update(b"mes")
m.update(b"sage ")
m.update(b"digest")
assert m.digest() == hashlib.md5(b"message digest").digest()

s = hashlib.sha1()
s.update(b"a")
s.update(b"b")
s.update(b"c")
assert s.digest() == hashlib.sha1(b"abc").digest()

# --- finality enforcement -----------------------------------------------

# After digest() the object is finalised: further update() / digest()
# must raise. Matches the upstream "hash is final" ValueError contract.
m = hashlib.md5(b"x")
m.digest()
try:
    m.update(b"y")
    assert False, "expected ValueError on update-after-final"
except ValueError:
    pass
try:
    m.digest()
    assert False, "expected ValueError on digest-after-final"
except ValueError:
    pass

# --- digest sizes -------------------------------------------------------

assert len(hashlib.md5(b"").digest()) == 16
assert len(hashlib.sha1(b"").digest()) == 20
assert len(hashlib.sha256(b"").digest()) == 32

print("OK")
