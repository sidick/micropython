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
#include <proto/exec.h>
#include <dos/dosextens.h>

#include "py/runtime.h"
#include "py/objstr.h"
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

static const mp_obj_type_t ssl_context_type;

static void check_amissl(void) {
    if (AmiSSLBase == NULL) {
        mp_raise_msg(&mp_type_OSError,
            MP_ERROR_TEXT("amisslmaster.library missing"));
    }
}

static MP_NORETURN void raise_ssl_error(void) {
    // ERR_peek_error keeps the error in the queue for chained raises;
    // ERR_get_error would consume it and clear the next-call slot.
    unsigned long e = ERR_get_error();
    if (e == 0) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("TLS error"));
    }
    char buf[128];
    ERR_error_string_n(e, buf, sizeof(buf));
    mp_raise_msg_varg(&mp_type_OSError,
        MP_ERROR_TEXT("TLS: %s"), buf);
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

static const mp_rom_map_elem_t ssl_context_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_load_verify_locations),
        MP_ROM_PTR(&ssl_context_load_verify_locations_obj) },
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
