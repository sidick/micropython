// sys.stdin / sys.stdout / sys.stderr stream objects for the Amiga port.
// Read/write go through the existing mphal_stdio HAL (which talks directly to
// dos.library Input()/Output()). fileno() returns POSIX-style 0/1/2 so simple
// scripts that probe sys.stdin.fileno() work; the value is informational only
// since AmigaOS uses BPTR file handles, not fds.

#include "py/obj.h"
#include "py/stream.h"
#include "py/mperrno.h"
#include "py/mphal.h"

#define STDIO_FD_IN  (0)
#define STDIO_FD_OUT (1)
#define STDIO_FD_ERR (2)

typedef struct _sys_stdio_obj_t {
    mp_obj_base_t base;
    int fd;
} sys_stdio_obj_t;

static void stdio_obj_print(const mp_print_t *print, mp_obj_t self_in, mp_print_kind_t kind) {
    sys_stdio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    mp_printf(print, "<io.%s %d>", mp_obj_get_type_str(self_in), self->fd);
}

static mp_obj_t stdio_fileno(mp_obj_t self_in) {
    sys_stdio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    return MP_OBJ_NEW_SMALL_INT(self->fd);
}
static MP_DEFINE_CONST_FUN_OBJ_1(stdio_fileno_obj, stdio_fileno);

static mp_uint_t stdio_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    sys_stdio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd != STDIO_FD_IN) {
        *errcode = MP_EPERM;
        return MP_STREAM_ERROR;
    }
    for (mp_uint_t i = 0; i < size; i++) {
        int c = mp_hal_stdin_rx_chr();
        if (c == '\r') {
            c = '\n';
        }
        ((byte *)buf)[i] = c;
    }
    return size;
}

static mp_uint_t stdio_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    sys_stdio_obj_t *self = MP_OBJ_TO_PTR(self_in);
    if (self->fd != STDIO_FD_OUT && self->fd != STDIO_FD_ERR) {
        *errcode = MP_EPERM;
        return MP_STREAM_ERROR;
    }
    mp_hal_stdout_tx_strn_cooked(buf, size);
    return size;
}

static mp_uint_t stdio_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    (void)self_in;
    (void)arg;
    if (request == MP_STREAM_CLOSE) {
        return 0;
    }
    *errcode = MP_EINVAL;
    return MP_STREAM_ERROR;
}

static const mp_rom_map_elem_t stdio_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_fileno),    MP_ROM_PTR(&stdio_fileno_obj) },
    { MP_ROM_QSTR(MP_QSTR_read),      MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),  MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),  MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),     MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(stdio_locals_dict, stdio_locals_dict_table);

static const mp_stream_p_t stdio_stream_p = {
    .read = stdio_read,
    .write = stdio_write,
    .ioctl = stdio_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    stdio_obj_type,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    print, stdio_obj_print,
    protocol, &stdio_stream_p,
    locals_dict, &stdio_locals_dict
    );

const sys_stdio_obj_t mp_sys_stdin_obj = {{&stdio_obj_type}, .fd = STDIO_FD_IN};
const sys_stdio_obj_t mp_sys_stdout_obj = {{&stdio_obj_type}, .fd = STDIO_FD_OUT};
const sys_stdio_obj_t mp_sys_stderr_obj = {{&stdio_obj_type}, .fd = STDIO_FD_ERR};
