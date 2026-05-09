#include <stdio.h>

#include "py/lexer.h"
#include "py/builtin.h"
#include "py/runtime.h"
#include "py/mperrno.h"

// Load a Python source file for the import system.
// Uses newlib fopen/fread for now; replace with dos.library Open/Read
// once the NDK is installed.
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

// Probe a path so the import machinery can distinguish files, directories,
// and missing paths. AmigaOS clib2 lacks POSIX stat(), so we use fopen to
// detect files. Directory detection is not yet supported (returns NO_EXIST);
// package imports will be added once the NDK is available via Lock/Examine.
mp_import_stat_t mp_import_stat(const char *path) {
    FILE *f = fopen(path, "rb");
    if (f) {
        fclose(f);
        return MP_IMPORT_STAT_FILE;
    }
    return MP_IMPORT_STAT_NO_EXIST;
}
