#include <stdio.h>

#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/dos.h>

#include "py/lexer.h"
#include "py/builtin.h"
#include "py/runtime.h"
#include "py/mperrno.h"

mp_lexer_t *mp_lexer_new_from_file(qstr filename) {
    const char *path = qstr_str(filename);
    FILE *f = fopen(path, "rb");
    if (!f) {
        mp_raise_OSError(MP_ENOENT);
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    char *buf = m_new(char, size + 1);
    if (fread(buf, 1, size, f) != (size_t)size) {
        fclose(f);
        mp_raise_OSError(MP_EIO);
    }
    fclose(f);
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
