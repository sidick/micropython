# AmiSSL TLS 1.3 close-after-handshake against Cloudflare

Filed: <https://github.com/jens-maus/amissl/issues/111> (2026-05-31).

This file keeps the full diagnostic + reproduction recipe alongside
the rest of the Phase 28 paper trail so the work doesn't have to be
re-derived if the GitHub issue is closed or migrates. Mirror updates
to both places when material new evidence comes in.

---

**Title:** `openssl s_client` post-handshake `CONNECTION CLOSED BY
SERVER` + `tls_retry_write_records: Broken pipe` against
Cloudflare-fronted hosts

### Environment

- AmiSSL 5.27 (`amissl_v362.library`)
- AmigaOS 3.x under Amiberry, UAE bsdsocket.library emulation
  (lib v4.1, NegSize 0x12c — original BSD API, gethostbyname-based
  DNS, getaddrinfo unavailable)
- Date: 2026-05-31

### Symptom

A clean TLS 1.3 handshake against a Cloudflare-fronted host (e.g.
`www.example.com`) completes successfully with full cert
verification, then the server closes the connection before any
application data can be sent. The next `SSL_write` returns `EPIPE`
("Broken pipe") and `recv` returns errno 54 (`ECONNRESET`).

The same trace appears against `api.github.com`. Direct origins
that don't strict-front (e.g. `www.python.org`) work cleanly.

### Reproduction

From an AmigaShell on a machine with AmiSSL installed:

```
1> Echo "HEAD / HTTP/1.0*NHost: www.example.com*NConnection: close*N*N" >T:req.txt
1> AmiSSL:OpenSSL s_client -connect www.example.com:443 \
       -servername www.example.com -CApath AmiSSL:certs -brief <T:req.txt
```

### Actual output

```
Connecting to 104.20.23.154
CONNECTION ESTABLISHED
Protocol version: TLSv1.3
Ciphersuite: TLS_AES_256_GCM_SHA384
Peer certificate: CN = example.com
Hash used: SHA256
Signature type: ecdsa_secp256r1_sha256
Verification: OK
Negotiated TLS1.3 group: X25519MLKEM768
CONNECTION CLOSED BY SERVER
4000FFC8:error:80000020:system library:tls_retry_write_records:Broken pipe:../../openssl/ssl/record/methods/tls_common.c:1949:tls_retry_write_records failure
```

With `-verify 2` and full chain output, all four certs return
`verify return:1` and the handshake reports `SSL handshake has read
5051 bytes and written 1626 bytes`, `Verify return code: 0 (ok)`.

### Expected output

`HTTP/1.0 200 OK` (or the response Cloudflare normally serves to a
plain TLS 1.3 client without ALPN).

### Workarounds tried

| Flag | Result |
|---|---|
| `-groups X25519` (skip post-quantum MLKEM hybrid) | same close-after-handshake |
| `-no_ticket` (disable session tickets) | same close-after-handshake |
| `-tls1_2` (force TLS 1.2) | Cloudflare rejects ClientHello: `unexpected eof while reading` |

So this isn't MLKEM-specific, isn't session-ticket-related, and
dropping to TLS 1.2 isn't an option (Cloudflare has phased it out
for most edge configurations in 2026).

### Version bisect across AmiSSL v5

Pinning the `OpenAmiSSLTags()` `APIVersion` to each installed
v5 sub-version in turn (via the
`-X sslver=<N>` flag added to the MicroPython port for this
purpose) shows the failure is **consistent across the entire
available v5 line**:

| Library | `AMISSL_V*` | APIVersion | Result |
|---|---|---:|---|
| `amissl_v352.library` (v5.22, Aug 2025) | `AMISSL_V352` | 41 | handshake ok, write broken pipe |
| `amissl_v353.library` (v5.23, Sep 2025) | `AMISSL_V353` | 42 | handshake ok, write broken pipe |
| `amissl_v354.library` (v5.24, Sep 2025) | `AMISSL_V354` | 43 | handshake ok, write broken pipe |
| `amissl_v360.library` (v5.25, Oct 2025) | `AMISSL_V360` | 44 | handshake ok, write broken pipe |
| `amissl_v361.library` (v5.26, Jan 2026) | `AMISSL_V361` | 45 | handshake ok, write broken pipe |
| `amissl_v362.library` (v5.27, Apr 2026) | `AMISSL_V362` | 46 | same |

So the behaviour is **not a v5.27 regression** — it has been
present continuously across at least 8 months of v5 releases.
That points to a structural / fingerprint-shape issue in
AmiSSL's TLS 1.3 ClientHello (or its OpenSSL 3.x configuration),
not to a specific patch that broke things recently.

### What didn't fix it (tried in the wrapper)

| Attempt | Result |
|---|---|
| `SSL_CTX_set_alpn_protos(ctx, "http/1.1")` (advertise ALPN) | same close-after-handshake |
| Switching `amiga_ssl_open` to `OpenAmiSSL() + InitAmiSSL()` (v4 init API), keeping the rest of the wrapper unchanged | `SSL_connect` now fails earlier with `SSL_ERROR_SYSCALL` / `EIO` — the v4 init evidently doesn't bring along the OpenSSL 3.x provider state that v5 needs for TLS 1.3 |
| `SSL_CTX_set_mode(ctx, SSL_MODE_AUTO_RETRY)` (transparently retry on post-handshake control records) | same close-after-handshake |
| `SSL_CTX_set_ciphersuites(ctx, "TLS_CHACHA20_POLY1305_SHA256")` (force a different TLS 1.3 suite than the default `TLS_AES_256_GCM_SHA384`) | same close-after-handshake |
| Kitchen sink: `SSL_OP_ALL \| SSL_OP_NO_TICKET` + `SSL_CTX_set_min/max_proto_version(TLS1_3_VERSION)` + `SSL_CTX_set_session_cache_mode(SSL_SESS_CACHE_OFF)` | same close-after-handshake |
| `SSL_get_peer_certificate(ssl); X509_free(...);` between `SSL_connect` and `SSL_write` (match the canonical AmiSSL `test/https.c` "settling" sequence) | same close-after-handshake |
| Explicit BIO chain: `BIO_f_buffer` → `BIO_new_socket(fd)` on the write side via `SSL_set_bio`, replacing `SSL_set_fd` | `SSL_write` now reports success (61 bytes accepted into the buffer), but the next `SSL_read` returns `SYSCALL`/`EIO`. The buffer just defers the symptom — Cloudflare still closes, we just notice it later. Not a real fix. |
| Pass `AmiSSL_UsesOpenSSLStructs, TRUE` to `OpenAmiSSLTags()` instead of `FALSE` (request the actual OpenSSL struct layouts rather than opaque pointers) | same close-after-handshake |

The v4 init path produces a different *broken* state, suggesting
the divergence between the two init paths inside AmiSSL is
non-trivial — it's possible that v4 init enables an option that
v5 init doesn't (default ciphers, extension order, `SSL_OP_*`
flags, etc.) that the server happens to accept.

A useful next investigation would be: instrument both paths to
log the OpenSSL `SSL_CTX` options / cipher list / sigalgs after
init returns, and diff. Whatever differs is a good candidate for
the actual trigger.

### Working comparison

Same `s_client` invocation against `www.python.org:443` completes
handshake, writes the request, reads the response, and closes
cleanly. So TLS 1.3 itself is fine; what's specific is something
about AmiSSL's ClientHello/handshake that Cloudflare-class fronts
choose not to follow up on.

### Probable cause (speculative)

Cloudflare uses TLS client fingerprinting (JA3/JA4) and
aggressively closes connections from clients whose fingerprint
matches their bot/abuse heuristics. AmiSSL's ClientHello extension
order, advertised groups, or signature algorithms may produce a
fingerprint Cloudflare doesn't accept. Worth comparing the AmiSSL
ClientHello byte-for-byte against a modern desktop OpenSSL 3.x to
spot any divergence (extension ordering, omitted extensions, or
differing default lists).

Alternatively, the close could be in response to a specific
post-handshake message — a NewSessionTicket the client doesn't ACK
properly, a KeyUpdate, or a handshake completion alert AmiSSL
doesn't emit in the way modern OpenSSL does.
