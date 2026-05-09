#include <stdio.h>
#include "py/mphal.h"
#include "py/runtime.h"
#include "py/objstr.h"

// Currently uses newlib stdio so we can build without the AmigaOS NDK.
// Once the NDK is available, replace with:
//   FGetC(Input())          for stdin
//   Write(Output(), ...)    for stdout

int mp_hal_stdin_rx_chr(void) {
    return getchar();
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    return fwrite(str, 1, len, stdout);
}

// Stub for the Python built-in open().
// modio.c references this symbol — it must be supplied by the port.
// Replace with a real implementation when file I/O is wired up in phase 2.
static mp_obj_t mp_builtin_open(size_t n_args, const mp_obj_t *args, mp_map_t *kwargs) {
    mp_raise_NotImplementedError(MP_ERROR_TEXT("open() not yet implemented"));
}
MP_DEFINE_CONST_FUN_OBJ_KW(mp_builtin_open_obj, 1, mp_builtin_open);
