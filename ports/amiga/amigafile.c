#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

#include "py/lexer.h"
#include "py/builtin.h"
#include "py/runtime.h"
#include "py/mperrno.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *path = qstr_str(filename);
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    if (!fh) {
        mp_raise_OSError(MP_ENOENT);
    }
    // Seek to end to get file size; Seek() to OFFSET_BEGINNING returns old pos = size.
    Seek(fh, 0, OFFSET_END);
    LONG size = Seek(fh, 0, OFFSET_BEGINNING);
    if (size < 0) {
        Close(fh);
        mp_raise_OSError(MP_EIO);
    }
    char *buf = m_new(char, size + 1);
    if (Read(fh, buf, size) != size) {
        Close(fh);
        mp_raise_OSError(MP_EIO);
    }
    Close(fh);
    buf[size] = '\0';
    return mp_lexer_new_from_str_len(filename, buf, (size_t)size, true);
}

// Use dos.library Lock/Examine to distinguish files, directories, and missing
// paths. fib_DirEntryType > 0 means directory, < 0 means file.
mp_import_stat_t mp_import_stat(const char *path) {
    BPTR lock = Lock((STRPTR)path, SHARED_LOCK);
    if (!lock) {
        return MP_IMPORT_STAT_NO_EXIST;
    }
    struct FileInfoBlock *fib = (struct FileInfoBlock *)AllocDosObject(DOS_FIB, NULL);
    mp_import_stat_t result = MP_IMPORT_STAT_NO_EXIST;
    if (fib) {
        if (Examine(lock, fib)) {
            result = (fib->fib_DirEntryType > 0) ? MP_IMPORT_STAT_DIR : MP_IMPORT_STAT_FILE;
        }
        FreeDosObject(DOS_FIB, fib);
    }
    UnLock(lock);
    return result;
}
