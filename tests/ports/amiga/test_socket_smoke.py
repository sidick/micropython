# socket module surface smoke test.
#
# ports/amiga/modsocket.c wraps bsdsocket.library (AmiTCP, MiamiDx,
# Roadshow, etc.). Under vamos bsdsocket.library isn't available, so
# the test can only exercise the static surface and the
# "library not available" error path. End-to-end socket I/O is left
# to Amiberry / real Amiga with a TCP stack installed -- the
# accompanying smoke story for that lives in the Amiberry test rig.

import socket

# --- module surface ------------------------------------------------------

# The factory + helpers.
assert isinstance(socket.socket, type), socket.socket
for name in ("getaddrinfo", "getfqdn"):
    assert hasattr(socket, name), name

# Address family constants.
assert socket.AF_UNSPEC == 0
assert socket.AF_INET == 2
assert socket.AF_INET6 == 23

# Socket type constants.
assert socket.SOCK_STREAM == 1
assert socket.SOCK_DGRAM == 2
assert socket.SOCK_RAW == 3

# IP protocol constants.
assert socket.IPPROTO_IP == 0
assert socket.IPPROTO_TCP == 6
assert socket.IPPROTO_UDP == 17

# Socket-option level + flags. These use AmigaOS bsdsocket numbering
# (SOL_SOCKET = 0xFFFF, the *_TIMEO codes are 16-bit offsets) which
# differs from Linux's SOL_SOCKET=1; the test pins the AmigaOS values
# so a copy-paste regression against Linux headers is caught.
assert socket.SOL_SOCKET == 0xFFFF
assert socket.SO_REUSEADDR == 4
assert socket.SO_RCVTIMEO == 4102
assert socket.SO_SNDTIMEO == 4101

# Methods + attrs all exist on the type.
for name in ("connect", "bind", "listen", "accept",
             "recv", "recvfrom", "send", "sendall", "sendto",
             "setblocking", "settimeout", "setsockopt",
             "close", "fileno", "makefile", "getpeername",
             "read", "readinto", "readline", "write"):
    assert hasattr(socket.socket, name), name

# --- construction fails cleanly when bsdsocket isn't open ----------------

# socket.socket() should raise OSError if the bsdsocket.library lookup
# failed at startup (vamos case). On a real Amiga with AmiTCP/Roadshow
# this would succeed and return a connected socket object instead;
# the live path is exercised on Amiberry.
try:
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    # bsdsocket was available -- nothing to assert beyond shape.
    assert isinstance(s, socket.socket)
    s.close()
except OSError as e:
    # Expected under vamos. The error message should mention bsdsocket
    # to make the failure cause obvious.
    assert "bsdsocket" in str(e).lower(), e

print("OK")
