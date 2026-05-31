# Draft issue for the AmiSSL repo

To be filed at <https://github.com/jens-maus/amissl/issues>. This file
exists to keep the diagnostic + draft alongside the rest of the
Phase 28 paper trail; once filed, link the GitHub issue number here.

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

### Impact

Modern HTTPS clients on AmigaOS can't reach the large fraction of
the public web sitting behind Cloudflare/GitHub/etc. Affects any
AmiSSL-based application (curl, browsers, scripted automation, this
MicroPython port) the same way.
