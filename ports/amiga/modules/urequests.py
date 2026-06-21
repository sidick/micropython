# urequests -- HTTP/1.0 client for the MicroPython Amiga port.
#
# Phase 29 covers Steps 1-5: GET / HEAD / POST / PUT / DELETE / PATCH
# verbs, fixed-length and chunked response bodies, gzip / deflate
# response decompression, HTTPS via ssl.SSLContext, plus
# data= / json= / headers= keyword arguments on request().
#
# Shape mirrors upstream `micropython-lib`'s requests / urequests so
# existing examples and tutorials transfer:
#
#     import urequests
#     r = urequests.get("https://www.python.org/")
#     print(r.status_code, r.reason)
#     print(r.text[:80])
#     r.close()
#
# Known limitation: HTTPS against modern CDNs that gate on TLS-1.3
# client fingerprinting (Cloudflare, GitHub) inherits the Phase 28
# AmiSSL issue tracked at https://github.com/jens-maus/amissl/issues/111
# -- the handshake completes, then the next write returns broken
# pipe. Direct origins that still negotiate TLS 1.2 by default
# (e.g. www.python.org) work cleanly.

import socket

_VERSION = "0.1"
_DEFAULT_UA = "MicroPython-Amiga/" + _VERSION


class Response:
    def __init__(self, sock):
        self.raw = sock
        self.status_code = None
        self.reason = ""
        self.headers = {}
        self.encoding = "utf-8"
        self._buf = b""
        self._cached = None

    def _fill(self):
        # Pull more bytes into the internal read buffer. False on EOF.
        chunk = self.raw.recv(4096)
        if not chunk:
            return False
        self._buf = self._buf + chunk
        return True

    def _read_line(self):
        # Read one LF-terminated line (including the LF). On EOF
        # before any LF, return whatever's left -- the upstream
        # header loop treats that as "headers done".
        while b"\n" not in self._buf:
            if not self._fill():
                line = self._buf
                self._buf = b""
                return line
        idx = self._buf.index(b"\n") + 1
        line = self._buf[:idx]
        self._buf = self._buf[idx:]
        return line

    def _read_raw(self, size):
        # Read exactly `size` bytes (or fewer on EOF).
        while len(self._buf) < size:
            if not self._fill():
                break
        n = min(size, len(self._buf))
        out = bytes(self._buf[:n])
        self._buf = self._buf[n:]
        return out

    def _read_status_line(self):
        line = self._read_line()
        # "HTTP/1.x NNN reason\r\n"
        parts = line.split(None, 2)
        if len(parts) < 2:
            raise OSError("bad status line")
        self.status_code = int(parts[1])
        if len(parts) >= 3:
            self.reason = parts[2].strip().decode()

    def _read_headers(self):
        while True:
            line = self._read_line()
            if not line or line == b"\r\n" or line == b"\n":
                break
            if b":" not in line:
                continue
            k, _, v = line.partition(b":")
            self.headers[k.decode().lower()] = v.strip().decode()

    def _read_chunked(self):
        # chunked = (chunk-size [;ext] CRLF chunk-data CRLF)* 0 CRLF
        #           trailer-headers CRLF
        chunks = []
        while True:
            line = self._read_line()
            size_str = line.strip()
            if b";" in size_str:
                size_str = size_str.split(b";", 1)[0]
            if not size_str:
                break
            chunk_size = int(size_str, 16)
            if chunk_size == 0:
                # Final chunk -- drain optional trailer headers then stop.
                while True:
                    trailer = self._read_line()
                    if not trailer or trailer == b"\r\n" or trailer == b"\n":
                        break
                break
            chunks.append(self._read_raw(chunk_size))
            # CRLF after each chunk's data.
            self._read_line()
        return b"".join(chunks)

    @property
    def content(self):
        if self._cached is None:
            tenc = self.headers.get("transfer-encoding", "").lower()
            if "chunked" in tenc:
                data = self._read_chunked()
            else:
                cl = self.headers.get("content-length")
                if cl is not None:
                    data = self._read_raw(int(cl))
                else:
                    # No Content-Length: read until EOF. Safe because
                    # we send Connection: close, so the server FINs
                    # after sending the body.
                    chunks = [self._buf]
                    self._buf = b""
                    while True:
                        chunk = self.raw.recv(4096)
                        if not chunk:
                            break
                        chunks.append(chunk)
                    data = b"".join(chunks)
            ce = self.headers.get("content-encoding", "").lower()
            if "gzip" in ce or "deflate" in ce:
                # Lazy import keeps the module light when no compression
                # is in play. Most AmiSSL-served pages don't compress.
                import deflate
                import io

                fmt = deflate.GZIP if "gzip" in ce else deflate.ZLIB
                data = deflate.DeflateIO(io.BytesIO(data), fmt).read()
            if self.raw is not None:
                self.raw.close()
                self.raw = None
            self._cached = data
        return self._cached

    @property
    def text(self):
        try:
            return self.content.decode(self.encoding)
        except (UnicodeError, ValueError):
            # MicroPython's decode() doesn't take errors="replace";
            # substitute non-ASCII bytes with '?' so .text at least
            # round-trips. Callers wanting the raw bytes use .content.
            buf = self.content
            arr = bytearray(len(buf))
            for i in range(len(buf)):
                arr[i] = buf[i] if buf[i] < 128 else 63  # '?'
            return arr.decode()

    def json(self):
        import json

        return json.loads(self.text)

    def close(self):
        if self.raw is not None:
            self.raw.close()
            self.raw = None


def _parse_url(url):
    # Returns (use_ssl, host, port, path).
    if url.startswith("https://"):
        body = url[8:]
        use_ssl = True
        default_port = 443
    elif url.startswith("http://"):
        body = url[7:]
        use_ssl = False
        default_port = 80
    else:
        raise ValueError("only http:// and https:// URLs are supported")
    slash = body.find("/")
    if slash < 0:
        hostport = body
        path = "/"
    else:
        hostport = body[:slash]
        path = body[slash:]
    if ":" in hostport:
        host, port_s = hostport.split(":", 1)
        port = int(port_s)
    else:
        host = hostport
        port = default_port
    return use_ssl, host, port, path


_QUOTE_SAFE = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~"


def _quote(s):
    # Minimal percent-encoding for the unreserved set; everything else
    # gets %XX (UTF-8 byte by byte). Enough for application/
    # x-www-form-urlencoded payloads in Step 4.
    if isinstance(s, str):
        s = s.encode("utf-8")
    out = []
    for b in s:
        if b in _QUOTE_SAFE:
            out.append(bytes([b]))
        else:
            out.append(b"%%%02X" % b)
    return b"".join(out)


def _urlencode(d):
    parts = []
    for k, v in d.items():
        parts.append(_quote(str(k)) + b"=" + _quote(str(v)))
    return b"&".join(parts)


# One TLS context, built lazily and reused for every HTTPS request.
# Each ssl.SSLContext() + set_default_verify_paths() loads the AmiSSL CA
# store, which is slow; rebuilding it per request also piles up CA stores
# that aren't freed until GC, exhausting AmiSSL/socket resources on
# repeated HTTPS calls in one process. SSL_CTX is designed to be shared
# (a fresh SSL is made per wrap_socket), so one context keeps in-process
# repeat requests reliable.
_ssl_context = None


def _https_context():
    global _ssl_context
    if _ssl_context is None:
        import ssl

        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
        ctx.verify_mode = ssl.CERT_REQUIRED
        ctx.set_default_verify_paths()
        _ssl_context = ctx
    return _ssl_context


def request(method, url, *, data=None, json=None, headers=None):
    use_ssl, host, port, path = _parse_url(url)

    addr = socket.getaddrinfo(host, port)[0][-1]
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.connect(addr)
        if use_ssl:
            s = _https_context().wrap_socket(s, server_hostname=host)
    except Exception:
        # Don't leak the socket (or its SSL state) on a failed connect or
        # TLS handshake. A leftover fd/SSL after a request to a server
        # AmiSSL can't complete with poisons later requests in the same
        # process -- they start failing with EIO/EINVAL.
        s.close()
        raise

    # Resolve the request body once so we can set Content-Length up
    # front. dict body -> urlencoded; str body -> utf-8 bytes; bytes
    # body -> as-is; json= -> JSON serialisation + auto Content-Type.
    body = None
    auto_content_type = None
    if json is not None:
        import json as _json

        body = _json.dumps(json).encode()
        auto_content_type = "application/json"
    elif isinstance(data, dict):
        body = _urlencode(data)
        auto_content_type = "application/x-www-form-urlencoded"
    elif isinstance(data, str):
        body = data.encode()
    elif isinstance(data, (bytes, bytearray, memoryview)):
        body = bytes(data)
    elif data is None:
        body = None
    else:
        raise TypeError("data must be dict/str/bytes/None")

    # Apply caller-supplied headers on top of our defaults
    # case-insensitively. Caller wins -- e.g. headers={"User-Agent": "x"}
    # replaces our default UA, not stacks on top.
    final = {
        "Host": host,
        "Connection": "close",
        "User-Agent": _DEFAULT_UA,
        "Accept-Encoding": "gzip",
    }
    if auto_content_type is not None:
        final["Content-Type"] = auto_content_type
    if body is not None:
        final["Content-Length"] = str(len(body))
    if headers:
        # Drop any of our defaults whose key is overridden by the caller.
        caller_keys_lc = {k.lower() for k in headers}
        for key in list(final):
            if key.lower() in caller_keys_lc:
                del final[key]
        for k, v in headers.items():
            final[k] = str(v)

    # Send request head.
    if isinstance(method, str):
        method = method.encode()
    try:
        s.send(method + b" " + path.encode() + b" HTTP/1.0\r\n")
        for k, v in final.items():
            s.send(k.encode() + b": " + str(v).encode() + b"\r\n")
        s.send(b"\r\n")
        if body is not None:
            s.send(body)

        resp = Response(s)
        resp._read_status_line()
        resp._read_headers()
    except Exception:
        # Same reasoning as above: close on any send/response failure so a
        # half-finished request can't leak the socket.
        s.close()
        raise
    return resp


def get(url, **kw):
    return request("GET", url, **kw)


def head(url, **kw):
    return request("HEAD", url, **kw)


def post(url, **kw):
    return request("POST", url, **kw)


def put(url, **kw):
    return request("PUT", url, **kw)


def delete(url, **kw):
    return request("DELETE", url, **kw)


def patch(url, **kw):
    return request("PATCH", url, **kw)
