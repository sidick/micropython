# Phase 28 — AmiSSL v5 step plan

Companion to the Phase 28 design write-up in
[docs/amiga.md](amiga.md#phase-28--tlsssl-via-amissl-v5-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable,
individually-testable PRs.

Phase 28 lands behind a build flag (`MICROPY_PY_AMIGA_SSL`,
defaulting to ON for the variants that already include
`MICROPY_PY_AMIGA_SOCKET`), so partial progress doesn't disturb
the variants that are currently green.

## Phasing overview

```
Step 1: SDK in build env  →  Step 2: library lifecycle  →  Step 3: SSLContext
                                       │                          │
                                       ↓                          ↓
                                Step 5: variants/heap    Step 4: SSLSocket
                                       └─────────┬────────────────┘
                                                 ↓
                                       Step 6: docs + tests
```

| # | Step | Output | On-target smoke test |
|---|------|--------|---------------------|
| **1** | SDK fetched on every build | Headers `<openssl/*>` + `libamisslstubs.a` reachable from `m68k-amigaos-gcc` inside the bebbo container | 1-line C link test inside the container |
| **2** | `ports/amiga/amiga_ssl.{c,h}` — library lifecycle | `OpenLibrary("amisslmaster.library")` + `OpenAmiSSLTags(AMISSL_CURRENT_VERSION, ...)` filling `AmiSSLBase` / `AmiSSLExtBase`. Wired into `main.c` after `amiga_socket_open()`. Silent fallback if `amisslmaster.library` missing | Build + boot under Amiberry, observe no crash on startup/shutdown |
| **3** | `ports/amiga/modssl.c` — `ssl` module + `SSLContext` | `PROTOCOL_TLS_CLIENT`, `CERT_REQUIRED`, `CERT_NONE`; `SSLContext.__init__/__del__` (`SSL_CTX_new(TLS_client_method())` + `SSL_CTX_free`); `verify_mode` property; `load_verify_locations(cafile=…)` | `import ssl; ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)` |
| **4** | `SSLSocket` + `wrap_socket` | New type wrapping `SSL *` + fd. `SSL_new` → `SSL_set_fd` → `SSL_set_tlsext_host_name` (SNI) → `SSL_connect`. Stream protocol: `SSL_read`/`SSL_write` with `SSL_ERROR_WANT_READ/WRITE` → `MP_EAGAIN`. Close via `SSL_shutdown` + `SSL_free` | End-to-end HTTPS GET against an external host |
| **5** | Variant + heap reality check | Confirm `minimal` excludes SSL (no socket → no SSL). Decide heap policy for `standard` given `SSL_CTX` ~16 KB + `SSL` ~48 KB allocations from system memory | Two-context allocation under default heap; document `-X heap=` if needed |
| **6** | Docs + test wiring | Flip Phase 28 to ✅ in docs/amiga.md. Add SSL section to docs/amiga-testing.md. Run upstream `tests/extmod/ssl*` against our binary on-device | `tests/extmod/ssl_basic.py` via `tools/amiga-runtests.py` |

Each step is ~150–400 LOC and lands as its own PR.

---

## Step 1 — SDK fetched on every build

Per [docs/amiga.md SDK provisioning](amiga.md#sdk-provisioning-ci--container-build),
option 2 (workflow step + local script) is the chosen route. Vendor
nothing; rely on the upstream release artefact.

### Deliverables

- `tools/amiga-fetch-amissl-sdk.sh` — downloads the AmiSSL SDK archive
  from <https://github.com/jens-maus/amissl/releases> (pinned version,
  checksum-verified), unpacks the headers + stubs into a host-side
  cache directory (e.g. `~/.cache/amissl-sdk/<version>/`), and
  publishes the path to a stable symlink.
- `tools/amiga-build.sh` — call the fetcher before launching the
  container, bind-mount the cache as a second volume at
  `/sdk`, pass `EXTRA_CFLAGS=-I/sdk/include EXTRA_LDFLAGS="-L/sdk/lib"`
  into the make invocation.
- `ports/amiga/Makefile` — pick up `EXTRA_CFLAGS`/`EXTRA_LDFLAGS`
  (already standard MicroPython make variables, no change likely
  needed beyond confirming they're honoured).
- `.github/workflows/ports_amiga.yml` — add a step before the build
  that runs `tools/amiga-fetch-amissl-sdk.sh` and mounts the cache
  into the build container the same way `amiga-build.sh` does.
- Pinned version chosen — propose `AmiSSL 5.20`, the most recent
  stable at the time of writing; bump deliberately.

### Verification

Inside the container, after the fetch step:

```sh
m68k-amigaos-gcc -I/sdk/include -L/sdk/lib \
    -xc - -lamisslstubs <<'EOF'
#include <openssl/ssl.h>
int main(void) { return SSL_library_init(); }
EOF
```

Should produce an AmigaOS HUNK executable (we don't run it; linking
is the test).

### Notes

- ~10–30 s added to each clean CI run; cached across runs via
  `actions/cache` keyed on SDK version.
- `tools/amiga-build.sh clean` should *not* purge the SDK cache —
  it's not a build artefact, it's a toolchain artefact.

---

## Step 2 — Library lifecycle

### Deliverables

- `ports/amiga/amiga_ssl.h` — externs for `AmiSSLBase`, `AmiSSLExtBase`,
  `AmiSSLMasterBase`; prototypes for `amiga_ssl_open()` /
  `amiga_ssl_close()`; `AMISSL_CB` macro for callback annotations
  (`STDARGS SAVEDS`-equivalent for bebbo).
- `ports/amiga/amiga_ssl.c`:
  - `amiga_ssl_open()` — `OpenLibrary("amisslmaster.library",
    AMISSLMASTER_MIN_VERSION)`, then `OpenAmiSSLTags(AMISSL_CURRENT_VERSION,
    AmiSSL_UsesOpenSSLStructs, FALSE, AmiSSL_GetAmiSSLBase, &AmiSSLBase,
    AmiSSL_GetAmiSSLExtBase, &AmiSSLExtBase, AmiSSL_SocketBase,
    (ULONG)SocketBase, AmiSSL_ErrNoPtr, (ULONG)&errno, TAG_DONE)`.
    Silent (no error) if `amisslmaster.library` not present.
  - `amiga_ssl_close()` — `CloseAmiSSL()` if opened, then `CloseLibrary`.
- `ports/amiga/main.c` — call `amiga_ssl_open()` after
  `amiga_socket_open()`, `amiga_ssl_close()` before
  `amiga_socket_close()` (reverse order: AmiSSL → socket → dos).
- `ports/amiga/mpconfigport.h` —
  `MICROPY_PY_AMIGA_SSL (MICROPY_PY_AMIGA_SOCKET)` so it autodisables
  on the `minimal` variant.
- `ports/amiga/Makefile` — add `amiga_ssl.c` to `SRC_C` (or `SRC_QSTR`
  if any qstrs land in Step 3), link `-lamisslstubs` when
  `MICROPY_PY_AMIGA_SSL=1`.

### Verification

- Build `standard` variant. Verify text segment grows by the
  expected ~5–10 KB (mostly stub trampolines).
- Boot under Amiberry (AmiSSL already installed) — no startup error,
  no shutdown error. Add a `print(amiga.ssl_base())` debug helper
  temporarily if needed and confirm it returns non-zero.
- Boot under vamos — silent fallback path (vamos has no
  `amisslmaster.library`), confirm no crash and no warning.

---

## Step 3 — `SSLContext`

### Deliverables

- `ports/amiga/modssl.c` — new file, registers `ssl` module.
- Constants: `PROTOCOL_TLS_CLIENT`, `CERT_NONE`, `CERT_OPTIONAL`,
  `CERT_REQUIRED`.
- `SSLContext` type:
  - `__init__(protocol)` — `SSL_CTX_new(TLS_client_method())`; raise
    `OSError` with `ERR_error_string` text on failure.
  - `verify_mode` property (getter + setter calling
    `SSL_CTX_set_verify`).
  - `load_verify_locations(cafile=None, cadata=None)` —
    `SSL_CTX_load_verify_locations` or `SSL_CTX_load_verify_dir` as
    appropriate.
  - `close()` + `__del__` finaliser — `SSL_CTX_free`, then mark
    base as closed. Tolerate double-close.
- If `amiga_ssl_open()` left the base NULL → `import ssl` raises
  `ImportError("amisslmaster.library not available")`.

### Verification

Under Amiberry:

```python
import ssl
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.verify_mode = ssl.CERT_REQUIRED
ctx.load_verify_locations(capath="AmiSSL:certs/")
del ctx     # GC finalises, SSL_CTX_free runs
```

No crash, no leak (run `amiga.heap_info()` before/after a loop of
constructing + finalising 10 contexts to confirm release).

---

## Step 4 — `SSLSocket` + `wrap_socket`

### Deliverables

- `SSLSocket` type in `modssl.c`, wrapping `SSL *` + the underlying
  socket fd:
  - `SSLContext.wrap_socket(sock, server_hostname=...)` →
    `SSL_new(ctx)` → `SSL_set_fd(ssl, fd)` →
    `SSL_set_tlsext_host_name(ssl, server_hostname)` (SNI) →
    `SSL_connect(ssl)` looping on `WANT_READ/WANT_WRITE`.
  - Stream protocol (`mp_stream_p_t`):
    - `read` → `SSL_read`; `SSL_ERROR_WANT_READ`/`WANT_WRITE` →
      set `errcode = MP_EAGAIN`, return `MP_STREAM_ERROR`.
    - `write` → `SSL_write`; same EAGAIN handling.
    - `ioctl(close)` → `SSL_shutdown` (one round-trip), then
      `SSL_free`.
- Error mapping: terminal `SSL_get_error` values → `OSError` with
  `ERR_error_string` text.

### Verification

Under Amiberry with network:

```python
import socket, ssl

s = socket.socket()
s.connect(socket.getaddrinfo("www.python.org", 443)[0][-1])
ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_CLIENT)
ctx.verify_mode = ssl.CERT_REQUIRED
ctx.set_default_verify_paths()
ws = ctx.wrap_socket(s, server_hostname="www.python.org")
ws.write(b"GET / HTTP/1.0\r\nHost: www.python.org\r\n\r\n")
print(ws.read(2048)[:80])
ws.close()
```

Should print the start of an HTTP/1.0 response — verified on
2026-05-31 with `HTTP/1.1 200 OK` followed by ~54 KB of HTML.

### Status — verified

End-to-end HTTPS GET against `www.python.org` succeeds under
Amiberry with full `CERT_REQUIRED` chain validation: TCP connect,
TLS handshake + verify, write 104-byte request, read 53 912 bytes
of HTML, clean shutdown. The pure-protocol pipeline works.

### Known limitation — TLS 1.3 against modern CDNs

Cloudflare-fronted (`www.example.com`) and GitHub
(`api.github.com`) endpoints complete the TLS 1.3 handshake
(`CERT_REQUIRED` validation passes — the chain is verified, with
all four certs `verify return:1`) but the connection then dies
before any application data can be written: OpenSSL reports
"CONNECTION CLOSED BY SERVER" followed by
`tls_retry_write_records:Broken pipe`
(`ssl/record/methods/tls_common.c:1949`). The underlying socket
recv returns errno 54 (`ECONNRESET`).

**Confirmed reproducible with AmiSSL's own `openssl s_client`** —
this is not in our MicroPython wrapper. From an AmigaShell:

```
1> AmiSSL:OpenSSL s_client -connect www.example.com:443 \
       -servername www.example.com -CApath AmiSSL:certs -brief
... TLS 1.3 handshake completes, Verification: OK ...
CONNECTION CLOSED BY SERVER
tls_retry_write_records:Broken pipe ... tls_common.c:1949
```

The same trace appears against `api.github.com`. Workarounds
attempted via `s_client`:

| Attempt | Result |
|---|---|
| `-groups X25519` (classic, no MLKEM) | same close-after-handshake |
| `-no_ticket` (disable session tickets) | same close-after-handshake |
| `-tls1_2` (force TLS 1.2) | server rejects ClientHello: `unexpected eof while reading` |

`www.python.org` (a direct origin that still negotiates TLS 1.2
or TLS 1.3 without strict-fronting heuristics) handshakes,
writes, and reads cleanly from both our wrapper and `s_client`,
which is what the Step 4 end-to-end verification used.

The pattern points to **AmiSSL's TLS 1.3 ClientHello / handshake
producing a fingerprint that modern fronts choose not to talk to
post-handshake.** It is below the MicroPython layer; we have no
visible knob that helps. Reported upstream; not a Phase 28
blocker.

### Cert path gotcha

`SSL_CTX_load_verify_locations(capath=...)` is sensitive to a
trailing slash on AmigaOS: `"AmiSSL:certs/"` fails (path gets
concatenated to `"AmiSSL:certs//<hash>.0"` which the AmigaDOS
handler interprets as parent-dir-reference and ends up nowhere),
`"AmiSSL:certs"` works. `set_default_verify_paths()` sidesteps
the issue entirely and is the recommended call.

Also: AmiSSL's c_rehash output uses the *old* (pre-1.0.0) subject
hash algorithm. New-hash filenames silently miss in lookups even
though byte-identical files exist under their old-hash names in
the same directory. Use `set_default_verify_paths()` and let
AmiSSL handle this internally.

---

## Step 5 — Variants + heap reality check

### Deliverables

- Confirm `MICROPY_PY_AMIGA_SSL` correctly autodisables on `minimal`
  (no socket → no SSL).
- Measure actual heap pressure of:
  - 1 × `SSLContext` + 1 × `SSLSocket` connected idle.
  - 2 × concurrent `SSLSocket`s mid-handshake.
- Decision matrix:
  - If a single connection comfortably fits in the existing 256 KB
    standard heap, leave heap defaults alone, document the cliff.
  - If not, bump `standard` default heap to 384 or 512 KB.
- Update `docs/amiga.md` Phase 27 build-variants table with any
  text-segment growth.

### Verification

- `amiga.heap_info()` snapshots around handshake.
- `AvailMem(MEMF_ANY|MEMF_LARGEST)` snapshot to size system-memory
  vs GC-heap consumption.

---

## Step 6 — Docs + test wiring

### Deliverables

- `docs/amiga.md` Phase 28 status → ✅; add link to this plan
  with `(see [step plan](phase28-ssl-plan.md))`.
- `docs/amiga-testing.md` — new "Step 4 — SSL tests" subsection
  under the Amiberry runner: requires AmiSSL v5 installed
  (`amisslmaster.library` reachable via LIBS:, e.g. via
  `Assign LIBS: AmiSSL:Libs ADD`; CA dir at `AmiSSL:certs/`),
  not exercisable under vamos.
- `tools/amiga-runtests.py` — confirm `tests/extmod/ssl*.py` aren't
  in any `_SKIP_*` list and run cleanly.
- Possibly: a small host-side TLS echo server (Python `ssl.wrap_socket`)
  so the on-device runner has a deterministic endpoint that doesn't
  depend on the external internet.

### Verification

`tests/extmod/ssl_basic.py` + any other applicable upstream tests
pass via the on-device runner under Amiberry.

---

## Cross-cutting concerns

- **Cleanup ordering.** AmiSSL holds `SocketBase`; closing
  `bsdsocket.library` before AmiSSL would leak or double-fault.
  `main.c` must shut down in reverse open order: AmiSSL → socket → dos.
- **`STDARGS SAVEDS` callbacks.** Phase 28 punts custom callback
  thunks. Step 4 stays on `SSL_VERIFY_PEER` with OpenSSL's default
  verify callback, which doesn't need user code. If a future step
  exposes `set_verify_callback` we'll need the bebbo equivalent
  (`__attribute__((stkparm)) __saveds`) — verify at that point that
  bebbo's calling convention attributes still work as Phase 28
  documents.
- **CA bundle distribution.** Don't ship one in the binary. Rely on
  `AmiSSL:certs/` (the c_rehash-style dir of hashed CAs the AmiSSL installer
  drops there) for the default trust store.
- **AmiSSL v4 fallback.** Out of scope. v5-only initially.
- **Async-friendly handshake.** Tied to asyncio gating
  (`MICROPY_PY_ASYNCIO (0)` today). Out of scope.
- **`urequests` / `mip`.** Once Step 4 lands, these become trivial
  to freeze into the `standard` variant manifest. Separate PR.

---

## Out-of-scope items reaffirmed

The "Deliberately not doing" list from Phase 28 stands:

- Server-side TLS.
- OpenSSL config-file integration.
- Asyncio-friendly handshake.
- Touching upstream `extmod/modssl_*.c`.

If any of those become relevant later, they get their own
follow-up issue.
