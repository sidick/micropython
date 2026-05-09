#include <stdio.h>
#include "py/mphal.h"
#include "py/runtime.h"

// Currently uses newlib stdio so we can build without the AmigaOS NDK.
// Once the NDK is available, replace with:
//   FGetC(Input())          for stdin
//   Write(Output(), ...)    for stdout

int mp_hal_stdin_rx_chr(void) {
    int c = getchar();
    // AmigaOS uses Ctrl+\ (ASCII 28) for EOF; map it to Ctrl+D (ASCII 4)
    // which MicroPython's readline uses as the REPL exit signal.
    if (c == 28) {
        c = 4;
    }
    return c;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    mp_uint_t ret = fwrite(str, 1, len, stdout);
    fflush(stdout);
    return ret;
}

