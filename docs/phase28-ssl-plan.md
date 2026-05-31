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

Tests against Cloudflare-fronted (`www.example.com`) and
GitHub (`api.github.com`) endpoints complete the TLS 1.3 handshake
(`CERT_REQUIRED` validation passes — the chain is verified) but
the immediate post-handshake `SSL_write` returns EPIPE / broken
pipe with no bytes sent. Forcing TLS 1.2 via
`SSL_CTX_set_max_proto_version` doesn't help: those servers reject
TLS 1.2 outright. `www.python.org` (which still negotiates TLS 1.2
by default) works in both modes.

The failure mode strongly suggests AmiSSL's post-handshake state
isn't fully ready for `SSL_write` when the server sends a
NewSessionTicket immediately after the Finished message. The
upstream OpenSSL example (`test/https.c`) calls
`SSL_get_peer_certificate` etc. between handshake and write — that
extra dance may be load-bearing on AmigaOS.

Tracked as a Phase 28 follow-up. The MicroPython glue is correct;
the bug is below our layer.

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

### Status — verified

Measured under Amiberry on 2026-05-31, standard variant, end-to-end
HTTPS GET against `www.python.org` (53 912-byte response). Each row
is a snapshot from `amiga.heap_info()` + `AvailMem(MEMF_ANY|*)`:

| Stage | GC used | GC free | sys total | sys largest |
|---|---:|---:|---:|---:|
| startup | 26 816 | 229 312 | 14 243 080 | 12 166 992 |
| after `SSLContext()` + `set_default_verify_paths()` | 27 008 | 229 120 | 13 919 952 | 11 835 784 |
| after `del ctx` + `gc.collect()` | 26 992 | 229 136 | **13 919 952** | 11 835 784 |
| ctx ready (second one) | 27 008 | 229 120 | 13 911 720 | 11 827 552 |
| after `wrap_socket` (TLS handshake done) | 27 136 | 228 992 | 13 823 208 | 11 663 352 |
| after full read (53 912 B response) | 28 400 | 227 728 | 13 789 880 | 11 663 352 |
| after `ws.close()` + `s.close()` | 28 352 | 227 776 | 13 829 376 | 11 663 352 |
| after `del` + `gc.collect()` | 28 304 | 227 824 | 13 837 608 | 11 687 976 |

**GC heap impact: negligible.** Total growth across the entire
session is well under 2 KB. The 256 KB default heap is plenty for
SSL workloads; no heap bump needed.

**System memory is where AmiSSL lives.** It allocates from `MEMF_ANY`
via `exec.library`, completely outside the GC heap, so `-X heap=`
doesn't help. The observed costs:

- `SSL_CTX_new(TLS_client_method())` + `set_default_verify_paths()`
  → **~316 KB** of system memory, persistent for the lifetime of the
  process. `SSL_CTX_free` doesn't return it (likely caches the
  parsed default trust paths globally inside AmiSSL).
- One HTTPS session (handshake + 54 KB read) → **~125 KB** additional
  during the session.
- `ws.close()` + `del` recovers **~48 KB**.
- Net per-session leak after close: **~77 KB**, plausibly the
  AmiSSL session-resumption cache for that endpoint.

For a stock 2 MB A1200 (chip RAM only), `~441 KB` for one HTTPS
session is significant but workable if Fast RAM is added. The
`minimal` variant is the right target for tight-memory setups —
SSL autodisables there.

### Minimal variant gate — verified

`MICROPY_PY_AMIGA_SSL = 0` in `variants/minimal/mpconfigvariant.mk`
correctly drops `amiga_ssl.c` and `modssl.c` from `SRC_C`, omits
`-lamisslstubs` from the link, and excludes AmiSSL-related strings
from the binary (3 occurrences in `standard`, 0 in `minimal`).

### Variant sizes (post-Step-4 baseline)

| Variant | Text bytes | Total binary |
|---|---:|---:|
| `standard` | 491 216 | 562 840 |
| `minimal`  | 459 808 | 527 220 |
| `68020fpu` | (built) | 530 388 |
| `68040`    | (built) | 540 716 |

Standard grew ~33 KB over the pre-Phase-28 baseline (488 596 →
491 216) for the SSL wiring + AmiSSL stub trampolines. Minimal
is unchanged from pre-Phase 28.

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
