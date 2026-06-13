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
    mp_obj_t sock_obj; // pin the underlying socket against GC
} mp_ssl_socket_t;

static const mp_obj_type_t ssl_context_type;
static const mp_obj_type_t ssl_socket_type;

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

// SSLContext(protocol)
static mp_obj_t ssl_context_make_new(const mp_obj_type_t *type,
    size_t n_args, size_t n_kw,
    const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 1, 1, false);
    check_amissl();

    int protocol = mp_obj_get_int(args[0]);
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

    mp_ssl_context_t *self =
        mp_obj_malloc_with_finaliser(mp_ssl_context_t, type);
    self->ctx = ctx;
    return MP_OBJ_FROM_PTR(self);
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
    enum { ARG_cafile, ARG_capath };
    static const mp_arg_t allowed_args[] = {
        { MP_QSTR_cafile, MP_ARG_OBJ | MP_ARG_KW_ONLY,
          {.u_rom_obj = MP_ROM_NONE} },
        { MP_QSTR_capath, MP_ARG_OBJ | MP_ARG_KW_ONLY,
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
    if (cafile == NULL && capath == NULL) {
        mp_raise_TypeError(
            MP_ERROR_TEXT("cafile or capath required"));
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
            int cert = mp_obj_get_int(dest[1]);
            int mode;
            switch (cert) {
                case MOD_CERT_NONE:
                    mode = SSL_VERIFY_NONE;
                    break;
                case MOD_CERT_OPTIONAL:
                    mode = SSL_VERIFY_PEER;
                    break;
                case MOD_CERT_REQUIRED:
                    mode = SSL_VERIFY_PEER
                        | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
                    break;
                default:
                    mp_raise_ValueError(
                        MP_ERROR_TEXT("verify_mode"));
            }
            SSL_CTX_set_verify(self->ctx, mode, NULL);
            dest[0] = MP_OBJ_NULL; // success
        }
    }
}

// SSLContext.wrap_socket(sock, server_hostname=None,
//                        do_handshake_on_connect=True)
//
// sock must expose a fileno() method returning the underlying OS
// file descriptor (the bsdsocket.library handle). We don't take
// ownership of the socket -- the caller still owns its lifetime --
// but we pin a reference so it can't be GC'd while the SSLSocket
// is alive.
static mp_obj_t ssl_context_wrap_socket(
    size_t n_args, const mp_obj_t *pos_args, mp_map_t *kw_args) {
    enum { ARG_sock, ARG_server_hostname, ARG_do_handshake };
    static const mp_arg_t allowed[] = {
        { MP_QSTR_sock, MP_ARG_OBJ | MP_ARG_REQUIRED, {0} },
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
    if (self->ctx == NULL) {
        mp_raise_OSError(MP_EBADF);
    }

    mp_obj_t sock_obj = args[ARG_sock].u_obj;
    int fd = mp_obj_get_int(
        mp_call_function_0(mp_load_attr(sock_obj, MP_QSTR_fileno)));

    SSL *ssl = SSL_new(self->ctx);
    if (ssl == NULL) {
        raise_ssl_error();
    }
    SSL_set_fd(ssl, fd);

    if (args[ARG_server_hostname].u_obj != mp_const_none) {
        const char *hostname =
            mp_obj_str_get_str(args[ARG_server_hostname].u_obj);
        SSL_set_tlsext_host_name(ssl, hostname);
    }

    if (args[ARG_do_handshake].u_bool) {
        // Underlying sockets are blocking by default; SSL_connect
        // therefore either succeeds or returns a terminal error.
        // WANT_READ/WANT_WRITE only crop up on non-blocking sockets,
        // which Step 4 doesn't aim to handle; surface them as EAGAIN
        // so a future asyncio glue knows what to do.
        int rc = SSL_connect(ssl);
        if (rc != 1) {
            // Capture diagnostic state before SSL_free -- afterwards
            // the ssl pointer is dangling and SSL_get_verify_result
            // silently returns X509_V_OK off freed memory.
            int err = SSL_get_error(ssl, rc);
            long verify = SSL_get_verify_result(ssl);
            SSL_free(ssl);
            if (err == SSL_ERROR_WANT_READ
                || err == SSL_ERROR_WANT_WRITE) {
                mp_raise_OSError(MP_EAGAIN);
            }
            raise_ssl_diag(err, verify);
        }
    }

    mp_ssl_socket_t *s = mp_obj_malloc_with_finaliser(
        mp_ssl_socket_t, &ssl_socket_type);
    s->ssl = ssl;
    s->sock_obj = sock_obj;
    return MP_OBJ_FROM_PTR(s);
}
static MP_DEFINE_CONST_FUN_OBJ_KW(
    ssl_context_wrap_socket_obj, 1, ssl_context_wrap_socket);

// SSLSocket -- stream protocol on top of SSL_read / SSL_write.

// Translate an SSL_get_error code for a read/write call into a
// stream-protocol errcode. ZERO_RETURN is the caller's responsibility
// (read uses it as EOF; write doesn't).
static int ssl_stream_errcode(int err) {
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        return MP_EAGAIN;
    }
    return MP_EIO;
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

static mp_uint_t ssl_socket_read(mp_obj_t self_in, void *buf,
    mp_uint_t size, int *errcode) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ssl == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    int rc = SSL_read(self->ssl, buf, (int)size);
    if (rc > 0) {
        return (mp_uint_t)rc;
    }
    int err = SSL_get_error(self->ssl, rc);
    if (err == SSL_ERROR_ZERO_RETURN) {
        return 0; // clean TLS close = EOF
    }
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        *errcode = ssl_stream_errcode(err);
        return MP_STREAM_ERROR;
    }
    raise_stream_ssl_error(self->ssl, rc);
}

static mp_uint_t ssl_socket_write(mp_obj_t self_in, const void *buf,
    mp_uint_t size, int *errcode) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->ssl == NULL) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    int rc = SSL_write(self->ssl, buf, (int)size);
    if (rc > 0) {
        return (mp_uint_t)rc;
    }
    int err = SSL_get_error(self->ssl, rc);
    if (err == SSL_ERROR_WANT_READ || err == SSL_ERROR_WANT_WRITE) {
        *errcode = ssl_stream_errcode(err);
        return MP_STREAM_ERROR;
    }
    raise_stream_ssl_error(self->ssl, rc);
}

static mp_uint_t ssl_socket_ioctl(mp_obj_t self_in, mp_uint_t request,
    uintptr_t arg, int *errcode) {
    mp_ssl_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (request == MP_STREAM_CLOSE) {
        if (self->ssl) {
            // One-shot SSL_shutdown: send close_notify, don't wait
            // for peer's. Two-step polling is what asyncio buys us;
            // not in scope for Step 4. The peer may complain in its
            // log but TLS-spec-wise both sides are free to close.
            SSL_shutdown(self->ssl);
            SSL_free(self->ssl);
            self->ssl = NULL;
        }
        self->sock_obj = mp_const_none;
        return 0;
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

static const mp_rom_map_elem_t mp_module_ssl_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__), MP_ROM_QSTR(MP_QSTR_ssl) },
    { MP_ROM_QSTR(MP_QSTR_SSLContext),
      MP_ROM_PTR(&ssl_context_type) },
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
