# urequests (frozen HTTP/HTTPS client) surface smoke test.
#
# ports/amiga/modules/urequests.py is a frozen module that talks to
# bsdsocket / AmiSSL via the socket and ssl modules. Neither library
# is available under vamos, so the live request() path can't be
# exercised here -- this test pins what *can* be verified statically:
# helper functions (_parse_url, _quote, _urlencode), module surface
# (Response class, get/head/post/put/delete/patch verbs), and the
# argument-shape contract.

import urequests

# --- module surface ------------------------------------------------------

assert hasattr(urequests, "Response")
assert isinstance(urequests.Response, type)
for verb in ("get", "head", "post", "put", "delete", "patch", "request"):
    assert callable(getattr(urequests, verb)), verb

# --- _parse_url: scheme / host / port / path -------------------------

# http with explicit port + query string.
assert urequests._parse_url("http://x.com:80/p?q=1") == (False, "x.com", 80, "/p?q=1")
# https default port.
assert urequests._parse_url("https://example.com") == (True, "example.com", 443, "/")
# https with trailing slash.
assert urequests._parse_url("https://example.com/") == (True, "example.com", 443, "/")
# http default port + nested path.
assert urequests._parse_url("http://api.example.com/v1/things/42") == (
    False,
    "api.example.com",
    80,
    "/v1/things/42",
)

# --- _quote: URL-percent-encode -----------------------------------------

# Spaces, punctuation, control characters all get %HH.
assert urequests._quote(" ") == b"%20"
assert urequests._quote("!") == b"%21"
assert urequests._quote(" hello world!") == b"%20hello%20world%21"
# Safe alphanumerics pass through.
assert urequests._quote("abc123") == b"abc123"
# Empty input.
assert urequests._quote("") == b""

# --- _urlencode: dict -> querystring bytes ------------------------------

# Single key.
assert urequests._urlencode({"a": "1"}) == b"a=1"
# Multiple keys joined with &; iteration order matches insertion.
encoded = urequests._urlencode({"a": "1", "b": "2"})
assert encoded in (b"a=1&b=2", b"b=2&a=1"), encoded
# Empty dict.
assert urequests._urlencode({}) == b""
# Value containing characters that need encoding.
encoded = urequests._urlencode({"q": "hello world"})
assert encoded == b"q=hello%20world", encoded

# --- Response class shape ----------------------------------------------

# Response() requires a connected socket arg; we can't construct one
# under vamos. Just sanity-check the class exposes the attributes the
# callers actually use.
for attr in ("__init__", "close", "json", "text", "content"):
    assert hasattr(urequests.Response, attr), attr

print("OK")
