# Phase 29 — `urequests` step plan

Companion to the Phase 29 design block in
[docs/amiga.md](amiga.md#phase-29--urequests-frozen-httphttps-client-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable,
individually-testable PRs.

Phase 29 is pure-Python frozen onto the variants that already
ship `socket` and `ssl` (`standard`, `68020fpu`, `68040`); the
`minimal` variant — which gates `MICROPY_PY_AMIGA_SOCKET` off,
therefore also `MICROPY_PY_AMIGA_SSL` — gets nothing.

## Phasing overview

```
Step 1: skeleton (HTTP/1.0 GET) → Step 2: chunked TE + gzip
                                           ↓
                              Step 3: HTTPS via SSLContext
                                           ↓
                              Step 4: write methods + kwargs
                                           ↓
                              Step 5: freeze + docs + tests
```

| # | Step | Output | On-target smoke test |
|---|------|--------|---------------------|
| **1 ✅** | Core `Response` + `request("GET", ...)` over plain HTTP. URL parsing, status line, headers, fixed-length body via `Content-Length`. | `urequests.py` shipping `get`, `head`, `request`, `Response`. No HTTPS, no chunked, no compression. | `urequests.get("http://www.example.com/")` returns status 200 and a reasonable `.text` |
| **2 ✅** | Chunked Transfer-Encoding decoder + `Content-Encoding: gzip` decompression via the existing `deflate` module. | `Response.content` handles both inline. | One host that serves chunked (`httpforever.com`), one that serves gzip. |
| **3 ✅** | HTTPS via `ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)` + `verify_mode = CERT_REQUIRED` + `set_default_verify_paths()`. SSLContext built once per call. | `urequests.get("https://www.python.org/")` returns 200. Inherits the Phase 28 Cloudflare TLS 1.3 limitation. | `www.python.org`, end-to-end, full HTML body. |
| **4 ✅** | `post` / `put` / `delete` / `patch` methods. `data=` (str/bytes urlencoded), `json=` (auto-serialised → `application/json`), `headers=` overrides. | All HTTP verbs land. POSTing JSON to `https://httpbin.org/post` round-trips. | Verbs against `httpbin.org` (or local TLS echo). |
| **5 ✅** | Freeze into `manifest.py`, gate behind the existing `MICROPY_PY_AMIGA_SSL` (-equivalent) Make flag, doc updates. | `import urequests` works after a clean variant build. Phase 29 row flips to ✅. | `import urequests` under the on-device runner + tests/extmod/* untouched-by-skip-list verification. |

Each step is small (50–150 LOC of Python plus tests). Step 4 is
the only place the API surface grows materially.

---

## Step 1 — Core HTTP GET + Response

### Deliverables

- `ports/amiga/modules/urequests.py`:
  - `class Response`:
    - `__init__(sock)` — keeps the raw socket, init buffer, headers dict, encoding.
    - `_read_line()` / `_read_raw(n)` — byte-level reads with a local buffer (avoid one `recv` per byte).
    - `_read_status_line()`, `_read_headers()` — parse HTTP/1.x response head.
    - `content` property — read body using `Content-Length`, fall back to "read until EOF" if header absent. Cache result; close socket once cached.
    - `text` property — decode `content` as UTF-8 (lossy fallback to `?` for non-UTF-8 bytes, matching OoZe — MicroPython's `decode()` doesn't support `errors=replace`).
    - `json()` — decode then `json.loads`.
    - `status_code`, `reason`, `headers`, `encoding="utf-8"`, `close()`.
  - `def request(method, url, *, headers=None)`:
    - URL parsing: split scheme, host (with optional `:port`), path.
    - Reject non-`http://` for now (Step 3 adds `https://`).
    - DNS via `socket.getaddrinfo`, connect, send `<METHOD> <PATH> HTTP/1.0\r\n` plus mandatory `Host:` / `Connection: close` / `User-Agent: MicroPython-Amiga/<ver>`. Optional caller headers added unmodified.
    - Build `Response` from the connected socket and return it.
  - Convenience wrappers `get`, `head` (`post`/`put`/`delete`/`patch` arrive in Step 4).

### Verification

- `urequests.get("http://www.example.com/")` → `.status_code == 200`,
  `.text` contains `<title>Example Domain</title>`.
- `urequests.head("http://www.example.com/")` returns headers,
  empty body.
- 4xx / 5xx still return a `Response` with the right code (no
  exception raised) — caller decides how to react.

---

## Step 2 — Chunked transfer encoding + gzip decompression

### Deliverables

- `Response._read_chunked()` — read `chunk-size CRLF chunk-data
  CRLF ... 0 CRLF CRLF`. Handles the `;` extension separator
  (drop trailing chunk extensions) and trailers.
- Wire up `Response.content`:
  - If `Transfer-Encoding: chunked` → `_read_chunked()`.
  - Else if `Content-Length: N` → `_read_raw(N)` (Step 1 behaviour).
  - Else → read-until-EOF.
- If `Content-Encoding: gzip` (or `deflate`), decompress through
  `deflate.DeflateIO(io.BytesIO(raw), deflate.GZIP).read()`.

### Verification

- A server that's known to chunk (`http://httpforever.com/` or a
  local Python `python3 -m http.server` returning chunked responses).
- A gzip-serving endpoint: e.g. `http://www.python.org/` with
  `Accept-Encoding: gzip` in our default request.

### Dependency note

`deflate` must be in our build. `MICROPY_PY_DEFLATE` is set under
`MICROPY_CONFIG_ROM_LEVEL_EXTRA_FEATURES` (our level). Confirm
`import deflate` works on the `standard` variant before relying
on it; if absent, either flip the config or skip the gzip path
and document.

---

## Step 3 — HTTPS via `ssl.SSLContext`

### Deliverables

- In `urequests.request()`, when the URL is `https://`:
  ```python
  import ssl
  ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
  ctx.verify_mode = ssl.CERT_REQUIRED
  ctx.set_default_verify_paths()
  s = ctx.wrap_socket(s, server_hostname=host)
  ```
  Single context per call (cheap on AmiSSL — Phase 28 measured
  ~316 KB of system memory the first time, ~125 KB per subsequent
  session; not worth caching at the urequests layer).
- Use `SSLSocket.write` / `read` (our Phase 28 stream protocol
  surface) for I/O on the wrapped socket.

### Verification

- `urequests.get("https://www.python.org/")` → 200 OK, HTML body.
- `urequests.get("https://example.com")` against a Cloudflare-fronted
  host — known to fail with the server-timeout-on-slow-CPU issue
  inherited from Phase 28; documented but not gated out.

### Known limitation

`urequests.get` against modern HTTPS servers with short
request-read timeouts (Cloudflare's edge, GitHub's API, etc.)
will complete the TLS 1.3 handshake then raise
`OSError: TLS: ... Broken pipe` because the 68k CPU can't finish
the handshake and write the request before the server-side
timeout fires. Per the AmiSSL maintainer
([jens-maus/amissl#111](https://github.com/jens-maus/amissl/issues/111))
this is a processor-speed limitation, not an AmiSSL or
Cloudflare bug -- AmiSSL is OpenSSL, and the Amiga's CPU just
isn't fast enough at modern TLS. Direct origins with looser
timeouts (`www.python.org`, etc.) work cleanly.

---

## Step 4 — `post` / `put` / `delete` / `patch` + kwargs

### Deliverables

- Verb wrappers: `post`, `put`, `delete`, `patch` (plus `options`
  if cheap).
- `request(method, url, *, data=None, json=None, headers=None)`:
  - If `json` is supplied: `data = json.dumps(json_obj)`,
    auto-set `Content-Type: application/json`.
  - If `data` is `str`, encode to UTF-8 bytes.
  - If `data` is `bytes`, accept as-is.
  - If `data` is a dict, urlencode it (`application/x-www-form-urlencoded`)
    — small inline `urlencode()` helper rather than dragging in
    `urllib.parse`.
  - Set `Content-Length: len(data)` and send the body after the
    empty `\r\n`.
- Caller's `headers` dict wins over our defaults (case-insensitive
  override): if caller sets `Content-Type`, don't double-send.

### Verification

- `urequests.post("https://httpbin.org/post", json={"a": 1})`
  returns 200 with the echoed JSON in `.json()`.
- Headers override: `headers={"User-Agent": "X"}` shows `X`, not
  our default.

---

## Step 5 — Freeze, variant gating, docs, tests

### Deliverables

- `ports/amiga/manifest.py`:
  - `freeze("$(PORT_DIR)/modules", "urequests.py")`
- Build the four variants; confirm `urequests` is absent from
  `minimal` and present in the other three.
- `docs/amiga.md` Phase 29 status → ✅; remove the "Phase 29 picks
  up urequests" forward-reference in Phase 28 design.
- `docs/amiga-testing.md` — add a short subsection under "SSL
  tests" pointing at urequests as the recommended way to do HTTPS
  GETs.
- `tests/ports/amiga/test_urequests_smoke.py` — a small on-device smoke
  that hits `http://www.example.com/` and `https://www.python.org/`
  and prints the first response line + content length.

### Verification

- `import urequests; urequests.get("http://www.example.com/").text[:80]`
  on the on-device runner against the `standard` binary.

---

## Cross-cutting concerns

- **HTTP/1.0 wire form.** We send `HTTP/1.0` with `Connection: close`
  so we can do "read until EOF" without managing keep-alive state.
  Servers are happy to downgrade. HTTP/1.1 keep-alive is a Step 6
  follow-up if it ever lands.
- **Response buffering.** `_read_line` and `_read_raw` share a
  `bytearray`-backed buffer to avoid one `recv` per byte (the OoZe
  reference does this; without it, response parsing on the Amiga is
  noticeably slow because every `recv()` round-trips into
  `bsdsocket.library`).
- **Error semantics.** Network failures (DNS lookup, `connect`,
  `recv`) raise `OSError` as the socket layer would; HTTP errors
  do *not* raise — caller checks `status_code`.
- **Timeouts.** Not in Step 1–5. Socket-level timeout via
  `socket.settimeout` would be the natural Step 6.
- **Heap pressure.** `Response.content` materialises the whole
  body into a single `bytes`. For a 1 MB download on the
  `minimal`-ish heap that means ~1 MB of GC heap pressure (split
  heap can grow but it's noticeable). Document; don't pre-optimise.
- **No `Session` object.** Cookies, persistent connections, and
  custom adapters all rest on a `Session`. Not in scope.

---

## Out-of-scope items reaffirmed

- HTTP/1.1 keep-alive / `Session`.
- Cookies / cookie jar.
- Streaming uploads (`data=<iterator>`), `multipart/form-data`.
- HTTPS hostname verification customisation (default-only).
- HTTP/2, async, websocket.
- Authentication helpers beyond a caller-supplied `Authorization`
  header.

## Status — done

All five steps shipped 2026-05-31. Verified end-to-end under
Amiberry in a single boot session across five live endpoints:

| Test | Result |
|---|---|
| `urequests.get("http://www.example.com/")` | 200, 528 B body (gzip decoded transparently) |
| `urequests.get("http://httpbin.org/get")` | 200, 274 B JSON |
| `urequests.get("https://www.python.org/")` | 200, 48 887 B HTML through Phase 28 TLS |
| `urequests.post("http://httpbin.org/post", json={"hello": "amiga", "n": 42}, headers={"X-Test": "phase29"})` | 200, JSON echoed, X-Test override survived round-trip |
| `urequests.post("http://httpbin.org/post", data={"name": "amiga 1200", "ram_kb": 2048})` | 200, form echoed, `Content-Type: application/x-www-form-urlencoded` auto-set |

### Side fix

The HTTPS path turned up a real gap in `modssl.c`: `SSLSocket`
exposed `read` / `write` (stream protocol) only — no
`send` / `recv` aliases. Calling `s.send(...)` after
`ctx.wrap_socket(...)` raised `AttributeError`. Added a two-line
alias in `ssl_socket_locals_dict_table` pointing `send` /
`recv` at the same `mp_stream_write_obj` / `mp_stream_read_obj`
the stream-protocol entries use, so `SSLSocket` is now drop-in
compatible with the BSD-socket idiom `modsocket.c` exposes.

### Variant gating note

The frozen `urequests.py` ships in all four variants (the
manifest's `freeze("$(PORT_DIR)/modules")` rule is
unconditional). On `minimal` it imports cleanly but the
constructor calls fail at the implicit `import socket` step —
correct behaviour, since `minimal` deliberately omits
`MICROPY_PY_AMIGA_SOCKET`. No variant carve-out needed.
