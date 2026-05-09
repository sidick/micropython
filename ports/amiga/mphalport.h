#pragma once

#include <time.h>
#include "py/mpconfig.h"

// Millisecond ticks via ANSI clock().
// Replace with DateStamp() from dos.library once the NDK is installed.
static inline mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)(clock() * 1000 / CLOCKS_PER_SEC);
}

// Ctrl-C interrupt character hook — wire to AmigaOS SIGBREAKF_CTRL_C later.
static inline void mp_hal_set_interrupt_char(int c) {
    (void)c;
}
