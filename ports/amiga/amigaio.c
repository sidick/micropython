#include <string.h>

#include <dos/dos.h>
#include <proto/dos.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/obj.h"
#include "py/mperrno.h"

// File object wrapping a dos.library BPTR file handle.
// Used by the Python open() builtin. Both binary (FileIO) and text
// (TextIOWrapper) modes share the same struct and protocol.

typedef struct _mp_obj_amiga_file_t {
    mp_obj_base_t base;
    BPTR fh;  // 0 = closed
    bool readable;
    bool writable;
} mp_obj_amiga_file_t;

extern const mp_obj_type_t mp_type_amiga_fileio;
extern const mp_obj_type_t mp_type_amiga_textio;

// Map a dos.library IoErr() code to a MicroPython MP_E* errno.
static int dos_errno(void) {
    switch (IoErr()) {
        case ERROR_OBJECT_NOT_FOUND:    return MP_ENOENT;
        case ERROR_OBJECT_EXISTS:       return MP_EEXIST;
        case ERROR_DISK_FULL:           return MP_ENOSPC;
        case ERROR_OBJECT_IN_USE:       return MP_EBUSY;
        case ERROR_READ_PROTECTED:
        case ERROR_WRITE_PROTECTED:     return MP_EACCES;
        case ERROR_OBJECT_WRONG_TYPE:   return MP_EISDIR;
        case ERROR_DIRECTORY_NOT_EMPTY: return MP_EACCES;
        default:                        return MP_EIO;
    }
}

static mp_uint_t amiga_file_read(mp_obj_t self_in, void *buf, mp_uint_t size, int *errcode) {
    mp_obj_amiga_file_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->fh || !self->readable) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    LONG n = Read(self->fh, buf, (LONG)size);
    if (n < 0) {
        *errcode = dos_errno();
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t amiga_file_write(mp_obj_t self_in, const void *buf, mp_uint_t size, int *errcode) {
    mp_obj_amiga_file_t *self = MP_OBJ_TO_PTR(self_in);
    if (!self->fh || !self->writable) {
        *errcode = MP_EBADF;
        return MP_STREAM_ERROR;
    }
    LONG n = Write(self->fh, (APTR)buf, (LONG)size);
    if (n < 0) {
        *errcode = dos_errno();
        return MP_STREAM_ERROR;
    }
    if ((mp_uint_t)n < size) {
        *errcode = MP_ENOSPC;
        return MP_STREAM_ERROR;
    }
    return (mp_uint_t)n;
}

static mp_uint_t amiga_file_ioctl(mp_obj_t self_in, mp_uint_t request, uintptr_t arg, int *errcode) {
    mp_obj_amiga_file_t *self = MP_OBJ_TO_PTR(self_in);
    switch (request) {
        case MP_STREAM_FLUSH:
            if (self->fh && !Flush(self->fh)) {
                *errcode = MP_EIO;
                return MP_STREAM_ERROR;
            }
            return 0;

        case MP_STREAM_SEEK: {
            if (!self->fh) {
                *errcode = MP_EBADF;
                return MP_STREAM_ERROR;
            }
            struct mp_stream_seek_t *s = (struct mp_stream_seek_t *)arg;
            // Map POSIX whence to AmigaOS: SET=-1, CUR=0, END=1
            static const LONG whence_map[3] = {
                OFFSET_BEGINNING, OFFSET_CURRENT, OFFSET_END
            };
            if (s->whence < 0 || s->whence > 2) {
                *errcode = MP_EINVAL;
                return MP_STREAM_ERROR;
            }
            if (Seek(self->fh, (LONG)s->offset, whence_map[s->whence]) < 0) {
                *errcode = dos_errno();
                return MP_STREAM_ERROR;
            }
            // Seek() returns old position; get new position with a 0-offset seek.
            LONG pos = Seek(self->fh, 0, OFFSET_CURRENT);
            if (pos < 0) {
                *errcode = dos_errno();
                return MP_STREAM_ERROR;
            }
            s->offset = pos;
            return 0;
        }

        case MP_STREAM_CLOSE:
            if (self->fh) {
                Close(self->fh);
                self->fh = 0;
            }
            return 0;

        default:
            *errcode = MP_EINVAL;
            return MP_STREAM_ERROR;
    }
}

static const mp_rom_map_elem_t amiga_file_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_read),      MP_ROM_PTR(&mp_stream_read_obj) },
    { MP_ROM_QSTR(MP_QSTR_readinto),  MP_ROM_PTR(&mp_stream_readinto_obj) },
    { MP_ROM_QSTR(MP_QSTR_readline),  MP_ROM_PTR(&mp_stream_unbuffered_readline_obj) },
    { MP_ROM_QSTR(MP_QSTR_readlines), MP_ROM_PTR(&mp_stream_unbuffered_readlines_obj) },
    { MP_ROM_QSTR(MP_QSTR_write),     MP_ROM_PTR(&mp_stream_write_obj) },
    { MP_ROM_QSTR(MP_QSTR_seek),      MP_ROM_PTR(&mp_stream_seek_obj) },
    { MP_ROM_QSTR(MP_QSTR_tell),      MP_ROM_PTR(&mp_stream_tell_obj) },
    { MP_ROM_QSTR(MP_QSTR_flush),     MP_ROM_PTR(&mp_stream_flush_obj) },
    { MP_ROM_QSTR(MP_QSTR_close),     MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___del__),   MP_ROM_PTR(&mp_stream_close_obj) },
    { MP_ROM_QSTR(MP_QSTR___enter__), MP_ROM_PTR(&mp_identity_obj) },
    { MP_ROM_QSTR(MP_QSTR___exit__),  MP_ROM_PTR(&mp_stream___exit___obj) },
};
static MP_DEFINE_CONST_DICT(amiga_file_locals_dict, amiga_file_locals_dict_table);

static const mp_stream_p_t amiga_fileio_stream_p = {
    .read = amiga_file_read,
    .write = amiga_file_write,
    .ioctl = amiga_file_ioctl,
};

static const mp_stream_p_t amiga_textio_stream_p = {
    .read = amiga_file_read,
    .write = amiga_file_write,
    .ioctl = amiga_file_ioctl,
    .is_text = true,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_amiga_fileio,
    MP_QSTR_FileIO,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &amiga_fileio_stream_p,
    locals_dict, &amiga_file_locals_dict
    );

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_amiga_textio,
    MP_QSTR_TextIOWrapper,
    MP_TYPE_FLAG_ITER_IS_STREAM,
    protocol, &amiga_textio_stream_p,
    locals_dict, &amiga_file_locals_dict
    );

mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    const char *fname = mp_obj_str_get_str(args[0]);
    const char *mode_s = "r";

    if (n_args > 1) {
        mode_s = mp_obj_str_get_str(args[1]);
    } else {
        mp_map_elem_t *elem = mp_map_lookup(kwargs, MP_OBJ_NEW_QSTR(MP_QSTR_mode), MP_MAP_LOOKUP);
        if (elem) {
            mode_s = mp_obj_str_get_str(elem->value);
        }
    }

    char base = 'r';
    bool has_plus = false, is_binary = false;
    for (const char *m = mode_s; *m; m++) {
        if (*m == 'r' || *m == 'w' || *m == 'a') {
            base = *m;
        } else if (*m == '+') {
            has_plus = true;
        } else if (*m == 'b') {
            is_binary = true;
        }
    }

    BPTR fh;
    if (base == 'r' && !has_plus) {
        fh = Open((STRPTR)fname, MODE_OLDFILE);
    } else if (base == 'r' && has_plus) {
        fh = Open((STRPTR)fname, MODE_READWRITE);
    } else if (base == 'w') {
        // 'w' and 'w+' both truncate
        fh = Open((STRPTR)fname, MODE_NEWFILE);
    } else {
        // 'a' and 'a+': open existing for read/write, or create; seek to end
        fh = Open((STRPTR)fname, MODE_READWRITE);
        if (!fh) {
            fh = Open((STRPTR)fname, MODE_NEWFILE);
        }
        if (fh) {
            Seek(fh, 0, OFFSET_END);
        }
    }

    if (!fh) {
        mp_raise_OSError(dos_errno());
    }

    const mp_obj_type_t *type = is_binary ? &mp_type_amiga_fileio : &mp_type_amiga_textio;
    mp_obj_amiga_file_t *o = mp_obj_malloc_with_finaliser(mp_obj_amiga_file_t, type);
    o->fh = fh;
    o->readable = (base == 'r') || has_plus;
    o->writable = (base != 'r') || has_plus;
    return MP_OBJ_FROM_PTR(o);
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
