# Phase 39 -- deflate compress (write) surface.
#
# MICROPY_PY_DEFLATE_COMPRESS is now on; this test exercises the
# write side of DeflateIO end-to-end. The decompress side has been
# in the port since EXTRA_FEATURES, so we lean on it to verify the
# compressor's output round-trips correctly across all three
# supported formats (raw / zlib / gzip).

import deflate
import io

# --- module surface ------------------------------------------------------

assert hasattr(deflate, "DeflateIO"), "deflate.DeflateIO missing"
assert hasattr(deflate.DeflateIO, "write"), "compress side not enabled"
for fmt in ("RAW", "ZLIB", "GZIP"):
    assert hasattr(deflate, fmt), fmt

# --- compress -> decompress round-trip ----------------------------------


def compress(data, fmt):
    buf = io.BytesIO()
    with deflate.DeflateIO(buf, fmt) as g:
        g.write(data)
    return buf.getvalue()


def decompress(data, fmt):
    buf = io.BytesIO(data)
    with deflate.DeflateIO(buf, fmt) as g:
        return g.read()


# Highly redundant input compresses well across all three formats.
payload = b"micropython " * 64

for fmt, name in [(deflate.RAW, "RAW"), (deflate.ZLIB, "ZLIB"), (deflate.GZIP, "GZIP")]:
    encoded = compress(payload, fmt)
    # Output should be strictly smaller than the original for repetitive input.
    assert len(encoded) < len(payload), (name, len(encoded), len(payload))
    decoded = decompress(encoded, fmt)
    assert decoded == payload, (name, len(decoded), decoded[:32])

# --- streaming write across multiple chunks -----------------------------

# Splitting the input across update-style writes must yield the same
# compressed/decompressed pair as a single write of the same total.
buf = io.BytesIO()
with deflate.DeflateIO(buf, deflate.RAW) as g:
    g.write(b"Lorem ipsum dolor sit amet, ")
    g.write(b"consectetur adipiscing elit. " * 10)
    g.write(b"")  # zero-length write is allowed.
    g.write(b"END.")
streamed = buf.getvalue()
expected_input = (
    b"Lorem ipsum dolor sit amet, "
    + b"consectetur adipiscing elit. " * 10
    + b""
    + b"END."
)
assert decompress(streamed, deflate.RAW) == expected_input

# --- write to a closed DeflateIO must raise -----------------------------

buf = io.BytesIO()
g = deflate.DeflateIO(buf, deflate.RAW)
g.write(b"hello")
g.close()
try:
    g.write(b"more")
    assert False, "expected OSError writing to closed DeflateIO"
except OSError:
    pass

# --- empty-input compression -------------------------------------------

# uzlib's compressor doesn't emit a header/trailer when no data was
# written, so an empty write produces an empty byte stream rather than
# a valid (header-only) zlib/gzip frame. Verify the documented quirk
# rather than treating it as a bug.
for fmt in (deflate.RAW, deflate.ZLIB, deflate.GZIP):
    assert compress(b"", fmt) == b"", fmt

print("OK")
