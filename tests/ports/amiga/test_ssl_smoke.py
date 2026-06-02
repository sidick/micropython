# ssl module surface smoke test.
#
# ports/amiga/modssl.c wraps AmiSSL v5 (the OpenSSL build for AmigaOS).
# AmiSSL itself isn't shipped with vamos, so the live wrap_socket path
# is exercised on Amiberry (project memory project_amissl_installed)
# rather than here -- this test pins what vamos *can* verify: module
# surface, constants, and the OSError raised when amisslmaster.library
# isn't around.

import ssl

# --- module surface ------------------------------------------------------

assert isinstance(ssl.SSLContext, type)

# Protocol constants.
assert ssl.PROTOCOL_TLS_CLIENT == 0
assert ssl.PROTOCOL_TLS_SERVER == 1

# Verify-mode constants. The values must match the CPython ssl module
# so callers can write `ctx.verify_mode = ssl.CERT_REQUIRED` without
# remembering an Amiga-specific number.
assert ssl.CERT_NONE == 0
assert ssl.CERT_OPTIONAL == 1
assert ssl.CERT_REQUIRED == 2

# SSLContext method names.
for name in ("load_verify_locations", "set_default_verify_paths",
             "wrap_socket", "close"):
    assert hasattr(ssl.SSLContext, name), name

# --- construction fails cleanly when AmiSSL isn't open ------------------

# SSLContext() opens amisslmaster.library on demand. Under vamos that
# library isn't present and the constructor raises OSError. On a real
# Amiga with AmiSSL v5 installed (or Amiberry with the bundled image)
# it succeeds; both paths are accepted so the test is portable to the
# live rig too.
try:
    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
    # AmiSSL was available -- only assert basic shape and clean up.
    assert ctx is not None
    ctx.close()
except OSError as e:
    msg = str(e).lower()
    # The diagnostic should mention amissl or the library name so the
    # cause is obvious from the error alone.
    assert "amissl" in msg or "library" in msg, e

print("OK")
