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

#include "py/mpconfig.h"

#if MICROPY_PY_AMIGA_SOCKET

#include <stdio.h>
#include <string.h>

// Avoid the devices/timer.h timeval conflict; use clib2's sys/time.h instead.
#define __NO_NETINCLUDE_TIMEVAL
#include <sys/time.h>

#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <proto/exec.h>
#include <proto/bsdsocket.h>

#include "py/objtuple.h"
#include "py/objstr.h"
#include "py/runtime.h"
#include "py/stream.h"
#include "py/mperrno.h"

// SocketBase is declared extern by proto/bsdsocket.h; define it here.
struct Library *SocketBase;

// Open bsdsocket.library. Called from main(). Returns false if unavailable.
bool amiga_socket_open(void) {
    SocketBase = OpenLibrary((STRPTR)"bsdsocket.library", 4);
    return SocketBase != NULL;
}

void amiga_socket_close(void) {
    if (SocketBase) {
        CloseLibrary(SocketBase);
        SocketBase = NULL;
    }
}

// Map Errno() to MicroPython MP_E* constants.
static int sock_errno(void) {
    int e = (int)Errno();
    // Errno() returns POSIX errno values — they match MicroPython's MP_E* codes.
    return e ? e : MP_EIO;
}

// -------------------------------------------------------------------------
// socket object

typedef struct _mp_obj_amiga_socket_t {
    mp_obj_base_t base;
    LONG fd;        // -1 = closed
    int timeout_ms; // -1 = blocking, 0 = non-blocking, >0 = timeout ms
} mp_obj_amiga_socket_t;

extern const mp_obj_type_t amiga_socket_type;

// Build a struct sockaddr_in from a (host, port) Python tuple.
static void sockaddr_from_tuple(mp_obj_t addr_in, struct sockaddr_in *addr, socklen_t *addrlen) {
    mp_obj_t *items;
    mp_obj_get_array_fixed_n(addr_in, 2, &items);
    const char *host = mp_obj_str_get_str(items[0]);
    int port = mp_obj_get_int(items[1]);

    memset(addr, 0, sizeof(*addr));
    addr->sin_family = AF_INET;
    addr->sin_port = htons((uint16_t)port);

    // Try numeric IP first, then hostname lookup.
    addr->sin_addr.s_addr = inet_addr((STRPTR)host);
    if (addr->sin_addr.s_addr == (in_addr_t)-1) {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            mp_raise_OSError(MP_ENOENT);
        }
        memcpy(&addr->sin_addr, he->h_addr, he->h_length);
    }
    *addrlen = sizeof(*addr);
}

// Convert a raw sockaddr to a (ip_string, port) Python tuple.
static mp_obj_t sockaddr_to_tuple(const struct sockaddr_in *addr) {
    mp_obj_t items[2];
    const char *ip_str = (const char *)Inet_NtoA(addr->sin_addr.s_addr);
    items[0] = mp_obj_new_str(ip_str, strlen(ip_str));
    items[1] = mp_obj_new_int(ntohs(addr->sin_port));
    return mp_obj_new_tuple(2, items);
}

// Resolve addr argument: either (host, port) tuple or raw bytes sockaddr.
static void resolve_addr(mp_obj_t addr_in, struct sockaddr_in *addr, socklen_t *addrlen) {
    if (mp_obj_is_type(addr_in, &mp_type_tuple)) {
        sockaddr_from_tuple(addr_in, addr, addrlen);
    } else {
        mp_buffer_info_t buf;
        mp_get_buffer_raise(addr_in, &buf, MP_BUFFER_READ);
        if (buf.len < sizeof(struct sockaddr_in)) {
            mp_raise_OSError(MP_EINVAL);
        }
        memcpy(addr, buf.buf, sizeof(struct sockaddr_in));
        *addrlen = (socklen_t)buf.len;
    }
}

static mp_uint_t socket_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    LONG n = recv(self->fd, buf, (LONG)size, 0);
    if (n < 0) {
        *errcode = sock_errno();
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t socket_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd < 0) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    LONG n = send(self->fd, (APTR)buf, (LONG)size, 0);
    if (n < 0) {
        *errcode = sock_errno();
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t socket_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    (void)arg;
    if (request == MP_STREAM_CLOSE) {
        if (self->fd >= 0) {
            CloseSocket(self->fd);
            self->fd = -1;
        }
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_stream_p_t socket_stream_p = {
    .read = socket_read,
    .write = socket_write,
    .ioctl = socket_ioctl,
};

// socket(family=AF_INET, type=SOCK_STREAM, proto=0)
static mp_obj_t socket_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    mp_arg_check_num(n_args, n_kw, 0, 3, false);
    if (!SocketBase) {
        mp_raise_msg(&mp_type_OSError, MP_ERROR_TEXT("bsdsocket.library not available"));
    }
    int family = (n_args > 0) ? mp_obj_get_int(args[0]) : AF_INET;
    int stype = (n_args > 1) ? mp_obj_get_int(args[1]) : SOCK_STREAM;
    int proto = (n_args > 2) ? mp_obj_get_int(args[2]) : 0;

    LONG fd = socket((LONG)family, (LONG)stype, (LONG)proto);
    if (fd < 0) {
        mp_raise_OSError(sock_errno());
    }
    mp_obj_amiga_socket_t *s = mp_obj_malloc_with_finaliser(mp_obj_amiga_socket_t, &amiga_socket_type);
    s->fd = fd;
    s->timeout_ms = -1;
    return MP_OBJ_FROM_PTR(s);
}

static mp_obj_t socket_connect(mp_obj_t self_in, mp_obj_t addr_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    struct sockaddr_in addr;
    socklen_t addrlen;
    resolve_addr(addr_in, &addr, &addrlen);
    if (connect(self->fd, (struct sockaddr *)&addr, addrlen) < 0) {
        mp_raise_OSError(sock_errno());
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_connect_obj, socket_connect);

static mp_obj_t socket_bind(mp_obj_t self_in, mp_obj_t addr_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    struct sockaddr_in addr;
    socklen_t addrlen;
    resolve_addr(addr_in, &addr, &addrlen);
    if (bind(self->fd, (struct sockaddr *)&addr, addrlen) < 0) {
        mp_raise_OSError(sock_errno());
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_bind_obj, socket_bind);

static mp_obj_t socket_listen(mp_obj_t self_in, mp_obj_t backlog_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (listen(self->fd, (LONG)mp_obj_get_int(backlog_in)) < 0) {
        mp_raise_OSError(sock_errno());
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_listen_obj, socket_listen);

static mp_obj_t socket_accept(mp_obj_t self_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    LONG fd = accept(self->fd, (struct sockaddr *)&addr, &addrlen);
    if (fd < 0) {
        mp_raise_OSError(sock_errno());
    }
    mp_obj_amiga_socket_t *ns = mp_obj_malloc_with_finaliser(mp_obj_amiga_socket_t, &amiga_socket_type);
    ns->fd = fd;
    ns->timeout_ms = self->timeout_ms;
    mp_obj_t tuple[2] = { MP_OBJ_FROM_PTR(ns), sockaddr_to_tuple(&addr) };
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_accept_obj, socket_accept);

static mp_obj_t socket_recv(mp_obj_t self_in, mp_obj_t size_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    LONG size = (LONG)mp_obj_get_int(size_in);
    vstr_t vstr;
    vstr_init_len(&vstr, size);
    LONG n = recv(self->fd, vstr.buf, size, 0);
    if (n < 0) {
        vstr_clear(&vstr);
        mp_raise_OSError(sock_errno());
    }
    vstr.len = n;
    return mp_obj_new_bytes_from_vstr(&vstr);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_recv_obj, socket_recv);

static mp_obj_t socket_recvfrom(mp_obj_t self_in, mp_obj_t size_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    LONG size = (LONG)mp_obj_get_int(size_in);
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    vstr_t vstr;
    vstr_init_len(&vstr, size);
    LONG n = recvfrom(self->fd, vstr.buf, size, 0, (struct sockaddr *)&addr, &addrlen);
    if (n < 0) {
        vstr_clear(&vstr);
        mp_raise_OSError(sock_errno());
    }
    vstr.len = n;
    mp_obj_t tuple[2] = {
        mp_obj_new_bytes_from_vstr(&vstr),
        sockaddr_to_tuple(&addr),
    };
    return mp_obj_new_tuple(2, tuple);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_recvfrom_obj, socket_recvfrom);

static mp_obj_t socket_send(mp_obj_t self_in, mp_obj_t data_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    LONG n = send(self->fd, buf.buf, (LONG)buf.len, 0);
    if (n < 0) {
        mp_raise_OSError(sock_errno());
    }
    return mp_obj_new_int(n);
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_send_obj, socket_send);

static mp_obj_t socket_sendall(mp_obj_t self_in, mp_obj_t data_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(data_in, &buf, MP_BUFFER_READ);
    const char *ptr = buf.buf;
    size_t remaining = buf.len;
    while (remaining > 0) {
        LONG n = send(self->fd, (APTR)ptr, (LONG)remaining, 0);
        if (n < 0) {
            mp_raise_OSError(sock_errno());
        }
        ptr += n;
        remaining -= n;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_sendall_obj, socket_sendall);

static mp_obj_t socket_sendto(size_t n_args, const mp_obj_t *args) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(args[0]);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(args[1], &buf, MP_BUFFER_READ);
    struct sockaddr_in addr;
    socklen_t addrlen;
    resolve_addr(args[2], &addr, &addrlen);
    LONG n = sendto(self->fd, buf.buf, (LONG)buf.len, 0, (struct sockaddr *)&addr, addrlen);
    if (n < 0) {
        mp_raise_OSError(sock_errno());
    }
    return mp_obj_new_int(n);
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_sendto_obj, 3, 3, socket_sendto);

static mp_obj_t socket_setblocking(mp_obj_t self_in, mp_obj_t flag_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    LONG nonblock = mp_obj_is_true(flag_in) ? 0L : 1L;
    IoctlSocket(self->fd, FIONBIO, (APTR)&nonblock);
    self->timeout_ms = nonblock ? 0 : -1;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_setblocking_obj, socket_setblocking);

static mp_obj_t socket_settimeout(mp_obj_t self_in, mp_obj_t t_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (t_in == mp_const_none) {
        // Blocking mode
        LONG nonblock = 0L;
        IoctlSocket(self->fd, FIONBIO, (APTR)&nonblock);
        self->timeout_ms = -1;
    } else {
        mp_float_t t = mp_obj_get_float(t_in);
        if (t == 0) {
            LONG nonblock = 1L;
            IoctlSocket(self->fd, FIONBIO, (APTR)&nonblock);
            self->timeout_ms = 0;
        } else {
            int ms = (int)(t * 1000);
            struct timeval tv;
            tv.tv_sec = ms / 1000;
            tv.tv_usec = (ms % 1000) * 1000;
            setsockopt(self->fd, SOL_SOCKET, SO_RCVTIMEO, (APTR)&tv, sizeof(tv));
            setsockopt(self->fd, SOL_SOCKET, SO_SNDTIMEO, (APTR)&tv, sizeof(tv));
            self->timeout_ms = ms;
        }
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(socket_settimeout_obj, socket_settimeout);

static mp_obj_t socket_setsockopt(size_t n_args, const mp_obj_t *args) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(args[0]);
    int level = mp_obj_get_int(args[1]);
    int opt = mp_obj_get_int(args[2]);
    if (mp_obj_is_int(args[3])) {
        LONG val = (LONG)mp_obj_get_int(args[3]);
        setsockopt(self->fd, (LONG)level, (LONG)opt, (APTR)&val, sizeof(val));
    } else {
        mp_buffer_info_t buf;
        mp_get_buffer_raise(args[3], &buf, MP_BUFFER_READ);
        setsockopt(self->fd, (LONG)level, (LONG)opt, buf.buf, (socklen_t)buf.len);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_setsockopt_obj, 4, 4, socket_setsockopt);

static mp_obj_t socket_close(mp_obj_t self_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd >= 0) {
        CloseSocket(self->fd);
        self->fd = -1;
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_close_obj, socket_close);

static mp_obj_t socket_fileno(mp_obj_t self_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    return mp_obj_new_int(self->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_fileno_obj, socket_fileno);

// makefile: return self; socket implements the stream protocol.
static mp_obj_t socket_makefile(size_t n_args, const mp_obj_t *args) {
    (void)n_args;
    (void)args;
    return args[0];
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(socket_makefile_obj, 1, 3, socket_makefile);

static mp_obj_t socket_getpeername(mp_obj_t self_in) {
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    struct sockaddr_in addr;
    socklen_t addrlen = sizeof(addr);
    if (getpeername(self->fd, (struct sockaddr *)&addr, &addrlen) < 0) {
        mp_raise_OSError(sock_errno());
    }
    return sockaddr_to_tuple(&addr);
}
static MP_DEFINE_CONST_FUN_OBJ_1(socket_getpeername_obj, socket_getpeername);

static const mp_rom_map_elem_t socket_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_connect),     MP_ROM_PTR(&socket_connect_obj) },
    { MP_ROM_QSTR(MP_QSTR_bind),        MP_ROM_PTR(&socket_bind_obj) },
    { MP_ROM_QSTR(MP_QSTR_listen),      MP_ROM_PTR(&socket_listen_obj) },
    { MP_ROM_QSTR(MP_QSTR_accept),      MP_ROM_PTR(&socket_accept_obj) },
    { MP_ROM_QSTR(MP_QSTR_recv),        MP_ROM_PTR(&socket_recv_obj) },
    { MP_ROM_QSTR(MP_QSTR_recvfrom),    MP_ROM_PTR(&socket_recvfrom_obj) },
    { MP_ROM_QSTR(MP_QSTR_send),        MP_ROM_PTR(&socket_send_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendall),     MP_ROM_PTR(&socket_sendall_obj) },
    { MP_ROM_QSTR(MP_QSTR_sendto),      MP_ROM_PTR(&socket_sendto_obj) },
    { MP_ROM_QSTR(MP_QSTR_setblocking), MP_ROM_PTR(&socket_setblocking_obj) },
    { MP_ROM_QSTR(MP_QSTR_settimeout),  MP_ROM_PTR(&socket_settimeout_obj) },
    { MP_ROM_QSTR(MP_QSTR_setsockopt),  MP_ROM_PTR(&socket_setsockopt_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),       MP_ROM_PTR(&socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR_fileno),      MP_ROM_PTR(&socket_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_makefile),    MP_ROM_PTR(&socket_makefile_obj) },
    { MP_ROM_QSTR(MP_QSTR_getpeername), MP_ROM_PTR(&socket_getpeername_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),        MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),    MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),    MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),       MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),     MP_ROM_PTR(&socket_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__),   MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),    MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(socket_locals_dict, socket_locals_dict_table);

static void socket_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    (void)kind;
    mp_obj_amiga_socket_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<socket fd=%d>", (int)self->fd);
}

MP_DEFINE_CONST_OBJ_TYPE(
    amiga_socket_type,
    MP_QSTR_socket,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    make_new, socket_make_new,
    print, socket_print,
    protocol, &socket_stream_p,
    locals_dict, &socket_locals_dict
    );

// -------------------------------------------------------------------------
// Module-level functions

// socket.getaddrinfo(host, port[, af[, type[, proto[, flags]]]])
//
// We deliberately don't call bsdsocket.library's getaddrinfo: UAE's
// built-in bsdsocket emulation only implements the original BSD
// socket API (lib_NegSize ~ 0x12c = 50 vector slots), and
// getaddrinfo lives at LVO 0x32A — far past the end of UAE's
// vector area, so calling it walks off into bad memory and faults
// with Software Failure 80000004. Roadshow / Miami / etc. ship a
// real getaddrinfo at that LVO but UAE doesn't.
//
// Build the result tuple ourselves from gethostbyname (works on
// every bsdsocket-compatible stack including UAE). IPv4-only,
// single result, no AI_PASSIVE handling — sufficient for the
// usual `addr = getaddrinfo(host, port)[0][-1]; sock.connect(addr)`
// pattern. Numeric IP strings short-circuit so we don't pay a
// lookup for them.
static mp_obj_t mod_getaddrinfo(size_t n_args, const mp_obj_t *args) {
    const char *host = mp_obj_str_get_str(args[0]);
    int port = mp_obj_get_int(args[1]);
    int stype = (n_args > 3) ? mp_obj_get_int(args[3]) : SOCK_STREAM;
    int proto = (n_args > 4) ? mp_obj_get_int(args[4]) : 0;
    // ai_family + ai_flags ignored: UAE bsdsocket is IPv4-only.

    struct sockaddr_in sa;
    memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);

    in_addr_t numeric = inet_addr((STRPTR)host);
    if (numeric != (in_addr_t)-1) {
        sa.sin_addr.s_addr = numeric;
    } else {
        struct hostent *he = gethostbyname((STRPTR)host);
        if (!he) {
            mp_raise_OSError(MP_ENOENT);
        }
        memcpy(&sa.sin_addr, he->h_addr, he->h_length);
    }

    mp_obj_t tuple[5];
    tuple[0] = mp_obj_new_int(AF_INET);
    tuple[1] = mp_obj_new_int(stype);
    tuple[2] = mp_obj_new_int(proto);
    tuple[3] = mp_const_none; // no canonical name from gethostbyname path
    tuple[4] = mp_obj_new_bytes((const byte *)&sa, sizeof(sa));

    mp_obj_t list = mp_obj_new_list(0, NULL);
    mp_obj_list_append(list, mp_obj_new_tuple(5, tuple));
    return list;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_getaddrinfo_obj, 2, 6, mod_getaddrinfo);

static mp_obj_t mod_getfqdn(mp_obj_t name_in) {
    (void)name_in;
    char buf[256];
    if (gethostname((STRPTR)buf, sizeof(buf)) < 0) {
        return mp_obj_new_str("", 0);
    }
    return mp_obj_new_str(buf, strlen(buf));
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_getfqdn_obj, mod_getfqdn);

// -------------------------------------------------------------------------
// Module definition

static const mp_rom_map_elem_t socket_module_globals_table[] = {
    { MP_ROM_QSTR(MP_QSTR___name__),   MP_ROM_QSTR(MP_QSTR_socket) },
    { MP_ROM_QSTR(MP_QSTR_socket),     MP_ROM_PTR(&amiga_socket_type) },
    { MP_ROM_QSTR(MP_QSTR_getaddrinfo), MP_ROM_PTR(&mod_getaddrinfo_obj) },
    { MP_ROM_QSTR(MP_QSTR_getfqdn),    MP_ROM_PTR(&mod_getfqdn_obj) },

    // Address families
    { MP_ROM_QSTR(MP_QSTR_AF_UNSPEC),  MP_ROM_INT(AF_UNSPEC) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET),    MP_ROM_INT(AF_INET) },
    { MP_ROM_QSTR(MP_QSTR_AF_INET6),   MP_ROM_INT(AF_INET6) },

    // Socket types
    { MP_ROM_QSTR(MP_QSTR_SOCK_STREAM), MP_ROM_INT(SOCK_STREAM) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_DGRAM),  MP_ROM_INT(SOCK_DGRAM) },
    { MP_ROM_QSTR(MP_QSTR_SOCK_RAW),    MP_ROM_INT(SOCK_RAW) },

    // Protocols
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_IP),  MP_ROM_INT(IPPROTO_IP) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_TCP), MP_ROM_INT(IPPROTO_TCP) },
    { MP_ROM_QSTR(MP_QSTR_IPPROTO_UDP), MP_ROM_INT(IPPROTO_UDP) },

    // Socket options
    { MP_ROM_QSTR(MP_QSTR_SOL_SOCKET),   MP_ROM_INT(SOL_SOCKET) },
    { MP_ROM_QSTR(MP_QSTR_SO_REUSEADDR), MP_ROM_INT(SO_REUSEADDR) },
    { MP_ROM_QSTR(MP_QSTR_SO_RCVTIMEO),  MP_ROM_INT(SO_RCVTIMEO) },
    { MP_ROM_QSTR(MP_QSTR_SO_SNDTIMEO),  MP_ROM_INT(SO_SNDTIMEO) },
};
static MP_DEFINE_CONST_DICT(socket_module_globals, socket_module_globals_table);

const mp_obj_module_t mp_module_socket = {
    .base = { &mp_type_module },
    .globals = (mp_obj_dict_t *)&socket_module_globals,
};

MP_REGISTER_MODULE(MP_QSTR_socket, mp_module_socket);

#endif // MICROPY_PY_AMIGA_SOCKET
