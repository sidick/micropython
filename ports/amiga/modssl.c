/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Simon Dick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

// ports/amiga/modssl.c -- Phase 28 Step 3: ssl module + SSLContext.
//
// Provides the upstream-shaped ssl.SSLContext API on top of AmiSSL v5.
// Step 3 lands the context object + verify-mode property +
// load_verify_locations. Step 4 will add SSLContext.wrap_socket and
// the SSLSocket stream type. No TLS handshake happens here yet.

#include "py/mpconfig.h"

#if MICROPY_PY_AMIGA_SSL

#include <stdbool.h>
#include <stddef.h>
#include <string.h>

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/bio.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/opensslv.h>
#include <proto/exec.h>
#include <dos/dosextens.h>

#include "py/runtime.h"
#include "py/objstr.h"
#include "py/stream.h"
#include "py/mperrno.h"

#include "amiga_ssl.h"

// User-visible constants -- numeric values match CPython's ssl.*.
#define MOD_PROTOCOL_TLS_CLIENT 0
#define MOD_PROTOCOL_TLS_SERVER 1
#define MOD_CERT_NONE     0
#define MOD_CERT_OPTIONAL 1
#define MOD_CERT_REQUIRED 2

typedef struct _mp_ssl_context_t {
    mp_obj_base_t base;
    SSL_CTX *ctx;
} mp_ssl_context_t;

typedef struct _mp_ssl_socket_t {
    mp_obj_base_t base;
    SSL *ssl;
    // net_bio is the application half of the BIO pair AmiSSL talks to;
    // freed and NULLed on close.
    BIO *net_bio;
    // Non-blocking poll state. poll_mask is the
    // cross-direction the last read/write needed (0 if it blocked in its
    // natural direction); ioctl(POLL) uses it to translate the caller's
    // requested direction. last_error latches a fatal TLS error so a
    // later poll reports NVAL instead of silently succeeding. out_buf
    // holds ciphertext drained from net_bio that the transport hasn't
    // accepted yet (so an EAGAIN mid-flush doesn't drop bytes).
    int poll_mask;
    int last_error;
    uint8_t *out_buf; // GC-managed; holds unflushed outgoing ciphertext
    size_t out_cap;
    size_t out_len;
    size_t out_off;
    mp_obj_t sock_obj; // pin the underlying socket against GC
} mp_ssl_socket_t;

static const mp_obj_type_t ssl_context_type;
static const mp_obj_type_t ssl_socket_type;

static mp_ssl_context_t *new_context(int protocol);
static void context_set_verify_mode(mp_ssl_context_t *self, int cert);
static void context_load_cadata(mp_ssl_context_t *self, mp_obj_t cadata_obj);

// ssl_socket_pump return codes. NEED_WR/NEED_RD mean the transport would
// block in that direction (the caller records it and reports EAGAIN); OK
// means forward progress was made, so the SSL operation should be retried.
#define SSL_PUMP_OK       (0)
#define SSL_PUMP_NEED_WR  (1)
#define SSL_PUMP_NEED_RD  (2)
#define SSL_PUMP_EOF      (-1)
#define SSL_PUMP_ERR      (-2)
static int ssl_socket_pump(mp_ssl_socket_t *self, bool want_read);
static mp_obj_t make_ssl_socket(mp_ssl_context_t *self, mp_obj_t sock,
    bool server_side, bool do_handshake, mp_obj_t server_hostname);

static void check_amissl(void) {
    if (AmiSSLBase == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("amisslmaster.library missing"));
    }
}

static MP_NORETURN void raise_ssl_error(void) {
    unsigned long e = ERR_get_error();
    if (e == 0) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("TLS error"));
    }
    char buf[128];
    ERR_error_string_n(e, buf, sizeof(buf));
    mp_raise_msg_varg(&mp_type_OSError,
        MP_ERROR_TEXT("TLS: %s"), buf);
}

// Resolve a captured SSL_get_error() code into an exception. Take
// the SSL_get_error and SSL_get_verify_result codes as values so
// callers can SSL_free the ssl pointer first if they want; reading
// them off a freed SSL is a use-after-free that silently returns
// X509_V_OK and hides the real reason.
static MP_NORETURN void raise_ssl_diag(int err, long verify) {
    if (verify != X509_V_OK) {
        // A failed cert chain shows up in the OpenSSL queue as the
        // generic "certificate verify failed" entry; the actual
        // reason (expired, missing issuer, hostname mismatch, etc.)
        // lives in SSL_get_verify_result. Surface that so callers
        // can act on it instead of staring at 0A000086.
        const char *why = X509_verify_cert_error_string(verify);
        if (!why) {
            why = "unknown";
        }
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("TLS verify (%ld): %s"), verify, why);
    }

    unsigned long e = ERR_get_error();
    if (e != 0) {
        char buf[128];
        ERR_error_string_n(e, buf, sizeof(buf));
        mp_raise_msg_varg(&mp_type_OSError,
            MP_ERROR_TEXT("TLS: %s"), buf);
    }
    if (err == SSL_ERROR_ZERO_RETURN) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("TLS: closed"));
    }
    if (err == SSL_ERROR_SYSCALL) {
        mp_raise_OSError(MP_EIO);
    }
    mp_raise_msg_varg(&mp_type_OSError,
        MP_ERROR_TEXT("TLS: SSL error %d"), err);
}

// Build an SSLContext wrapping a fresh SSL_CTX for the given protocol.
// Shared by the SSLContext constructor and the module-level wrap_socket
// shim. Callers must have already verified AmiSSL is open.
static mp_ssl_context_t *new_context(int protocol) {
    const SSL_METHOD *method;
    if (protocol == MOD_PROTOCOL_TLS_CLIENT) {
        method = TLS_client_method();
    } else if (protocol == MOD_PROTOCOL_TLS_SERVER) {
        method = TLS_server_method();
    } else {
        mp_raise_ValueError(MP_ERROR_TEXT("protocol"));
    }

    SSL_CTX *ctx = SSL_CTX_new(method);
    if (ctx == NULL) {
        raise_ssl_error();
    }

    // AmiSSL is built with OPENSSL_TLS_SECURITY_LEVEL 2, which imposes a
    // 2048-bit RSA / TLS-1.2 floor and rejects the small keys used across
    // the MicroPython test suite (and plenty of legacy Amiga-era certs).
    // mbedTLS and axtls -- the other MicroPython TLS backends -- impose no
    // such floor, so drop to level 0 to match their accept behaviour.
    // This only widens what we accept; verify_mode still governs whether a
    // peer chain is actually checked.
    SSL_CTX_set_security_level(ctx, 0);

    mp_ssl_context_t *self =
        mp_obj_malloc_with_finaliser(mp_ssl_context_t, &ssl_context_type);
    self->ctx = ctx;
    return self;
}

// SSLContext(protocol)
static mp_obj_t ssl_context_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw,
    const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    check_amissl();
    return MP_OBJ_FROM_PTR(new_context(mp_obj_get_int(args[0])));
}

static mp_obj_t ssl_context_close(mp_obj_t self_in) {
    mp_ssl_context_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ctx) {
        SSL_CTX_free(self->ctx);
        self->ctx = NULL;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssl_context_close_obj, ssl_context_close);

// load_verify_locations(cafile=None, capath=None)
//
// AmiSSL ships its CA bundle at LIBS:amissl/certs/cacert.pem; users
// typically pass that path here. cadata (PEM/DER as string/bytes) is
// out of scope for Step 3 -- needs SSL_CTX_load_verify_dir-on-memory
// glue we don't have yet.
static mp_obj_t ssl_context_load_verify_locations(
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_cafile, ARG_capath, ARG_cadata };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_cafile, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_capath, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cadata, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed_args)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed_args), allowed_args, args);

    mp_ssl_context_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    if (self->ctx == NULL) {
        mp_raise_OSError(MP_EBADF);
    }

    const char *cafile = NULL;
    const char *capath = NULL;
    if (args[ARG_cafile].u_obj != mp_const_none) {
        cafile = mp_obj_str_get_str(args[ARG_cafile].u_obj);
    }
    if (args[ARG_capath].u_obj != mp_const_none) {
        capath = mp_obj_str_get_str(args[ARG_capath].u_obj);
    }
    if (cafile == NULL && capath == NULL
        && args[ARG_cadata].u_obj == mp_const_none) {
        mp_raise_TypeError(
            MP_ERROR_TEXT("cafile, capath or cadata required"));
    }

    // In-memory CA blob -- no filesystem access, so no requester risk.
    if (args[ARG_cadata].u_obj != mp_const_none) {
        context_load_cadata(self, args[ARG_cadata].u_obj);
    }
    if (cafile == NULL && capath == NULL) {
        return mp_const_none;
    }

    // OpenSSL's load_verify_locations calls fopen / opendir internally;
    // on AmigaOS that pops the "Please insert volume X:" requester for
    // any path whose volume isn't mounted. Suppress the requester for
    // the duration of the call, matching the pattern in vfs_amiga.c.
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    int rc = SSL_CTX_load_verify_locations(self->ctx, cafile, capath);
    me->pr_WindowPtr = saved_wp;

    if (rc != 1) {
        raise_ssl_error();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_KW(
    ssl_context_load_verify_locations_obj, 1,
    ssl_context_load_verify_locations);

static mp_obj_t ssl_context_set_default_verify_paths(mp_obj_t self_in) {
    mp_ssl_context_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ctx == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    if (SSL_CTX_set_default_verify_paths(self->ctx) != 1) {
        raise_ssl_error();
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(
    ssl_context_set_default_verify_paths_obj,
    ssl_context_set_default_verify_paths);

// Translate a CERT_* value into the OpenSSL SSL_VERIFY_* bitmask and
// apply it. Shared by the verify_mode attr setter and the module-level
// wrap_socket shim (which takes cert_reqs).
static void context_set_verify_mode(mp_ssl_context_t *self, int cert) {
    if (self->ctx == NULL) {
        return;
    }
    int mode;
    switch (cert) {
        case MOD_CERT_NONE:
            mode = SSL_VERIFY_NONE;
            break;
        case MOD_CERT_OPTIONAL:
            mode = SSL_VERIFY_PEER;
            break;
        case MOD_CERT_REQUIRED:
            mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
            break;
        default:
            mp_raise_ValueError(MP_ERROR_TEXT("verify_mode"));
    }
    SSL_CTX_set_verify(self->ctx, mode, NULL);
}

// Load CA certificate(s) from an in-memory blob into the context's
// trust store -- the cadata path of CPython's load_verify_locations.
// Accepts a single DER cert or a PEM blob; bad data raises
// ValueError("invalid cert"), matching tests/extmod/ssl_cadata.py.
static void context_load_cadata(mp_ssl_context_t *self, mp_obj_t cadata_obj) {
    if (self->ctx == NULL) {
        mp_raise_OSError(MP_EBADF);
    }
    mp_buffer_info_t bi;
    mp_get_buffer_raise(cadata_obj, &bi, MP_BUFFER_READ);

    // Try DER (a single cert) first, then fall back to PEM.
    X509 *cert = NULL;
    BIO *bio = BIO_new_mem_buf(bi.buf, (int)bi.len);
    if (bio != NULL) {
        cert = d2i_X509_bio(bio, NULL);
        BIO_free(bio);
    }
    if (cert == NULL) {
        bio = BIO_new_mem_buf(bi.buf, (int)bi.len);
        if (bio != NULL) {
            cert = PEM_read_bio_X509(bio, NULL, NULL, NULL);
            BIO_free(bio);
        }
    }
    if (cert == NULL) {
        ERR_clear_error();
        mp_raise_ValueError(MP_ERROR_TEXT("invalid cert"));
    }

    // X509_STORE_add_cert takes its own reference, so drop ours.
    X509_STORE_add_cert(SSL_CTX_get_cert_store(self->ctx), cert);
    X509_free(cert);
}

// SSLContext.load_cert_chain(cert, key) -- cert and key are DER (ASN.1)
// blobs. The error contract is pinned by tests/extmod/ssl_keycert.py:
// the key is validated first (bad key -> ValueError("invalid key")),
// then the cert (None -> TypeError from the buffer fetch, bad cert ->
// ValueError("invalid cert")).
static mp_obj_t ssl_context_load_cert_chain(mp_obj_t self_in,
    mp_obj_t cert, mp_obj_t key) {
    mp_ssl_context_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ctx == NULL) {
        mp_raise_OSError(MP_EBADF);
    }

    mp_buffer_info_t kb;
    mp_get_buffer_raise(key, &kb, MP_BUFFER_READ); // None -> TypeError
    if (SSL_CTX_use_PrivateKey_ASN1(EVP_PKEY_RSA, self->ctx,
        kb.buf, (long)kb.len) != 1) {
        ERR_clear_error();
        mp_raise_ValueError(MP_ERROR_TEXT("invalid key"));
    }

    mp_buffer_info_t cb;
    mp_get_buffer_raise(cert, &cb, MP_BUFFER_READ); // None -> TypeError
    if (SSL_CTX_use_certificate_ASN1(self->ctx, (int)cb.len, cb.buf) != 1) {
        ERR_clear_error();
        mp_raise_ValueError(MP_ERROR_TEXT("invalid cert"));
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(
    ssl_context_load_cert_chain_obj, ssl_context_load_cert_chain);

// SSLContext.verify_mode -- expose via attr handler so we can
// translate the AmiSSL/OpenSSL SSL_VERIFY_* bitmask both ways.
static void ssl_context_attr(mp_obj_t self_in, qstr attr, mp_obj_t *dest) {
    mp_ssl_context_t *self = MP_OBJ_TO_PTR(self_in);
    if (dest[0] == MP_OBJ_NULL) {
        // load
        if (attr == MP_QSTR_verify_mode) {
            if (self->ctx == NULL) {
                return;
            }
            int mode = SSL_CTX_get_verify_mode(self->ctx);
            int cert;
            if (mode == 0) {
                cert = MOD_CERT_NONE;
            } else if (mode & SSL_VERIFY_FAIL_IF_NO_PEER_CERT) {
                cert = MOD_CERT_REQUIRED;
            } else {
                cert = MOD_CERT_OPTIONAL;
            }
            dest[0] = MP_OBJ_NEW_SMALL_INT(cert);
        } else {
            // delegate to locals_dict
            dest[1] = MP_OBJ_SENTINEL;
        }
    } else if (dest[0] == MP_OBJ_SENTINEL) {
        // store
        if (attr == MP_QSTR_verify_mode) {
            if (self->ctx == NULL) {
                return;
            }
            context_set_verify_mode(self, mp_obj_get_int(dest[1]));
            dest[0] = MP_OBJ_NULL; // success
        }
    }
}

// Build an SSLSocket wrapping `sock`. AmiSSL is always driven through a
// memory BIO pair (ssl_socket_pump shuttles ciphertext to/from the
// object's stream protocol), which works for every case: blocking
// clients (urequests), non-blocking clients (asyncio, do_handshake_on_
// connect=False), server-side sockets, and fileno-less stream objects
// (io.BytesIO, the extmod/ssl_*.py harness).
//
// An earlier revision had a second "fd path" that handed AmiSSL the raw
// descriptor via SSL_set_fd for blocking clients. It was dropped: its
// ioctl couldn't report poll readiness (so it was unusable from asyncio),
// and its single blocking SSL_connect was the more fragile of the two --
// it intermittently broke the pipe under the Amiga's slow handshakes,
// where the BIO pump (with proper EAGAIN handling) succeeds.
//
// We don't take ownership of `sock` -- the caller still owns its
// lifetime -- but we pin a reference so it can't be GC'd while the
// SSLSocket is alive.
static mp_obj_t make_ssl_socket(mp_ssl_context_t *self, mp_obj_t sock,
    bool server_side, bool do_handshake, mp_obj_t server_hostname) {
    if (self->ctx == NULL) {
        mp_raise_OSError(MP_EBADF);
    }

    const char *hostname = NULL;
    if (server_hostname != mp_const_none) {
        hostname = mp_obj_str_get_str(server_hostname);
    }

    // The object must implement the full stream protocol so the pump can
    // shuttle ciphertext through it.
    mp_get_stream_raise(sock,
        MP_STREAM_OP_READ | MP_STREAM_OP_WRITE | MP_STREAM_OP_IOCTL);

    SSL *ssl = SSL_new(self->ctx);
    if (ssl == NULL) {
        raise_ssl_error();
    }
    BIO *internal_bio = NULL, *net_bio = NULL;
    if (BIO_new_bio_pair(&internal_bio, 0, &net_bio, 0) != 1) {
        SSL_free(ssl);
        raise_ssl_error();
    }
    SSL_set_bio(ssl, internal_bio, internal_bio); // SSL owns internal_bio
    if (server_side) {
        SSL_set_accept_state(ssl);
    } else {
        SSL_set_connect_state(ssl);
        if (hostname) {
            SSL_set_tlsext_host_name(ssl, hostname);
        }
    }

    mp_ssl_socket_t *s = mp_obj_malloc_with_finaliser(
        mp_ssl_socket_t, &ssl_socket_type);
    s->ssl = ssl;
    s->net_bio = net_bio;
    s->poll_mask = 0;
    s->last_error = 0;
    s->out_buf = NULL;
    s->out_cap = 0;
    s->out_len = 0;
    s->out_off = 0;
    s->sock_obj = sock;

    if (do_handshake) {
        for (;;) {
            int rc = SSL_do_handshake(ssl);
            if (rc == 1) {
                break;
            }
            int err = SSL_get_error(ssl, rc);
            long verify = X509_V_OK;
            int hs_err = 0;
            if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
                int p = ssl_socket_pump(s, err == SSL_ERROR_WANT_READ);
                if (p == SSL_PUMP_OK) {
                    continue;
                }
                // Transport blocked (EAGAIN on a non-blocking socket) or
                // closed before the handshake finished.
                hs_err = (p == SSL_PUMP_NEED_RD || p == SSL_PUMP_NEED_WR)
                    ? MP_EAGAIN : MP_ECONNRESET;
            } else {
                // Terminal TLS error -- capture verify result before free.
                verify = SSL_get_verify_result(ssl);
            }
            SSL_free(ssl);
            BIO_free(net_bio);
            s->ssl = NULL;
            s->net_bio = NULL;
            s->sock_obj = mp_const_none;
            if (hs_err) {
                mp_raise_OSError(hs_err);
            }
            raise_ssl_diag(err, verify);
        }
    }
    return MP_OBJ_FROM_PTR(s);
}

// SSLContext.wrap_socket(sock, server_side=False, server_hostname=None,
//                        do_handshake_on_connect=True)
static mp_obj_t ssl_context_wrap_socket(
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sock, ARG_server_side, ARG_server_hostname, ARG_do_handshake };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sock, MP_ARG_OBJ | MP_ARG_REQUIRED, {0} },
        { MP_QSTR_server_side,
          MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_server_hostname,
          MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_do_handshake_on_connect,
          MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = true} },
    };
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args - 1, pos_args + 1, kw_args,
        MP_ARRAY_SIZE(allowed), allowed, args);

    mp_ssl_context_t *self = MP_OBJ_TO_PTR(pos_args[0]);
    return make_ssl_socket(self, args[ARG_sock].u_obj,
        args[ARG_server_side].u_bool, args[ARG_do_handshake].u_bool,
        args[ARG_server_hostname].u_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(
    ssl_context_wrap_socket_obj, 1, ssl_context_wrap_socket);

// Module-level ssl.wrap_socket(...) -- the legacy one-shot helper that
// micropython-lib's ssl.py provides on top of SSLContext. Mirrors that
// shim so the upstream extmod/ssl_*.py tests run unmodified.
static mp_obj_t mod_ssl_wrap_socket(
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum {
        ARG_sock, ARG_server_side, ARG_key, ARG_cert, ARG_cert_reqs,
        ARG_cadata, ARG_server_hostname, ARG_do_handshake
    };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sock, MP_ARG_OBJ | MP_ARG_REQUIRED, {0} },
        { MP_QSTR_server_side,
          MP_ARG_BOOL | MP_ARG_KW_ONLY, {.u_bool = false} },
        { MP_QSTR_key, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cert, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_cert_reqs, MP_ARG_INT | MP_ARG_KW_ONLY,
          {.u_int = MOD_CERT_NONE} },
        { MP_QSTR_cadata, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_server_hostname, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_do_handshake, MP_ARG_BOOL | MP_ARG_KW_ONLY,
          {.u_bool = true} },
    };
    // Module-level function: pos_args[0] is the sock argument itself,
    // not a bound self, so parse the whole positional vector.
    mp_arg_val_t args[MP_ARRAY_SIZE(allowed)];
    mp_arg_parse_all(n_args, pos_args, kw_args,
        MP_ARRAY_SIZE(allowed), allowed, args);

    check_amissl();
    bool server_side = args[ARG_server_side].u_bool;
    mp_ssl_context_t *ctx = new_context(
        server_side ? MOD_PROTOCOL_TLS_SERVER : MOD_PROTOCOL_TLS_CLIENT);

    mp_obj_t key = args[ARG_key].u_obj;
    mp_obj_t cert = args[ARG_cert].u_obj;
    if (mp_obj_is_true(cert) || mp_obj_is_true(key)) {
        ssl_context_load_cert_chain(MP_OBJ_FROM_PTR(ctx), cert, key);
    }
    if (args[ARG_cadata].u_obj != mp_const_none) {
        context_load_cadata(ctx, args[ARG_cadata].u_obj);
    }
    context_set_verify_mode(ctx, args[ARG_cert_reqs].u_int);

    return make_ssl_socket(ctx, args[ARG_sock].u_obj, server_side,
        args[ARG_do_handshake].u_bool, args[ARG_server_hostname].u_obj);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(mod_ssl_wrap_socket_obj, 1, mod_ssl_wrap_socket);

// SSLSocket -- stream protocol on top of SSL_read / SSL_write.

// Flush the stashed outgoing ciphertext (out_buf[out_off..out_len]) to the
// transport in a single write call, so a whole TLS flight reaches the peer
// as one record group -- callers that count writes (and the underlying
// socket's framing) depend on that. Returns an SSL_PUMP_* code.
static int ssl_flush_out(mp_ssl_socket_t *self, const mp_stream_p_t *stream) {
    while (self->out_off < self->out_len) {
        int errcode = 0;
        mp_uint_t w = stream->write(self->sock_obj,
            self->out_buf + self->out_off,
            (mp_uint_t)(self->out_len - self->out_off), &errcode);
        if (w == MP_STREAM_ERROR) {
            // Unsent tail stays put for the next pump; no bytes are lost.
            return mp_is_nonblocking_error(errcode)
                ? SSL_PUMP_NEED_WR : SSL_PUMP_ERR;
        }
        self->out_off += w;
    }
    self->out_len = self->out_off = 0;
    return SSL_PUMP_OK;
}

// Shuttle ciphertext between the network half of the BIO pair and the
// underlying Python stream (stream-path sockets only). A whole pending
// flight is drained into out_buf and written in one call; if the transport
// blocks the tail is retained for next time so no bytes are lost. When
// AmiSSL wants input, one chunk is read back and fed in. Runs in *our*
// frame, never inside an AmiSSL callback, so the Python stream methods it
// invokes may allocate, GC, or raise.
static int ssl_socket_pump(mp_ssl_socket_t *self, bool want_read) {
    const mp_stream_p_t *stream = mp_get_stream(self->sock_obj);

    // 1. Flush ciphertext left over from a previous blocked write.
    int rc = ssl_flush_out(self, stream);
    if (rc != SSL_PUMP_OK) {
        return rc;
    }

    // 2. Drain AmiSSL's entire pending output into one buffer, write once.
    size_t pending = BIO_ctrl_pending(self->net_bio);
    if (pending > 0) {
        if (self->out_cap < pending) {
            size_t cap = self->out_cap ? self->out_cap : 512;
            while (cap < pending) {
                cap *= 2;
            }
            self->out_buf = m_renew(uint8_t, self->out_buf, self->out_cap, cap);
            self->out_cap = cap;
        }
        size_t got = 0;
        while (got < pending) {
            int n = BIO_read(self->net_bio,
                self->out_buf + got, (int)(pending - got));
            if (n <= 0) {
                break;
            }
            got += (size_t)n;
        }
        self->out_len = got;
        self->out_off = 0;
        rc = ssl_flush_out(self, stream);
        if (rc != SSL_PUMP_OK) {
            return rc;
        }
    }

    // 3. Feed the peer's ciphertext in when AmiSSL is waiting to read.
    if (want_read) {
        uint8_t in_buf[512];
        int errcode = 0;
        mp_uint_t r = stream->read(self->sock_obj,
            in_buf, sizeof(in_buf), &errcode);
        if (r == MP_STREAM_ERROR) {
            return mp_is_nonblocking_error(errcode)
                ? SSL_PUMP_NEED_RD : SSL_PUMP_ERR;
        }
        if (r == 0) {
            return SSL_PUMP_EOF;
        }
        BIO_write(self->net_bio, in_buf, (int)r);
    }
    return SSL_PUMP_OK;
}

// If SSL_read / SSL_write fails, raise instead of silently returning
// EIO -- otherwise diagnosing TLS-layer failures from Python is just
// "OSError: EIO" with no clue what actually broke. WANT_READ/WRITE
// stay as MP_STREAM_ERROR + MP_EAGAIN since they're a normal part of
// non-blocking flow control.
static MP_NORETURN void raise_stream_ssl_error(SSL *ssl, int rc) {
    int err = SSL_get_error(ssl, rc);
    long verify = SSL_get_verify_result(ssl);
    raise_ssl_diag(err, verify);
}

// Map a pump result into the non-blocking stream contract. `cross` is the
// poll direction to record when the transport blocks opposite to the SSL
// op's natural direction (POLL_WR for a read, POLL_RD for a write); when it
// blocks in its natural direction poll_mask is left 0. Returns true if the
// caller should report the stored *errcode, false to retry the SSL op.
static bool ssl_pump_result(mp_ssl_socket_t *self, int p, int cross,
    int *errcode) {
    switch (p) {
        case SSL_PUMP_OK:
            return false; // progress -- retry
        case SSL_PUMP_NEED_WR:
            self->poll_mask = (cross == MP_STREAM_POLL_WR) ? cross : 0;
            *errcode = MP_EAGAIN;
            return true;
        case SSL_PUMP_NEED_RD:
            self->poll_mask = (cross == MP_STREAM_POLL_RD) ? cross : 0;
            *errcode = MP_EAGAIN;
            return true;
        default: // SSL_PUMP_EOF / SSL_PUMP_ERR
            self->last_error = MP_EIO;
            *errcode = MP_EIO;
            return true;
    }
}

static mp_uint_t ssl_socket_read(mp_obj_t self_in, void *buf,
    mp_uint_t size, int *errcode) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ssl == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }

    // Pump-driven, non-blocking aware (blocking transports just never
    // return EAGAIN, so the pump loops until the data arrives).
    self->poll_mask = 0;
    if (self->last_error != 0) {
        *errcode = self->last_error;
        return MP_STREAM_ERROR;
    }
    for (;;) {
        int rc = SSL_read(self->ssl, buf, (int)size);
        if (rc > 0) {
            return (mp_uint_t)rc;
        }
        int err = SSL_get_error(self->ssl, rc);
        if (err == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            self->last_error = MP_EIO;
            raise_stream_ssl_error(self->ssl, rc);
        }
        int p = ssl_socket_pump(self, err == SSL_ERROR_WANT_READ);
        if (p == SSL_PUMP_EOF) {
            return 0; // transport closed mid-read = EOF
        }
        if (ssl_pump_result(self, p, MP_STREAM_POLL_WR, errcode)) {
            return MP_STREAM_ERROR;
        }
    }
}

static mp_uint_t ssl_socket_write(mp_obj_t self_in, const void *buf,
    mp_uint_t size, int *errcode) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ssl == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }

    self->poll_mask = 0;
    if (self->last_error != 0) {
        *errcode = self->last_error;
        return MP_STREAM_ERROR;
    }
    for (;;) {
        int rc = SSL_write(self->ssl, buf, (int)size);
        if (rc > 0) {
            // SSL_write only queued the record into the BIO; push it to the
            // transport now. A blocked flush leaves the tail stashed for a
            // later pump -- the plaintext is already accepted either way --
            // so only a hard transport error is fatal.
            if (ssl_socket_pump(self, false) == SSL_PUMP_ERR) {
                self->last_error = MP_EIO;
                *errcode = MP_EIO;
                return MP_STREAM_ERROR;
            }
            return (mp_uint_t)rc;
        }
        int err = SSL_get_error(self->ssl, rc);
        if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {
            self->last_error = MP_EIO;
            raise_stream_ssl_error(self->ssl, rc);
        }
        int p = ssl_socket_pump(self, err == SSL_ERROR_WANT_READ);
        if (ssl_pump_result(self, p, MP_STREAM_POLL_RD, errcode)) {
            return MP_STREAM_ERROR;
        }
    }
}

static mp_uint_t ssl_socket_ioctl(mp_obj_t self_in, mp_uint_t request,
    uintptr_t arg, int *errcode) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);

    // Follows the upstream modtls semantics: CLOSE frees the TLS state then
    // passes the close down to the underlying stream; POLL on a closed
    // socket reports NVAL; everything else is unsupported.
    mp_obj_t sock = self->sock_obj;
    if (request == MP_STREAM_CLOSE) {
        if (self->ssl == NULL) {
            return 0; // already closed
        }
        SSL_free(self->ssl);
        self->ssl = NULL;
        if (self->net_bio) {
            BIO_free(self->net_bio);
            self->net_bio = NULL;
        }
        self->sock_obj = mp_const_none;
        if (sock == mp_const_none || sock == MP_OBJ_NULL) {
            return 0;
        }
        return mp_get_stream(sock)->ioctl(sock, request, arg, errcode);
    }
    if (request == MP_STREAM_POLL) {
        if (self->ssl == NULL || self->last_error != 0
            || sock == mp_const_none || sock == MP_OBJ_NULL) {
            return MP_STREAM_POLL_NVAL;
        }

        // If the last read/write needed the opposite direction, poll the
        // transport for that instead, but remember what the caller asked
        // so the answer can be reported in their terms.
        mp_uint_t want = arg;
        mp_uint_t saved = 0;
        const mp_uint_t rdwr = MP_STREAM_POLL_RD | MP_STREAM_POLL_WR;
        if (self->poll_mask && (want & rdwr)) {
            saved = want & rdwr;
            want = (want & ~saved) | self->poll_mask;
        }

        mp_uint_t ret = 0;
        // Decrypted data already buffered inside AmiSSL is readable
        // without touching the transport.
        if ((want & MP_STREAM_POLL_RD) && SSL_pending(self->ssl) > 0) {
            ret |= MP_STREAM_POLL_RD;
            if (want == MP_STREAM_POLL_RD) {
                return MP_STREAM_POLL_RD;
            }
        }

        ret |= mp_get_stream(sock)->ioctl(sock, request, want, errcode);

        // Translate the transport's readiness back to the caller's
        // requested direction so it re-enters read()/write().
        if (self->poll_mask && (ret & self->poll_mask)) {
            ret |= saved;
        }
        return ret;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static mp_obj_t ssl_socket_close(mp_obj_t self_in) {
    int errcode = 0;
    ssl_socket_ioctl(self_in, MP_STREAM_CLOSE, 0, &errcode);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(ssl_socket_close_obj, ssl_socket_close);

// SSLSocket.setblocking(flag) -- propagate to the underlying stream so a
// caller can flip blocking mode without reaching past the TLS wrapper.
static mp_obj_t ssl_socket_setblocking(mp_obj_t self_in, mp_obj_t flag_in) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_obj_t sock = self->sock_obj;
    if (sock == mp_const_none || sock == MP_OBJ_NULL) {
        return mp_const_none;
    }
    mp_obj_t dest[3];
    mp_load_method(sock, MP_QSTR_setblocking, dest);
    dest[2] = flag_in;
    return mp_call_method_n_kw(1, 0, dest);
}
static MP_DEFINE_CONST_FUN_OBJ_2(
    ssl_socket_setblocking_obj, ssl_socket_setblocking);

static const mp_stream_p_t ssl_socket_stream_p = {
    .read = ssl_socket_read,
    .write = ssl_socket_write,
    .ioctl = ssl_socket_ioctl,
};

static const mp_rom_map_elem_t ssl_socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),    MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),
      MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),
      MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),
      MP_ROM_PTR(&mp_stream_write_obj) },
    // send/recv aliases so SSLSocket is drop-in compatible with the
    // BSD-socket idiom used by urequests and any user code mirroring
    // modsocket.c's surface. They route through the same stream
    // protocol callbacks as write/read, so semantics match exactly.
    { MP_ROM_QSTR(MP_QSTR_send),    MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv),    MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking),
      MP_ROM_PTR(&ssl_socket_setblocking_obj) },
    // ioctl is exposed (upstream only does so under coverage builds) so
    // generic stream tooling -- and tests/extmod/ssl_ioctl.py -- can
    // reach the poll/close machinery directly.
    { MP_ROM_QSTR(MP_QSTR_ioctl),   MP_ROM_PTR(&mp_stream_ioctl_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),
      MP_ROM_PTR(&ssl_socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),
      MP_ROM_PTR(&ssl_socket_close_obj) },
};
static MP_DEFINE_CONST_DICT(
    ssl_socket_locals_dict, ssl_socket_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ssl_socket_type,
    MP_QSTR_SSLSocket,
    MP_TYPE_FLAG_NONE,
    protocol, &ssl_socket_stream_p,
    locals_dict, &ssl_socket_locals_dict);

static const mp_rom_map_elem_t ssl_context_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_load_verify_locations),
      MP_ROM_PTR(&ssl_context_load_verify_locations_obj) },
    { MP_ROM_QSTR(MP_QSTR_set_default_verify_paths),
      MP_ROM_PTR(&ssl_context_set_default_verify_paths_obj) },
    { MP_ROM_QSTR(MP_QSTR_load_cert_chain),
      MP_ROM_PTR(&ssl_context_load_cert_chain_obj) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket),
      MP_ROM_PTR(&ssl_context_wrap_socket_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),
      MP_ROM_PTR(&ssl_context_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),
      MP_ROM_PTR(&ssl_context_close_obj) },
};
static MP_DEFINE_CONST_DICT(
    ssl_context_locals_dict, ssl_context_locals_dict_table);

static MP_DEFINE_CONST_OBJ_TYPE(
    ssl_context_type,
    MP_QSTR_SSLContext,
    MP_TYPE_FLAG_NONE,
    make_new, ssl_context_make_new,
    attr, ssl_context_attr,
    locals_dict, &ssl_context_locals_dict);

// Advertise the AmiSSL/OpenSSL build version. Callers (and the upstream
// ssl tests) key off hasattr(ssl, "OPENSSL_VERSION") to tell an OpenSSL
// backend from axtls; exposing it lets the CERT_OPTIONAL/CERT_REQUIRED
// verify-mode tests run rather than skip.
static const MP_DEFINE_STR_OBJ(openssl_version_obj, OPENSSL_VERSION_TEXT);

static const mp_rom_map_elem_t mp_module_ssl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ssl) },
    { MP_ROM_QSTR(MP_QSTR_SSLContext),
      MP_ROM_PTR(&ssl_context_type) },
    { MP_ROM_QSTR(MP_QSTR_OPENSSL_VERSION),
      MP_ROM_PTR(&openssl_version_obj) },
    { MP_ROM_QSTR(MP_QSTR_wrap_socket),
      MP_ROM_PTR(&mod_ssl_wrap_socket_obj) },
    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_TLS_CLIENT),
      MP_ROM_INT(MOD_PROTOCOL_TLS_CLIENT) },
    { MP_ROM_QSTR(MP_QSTR_PROTOCOL_TLS_SERVER),
      MP_ROM_INT(MOD_PROTOCOL_TLS_SERVER) },
    { MP_ROM_QSTR(MP_QSTR_CERT_NONE),
      MP_ROM_INT(MOD_CERT_NONE) },
    { MP_ROM_QSTR(MP_QSTR_CERT_OPTIONAL),
      MP_ROM_INT(MOD_CERT_OPTIONAL) },
    { MP_ROM_QSTR(MP_QSTR_CERT_REQUIRED),
      MP_ROM_INT(MOD_CERT_REQUIRED) },
};
static MP_DEFINE_CONST_DICT(
    mp_module_ssl_globals, mp_module_ssl_globals_table);

const mp_obj_module_t mp_module_ssl = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&mp_module_ssl_globals,
};

MP_REGISTER_MODULE(MP_QSTR_ssl, mp_module_ssl);

#endif // MICROPY_PY_AMIGA_SSL
