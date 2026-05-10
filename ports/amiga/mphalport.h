#pragma once

#include <stdbool.h>
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

// Defined in main.c, set by `-X compile-only`. shared/runtime/pyexec.c
// references this when MICROPY_PYEXEC_COMPILE_ONLY is enabled, so the
// declaration must be visible from there. mphalport.h reaches everywhere
// pyexec.c does, matching the unix port's pattern.
#if MICROPY_PYEXEC_COMPILE_ONLY
extern bool mp_compile_only;
#endif
