// vfs_amiga.c — VFS implementation that wraps dos.library so the rest of
// MicroPython can use os.chdir/os.getcwd/os.listdir/etc. natively.
//
// Architecture: a single mp_type_vfs_amiga instance is mounted at "/"
// by main.c. AmigaDOS keeps the process cwd in pr_CurrentDir, so the
// VFS object itself is stateless and just dispatches into dos.library
// calls. Path strings come in as Python strings; we hand them straight
// to dos.library (AmigaOS paths look like "volume:dir/file" or just
// "dir/file" for cwd-relative).
//
// File types live here too (mp_type_amiga_fileio / mp_type_amiga_textio
// implementing the mp_stream_p_t protocol) so that vfs_amiga_open can
// return them directly. They replace the file types that used to live
// in amigaio.c.

#include <string.h>

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <exec/tasks.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "py/runtime.h"
#include "py/stream.h"
#include "py/obj.h"
#include "py/mperrno.h"
#include "extmod/vfs.h"

// ---------- dos.library errno mapping ----------

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

// ---------- file objects (binary + text, sharing one struct) ----------

typedef struct _mp_obj_amiga_file_t {
    mp_obj_base_t base;
    BPTR fh;
    bool readable;
    bool writable;
} mp_obj_amiga_file_t;

extern const mp_obj_type_t mp_type_amiga_fileio;
extern const mp_obj_type_t mp_type_amiga_textio;

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

// Open `fname` with the given mode string; returns a file object suitable
// for return from VFS.open.
static mp_obj_t amiga_open_path(const char *fname, const char *mode_s) {
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
    if (base == 'r') {
        // Python 'r' / 'r+': fail if file doesn't exist. MODE_OLDFILE
        // matches that semantic (and AmigaOS opens are read+write by
        // default, so 'r+' works through the same path). MODE_READWRITE
        // would create-on-missing, which would silently turn 'r+' on a
        // non-existent file into a success — wrong for Python semantics.
        fh = Open((STRPTR)fname, MODE_OLDFILE);
    } else if (base == 'w') {
        // Truncate-or-create.
        fh = Open((STRPTR)fname, MODE_NEWFILE);
    } else {
        // 'a' / 'a+': open existing for read/write, or create. Seek to end.
        fh = Open((STRPTR)fname, MODE_READWRITE);
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

// ---------- VFS object ----------

typedef struct _mp_obj_vfs_amiga_t {
    mp_obj_base_t base;
} mp_obj_vfs_amiga_t;

static mp_obj_t vfs_amiga_make_new(const mp_obj_type_t *type, size_t n_args, size_t n_kw, const mp_obj_t *args) {
    (void)n_args;
    (void)n_kw;
    (void)args;
    mp_obj_vfs_amiga_t *self = mp_obj_malloc(mp_obj_vfs_amiga_t, type);
    return MP_OBJ_FROM_PTR(self);
}

static mp_obj_t vfs_amiga_mount(mp_obj_t self_in, mp_obj_t readonly, mp_obj_t mkfs) {
    (void)self_in;
    (void)readonly;
    (void)mkfs;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_amiga_mount_obj, vfs_amiga_mount);

static mp_obj_t vfs_amiga_umount(mp_obj_t self_in) {
    (void)self_in;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(vfs_amiga_umount_obj, vfs_amiga_umount);

// open(self, path, mode)
static mp_obj_t vfs_amiga_open(mp_obj_t self_in, mp_obj_t path_in, mp_obj_t mode_in) {
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);
    const char *mode = mp_obj_str_get_str(mode_in);
    return amiga_open_path(path, mode);
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_amiga_open_obj, vfs_amiga_open);

// Save the inherited cwd lock on first chdir so we don't UnLock something
// the shell still owns -- the lock that CurrentDir returned to us at that
// point is what AmigaOS handed our process at startup (DupLock'd from the
// shell), and vfs_amiga_cleanup() restores it as current before we exit
// so the user's shell sees the same cwd it had before they ran us.
// Subsequent chdirs replace our own owned lock, so UnLock'ing the previous
// one is correct.
static BPTR vfs_amiga_owned_lock; // 0 = we never chdir'd; cwd is shell-inherited
static BPTR vfs_amiga_inherited_lock; // saved on first chdir

// Called by main.c just before mp_deinit() to put the process's cwd back
// to whatever we inherited from the shell. clib2's exit code would
// otherwise UnLock whatever cwd happens to be current, which may be a
// directory the user's script chdir'd into; restoring the inherited
// lock makes our exit a no-op for the shell.
void vfs_amiga_cleanup(void) {
    if (vfs_amiga_owned_lock) {
        BPTR prev = CurrentDir(vfs_amiga_inherited_lock);
        UnLock(prev);
        vfs_amiga_owned_lock = 0;
        vfs_amiga_inherited_lock = 0;
    }
}

static mp_obj_t vfs_amiga_chdir(mp_obj_t self_in, mp_obj_t path_in) {
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);

    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR new_lock = Lock((STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = saved_wp;
    if (!new_lock) {
        mp_raise_OSError(dos_errno());
    }

    BPTR prev = CurrentDir(new_lock);
    if (vfs_amiga_owned_lock) {
        // We owned the previous cwd lock -- it's safe to free it now.
        UnLock(prev);
    } else {
        // First chdir: prev is the shell-inherited cwd. Hold onto it so
        // we can restore it (or at least not leak it) at exit.
        vfs_amiga_inherited_lock = prev;
    }
    vfs_amiga_owned_lock = new_lock;
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_amiga_chdir_obj, vfs_amiga_chdir);

// getcwd(self): return the current directory as a string.
static mp_obj_t vfs_amiga_getcwd(mp_obj_t self_in) {
    (void)self_in;
    struct Process *me = (struct Process *)FindTask(NULL);
    BPTR cwd = me->pr_CurrentDir;
    char buf[256];
    if (NameFromLock(cwd, (STRPTR)buf, sizeof(buf))) {
        return mp_obj_new_str(buf, strlen(buf));
    }
    // NameFromLock failed (NULL cwd / orphan lock). Fall back to ":".
    return mp_obj_new_str(":", 1);
}
static MP_DEFINE_CONST_FUN_OBJ_1(vfs_amiga_getcwd_obj, vfs_amiga_getcwd);

// stat(self, path): return a 10-tuple (mode, ino, dev, nlink, uid, gid,
// size, atime, mtime, ctime). AmigaDOS doesn't have most of these
// concepts; we synthesise a Unix-ish mode from fib_DirEntryType and
// fib_Protection so callers that key off stat.S_ISDIR() get the right
// answer, and pull file size out of fib_Size.
static mp_obj_t vfs_amiga_stat(mp_obj_t self_in, mp_obj_t path_in) {
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);

    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = saved_wp;
    if (!lock) {
        mp_raise_OSError(dos_errno());
    }
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }
    BOOL ok = Examine(lock, fib);
    bool is_dir = ok && fib->fib_DirEntryType > 0;
    mp_int_t size = ok ? fib->fib_Size : 0;
    FreeDosObject(DOS_FIB, fib);
    UnLock(lock);
    if (!ok) {
        mp_raise_OSError(MP_EIO);
    }

    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(10, NULL));
    // mode: 0o40755 for dirs, 0o100644 for files (Unix S_IFDIR / S_IFREG).
    t->items[0] = MP_OBJ_NEW_SMALL_INT(is_dir ? 0040755 : 0100644);
    t->items[1] = MP_OBJ_NEW_SMALL_INT(0); // ino
    t->items[2] = MP_OBJ_NEW_SMALL_INT(0); // dev
    t->items[3] = MP_OBJ_NEW_SMALL_INT(1); // nlink
    t->items[4] = MP_OBJ_NEW_SMALL_INT(0); // uid
    t->items[5] = MP_OBJ_NEW_SMALL_INT(0); // gid
    t->items[6] = mp_obj_new_int(size);
    t->items[7] = MP_OBJ_NEW_SMALL_INT(0); // atime
    t->items[8] = MP_OBJ_NEW_SMALL_INT(0); // mtime
    t->items[9] = MP_OBJ_NEW_SMALL_INT(0); // ctime
    return MP_OBJ_FROM_PTR(t);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_amiga_stat_obj, vfs_amiga_stat);

// ilistdir(self, path): iterator yielding (name, type, inode) tuples.
// type is the Unix-style file-mode high bits (0x4000 = dir, 0x8000 = file)
// that os.listdir users expect.
typedef struct _vfs_amiga_ilistdir_it_t {
    mp_obj_base_t base;
    mp_fun_1_t iternext;
    mp_fun_1_t finaliser;
    BPTR lock;
    struct FileInfoBlock *fib;
} vfs_amiga_ilistdir_it_t;

static void vfs_amiga_ilistdir_cleanup(vfs_amiga_ilistdir_it_t *it) {
    if (it->fib) {
        FreeDosObject(DOS_FIB, it->fib);
        it->fib = NULL;
    }
    if (it->lock) {
        UnLock(it->lock);
        it->lock = 0;
    }
}

static mp_obj_t vfs_amiga_ilistdir_iternext(mp_obj_t self_in) {
    vfs_amiga_ilistdir_it_t *it = MP_OBJ_TO_PTR(self_in);
    if (!it->lock) {
        return MP_OBJ_STOP_ITERATION;
    }
    if (!ExNext(it->lock, it->fib)) {
        vfs_amiga_ilistdir_cleanup(it);
        return MP_OBJ_STOP_ITERATION;
    }
    mp_obj_tuple_t *t = MP_OBJ_TO_PTR(mp_obj_new_tuple(3, NULL));
    t->items[0] = mp_obj_new_str((char *)it->fib->fib_FileName,
        strlen((char *)it->fib->fib_FileName));
    t->items[1] = MP_OBJ_NEW_SMALL_INT(it->fib->fib_DirEntryType > 0 ? 0x4000 : 0x8000);
    t->items[2] = MP_OBJ_NEW_SMALL_INT(0);
    return MP_OBJ_FROM_PTR(t);
}

static mp_obj_t vfs_amiga_ilistdir_del(mp_obj_t self_in) {
    vfs_amiga_ilistdir_cleanup(MP_OBJ_TO_PTR(self_in));
    return mp_const_none;
}

static mp_obj_t vfs_amiga_ilistdir(mp_obj_t self_in, mp_obj_t path_in) {
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);

    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR lock = Lock((STRPTR)(path[0] ? path : ""), SHARED_LOCK);
    me->pr_WindowPtr = saved_wp;
    if (!lock) {
        mp_raise_OSError(dos_errno());
    }
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    if (!fib) {
        UnLock(lock);
        mp_raise_OSError(MP_ENOMEM);
    }
    if (!Examine(lock, fib)) {
        FreeDosObject(DOS_FIB, fib);
        UnLock(lock);
        mp_raise_OSError(dos_errno());
    }
    vfs_amiga_ilistdir_it_t *it = mp_obj_malloc_with_finaliser(
        vfs_amiga_ilistdir_it_t, &mp_type_polymorph_iter_with_finaliser);
    it->iternext = vfs_amiga_ilistdir_iternext;
    it->finaliser = vfs_amiga_ilistdir_del;
    it->lock = lock;
    it->fib = fib;
    return MP_OBJ_FROM_PTR(it);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_amiga_ilistdir_obj, vfs_amiga_ilistdir);

static mp_obj_t vfs_amiga_mkdir(mp_obj_t self_in, mp_obj_t path_in) {
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);
    BPTR lock = CreateDir((STRPTR)path);
    if (!lock) {
        mp_raise_OSError(dos_errno());
    }
    UnLock(lock);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_amiga_mkdir_obj, vfs_amiga_mkdir);

static mp_obj_t vfs_amiga_remove(mp_obj_t self_in, mp_obj_t path_in) {
    (void)self_in;
    const char *path = mp_obj_str_get_str(path_in);
    if (!DeleteFile((STRPTR)path)) {
        mp_raise_OSError(dos_errno());
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_amiga_remove_obj, vfs_amiga_remove);

static mp_obj_t vfs_amiga_rename(mp_obj_t self_in, mp_obj_t old_in, mp_obj_t new_in) {
    (void)self_in;
    const char *old_path = mp_obj_str_get_str(old_in);
    const char *new_path = mp_obj_str_get_str(new_in);
    if (!Rename((STRPTR)old_path, (STRPTR)new_path)) {
        mp_raise_OSError(dos_errno());
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(vfs_amiga_rename_obj, vfs_amiga_rename);

// rmdir is the same as remove on AmigaDOS -- DeleteFile rejects
// non-empty directories with ERROR_DIRECTORY_NOT_EMPTY.
static mp_obj_t vfs_amiga_rmdir(mp_obj_t self_in, mp_obj_t path_in) {
    return vfs_amiga_remove(self_in, path_in);
}
static MP_DEFINE_CONST_FUN_OBJ_2(vfs_amiga_rmdir_obj, vfs_amiga_rmdir);

// ---------- VFS protocol (import_stat) ----------

static mp_import_stat_t vfs_amiga_import_stat(void *self_in, const char *path) {
    (void)self_in;
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    me->pr_WindowPtr = saved_wp;
    if (!lock) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    mp_import_stat_t result = MP_IMPORT_STAT_NO_EXIST;
    if (fib) {
        if (Examine(lock, fib)) {
            result = (fib->fib_DirEntryType > 0)
                ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return result;
}

static const mp_rom_map_elem_t vfs_amiga_locals_dict_table[] = {
    { MP_ROM_QSTR(MP_QSTR_mount),    MP_ROM_PTR(&vfs_amiga_mount_obj) },
    { MP_ROM_QSTR(MP_QSTR_umount),   MP_ROM_PTR(&vfs_amiga_umount_obj) },
    { MP_ROM_QSTR(MP_QSTR_open),     MP_ROM_PTR(&vfs_amiga_open_obj) },
    { MP_ROM_QSTR(MP_QSTR_chdir),    MP_ROM_PTR(&vfs_amiga_chdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_getcwd),   MP_ROM_PTR(&vfs_amiga_getcwd_obj) },
    { MP_ROM_QSTR(MP_QSTR_ilistdir), MP_ROM_PTR(&vfs_amiga_ilistdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_mkdir),    MP_ROM_PTR(&vfs_amiga_mkdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_remove),   MP_ROM_PTR(&vfs_amiga_remove_obj) },
    { MP_ROM_QSTR(MP_QSTR_rename),   MP_ROM_PTR(&vfs_amiga_rename_obj) },
    { MP_ROM_QSTR(MP_QSTR_rmdir),    MP_ROM_PTR(&vfs_amiga_rmdir_obj) },
    { MP_ROM_QSTR(MP_QSTR_stat),     MP_ROM_PTR(&vfs_amiga_stat_obj) },
};
static MP_DEFINE_CONST_DICT(vfs_amiga_locals_dict, vfs_amiga_locals_dict_table);

static const mp_vfs_proto_t vfs_amiga_proto = {
    .import_stat = vfs_amiga_import_stat,
};

MP_DEFINE_CONST_OBJ_TYPE(
    mp_type_vfs_amiga,
    MP_QSTR_VfsAmiga,
    MP_TYPE_FLAG_NONE,
    make_new, vfs_amiga_make_new,
    protocol, &vfs_amiga_proto,
    locals_dict, &vfs_amiga_locals_dict
    );
