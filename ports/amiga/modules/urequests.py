# urequests -- HTTP/1.0 client for the MicroPython Amiga port.
#
# Phase 29 Step 1: plain HTTP only, GET / HEAD verbs, fixed-length
# body via Content-Length (read-until-EOF fallback for responses
# that omit it). Chunked Transfer-Encoding + gzip decompression
# arrive in Step 2; HTTPS in Step 3; POST/PUT/DELETE/PATCH and
# data=/json=/headers= kwargs in Step 4.
#
# Shape mirrors upstream `micropython-lib`'s requests / urequests
# so existing examples and tutorials transfer:
#
#     import urequests
#     r = urequests.get("http://www.example.com/")
#     print(r.status_code, r.reason)
#     print(r.text[:80])
#     r.close()

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

    @property
    def content(self):
        if self._cached is None:
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
    # Returns (host, port, path). Step 1: http:// only.
    if url.startswith("http://"):
        body = url[7:]
    else:
        raise ValueError("only http:// URLs are supported")
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
        port = 80
    return host, port, path


def request(method, url, *, headers=None):
    host, port, path = _parse_url(url)

    addr = socket.getaddrinfo(host, port)[0][-1]
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.connect(addr)

    # HTTP/1.0 + Connection: close lets us read-until-EOF when the
    # response has no Content-Length, and keeps us out of the
    # HTTP/1.1 keep-alive state machine until/unless Step 6 adds it.
    if isinstance(method, str):
        method = method.encode()
    s.send(method + b" " + path.encode() + b" HTTP/1.0\r\n")
    s.send(b"Host: " + host.encode() + b"\r\n")
    s.send(b"Connection: close\r\n")
    s.send(b"User-Agent: " + _DEFAULT_UA.encode() + b"\r\n")
    if headers:
        for k, v in headers.items():
            s.send(k.encode() + b": " + str(v).encode() + b"\r\n")
    s.send(b"\r\n")

    resp = Response(s)
    resp._read_status_line()
    resp._read_headers()
    return resp


def get(url, **kw):
    return request("GET", url, **kw)


def head(url, **kw):
    return request("HEAD", url, **kw)
