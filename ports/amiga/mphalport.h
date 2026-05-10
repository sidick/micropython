#pragma once

#include <time.h>
#include "py/mpconfig.h"
#include "shared/runtime/interrupt_char.h"

// Ticks via ANSI clock(). Replace with DateStamp() once NDK is installed.
static inline mp_uint_t mp_hal_ticks_ms(void) {
    return (mp_uint_t)(clock() * 1000 / CLOCKS_PER_SEC);
}

static inline mp_uint_t mp_hal_ticks_us(void) {
    return (mp_uint_t)(clock() * 1000000 / CLOCKS_PER_SEC);
}

static inline mp_uint_t mp_hal_ticks_cpu(void) {
    return (mp_uint_t)clock();
}

// Called by the VM hook every N bytecodes to check for Ctrl+C.
void amiga_check_ctrl_c(void);
