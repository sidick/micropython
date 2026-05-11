#pragma once

#include <stdbool.h>
#include "py/mpconfig.h"
#include "shared/runtime/interrupt_char.h"

// Timing HAL: backed by AmigaOS timer.device / ReadEClock (see amiga_timer.c).
// Defined non-inline because the implementations do 64-bit math against a
// cached E-Clock frequency; inlining wouldn't help on m68k.
mp_uint_t mp_hal_ticks_ms(void);
mp_uint_t mp_hal_ticks_us(void);
mp_uint_t mp_hal_ticks_cpu(void);

// Called by the VM hook every N bytecodes to check for Ctrl+C.
void amiga_check_ctrl_c(void);

// timer.device lifecycle (Phase 23). main.c calls these around mp_init() /
// mp_deinit().
bool amiga_timer_open(void);
void amiga_timer_close(void);

// Defined in main.c, set by `-X compile-only`. shared/runtime/pyexec.c
// references this when MICROPY_PYEXEC_COMPILE_ONLY is enabled, so the
// declaration must be visible from there. mphalport.h reaches everywhere
// pyexec.c does, matching the unix port's pattern.
#if MICROPY_PYEXEC_COMPILE_ONLY
extern bool mp_compile_only;
#endif
