// time module wall-clock backing for the Amiga port (Phase 39 step 1).
//
// Included into extmod/modtime.c via MICROPY_PY_TIME_INCLUDEFILE. Supplies
// the three hooks the include-file contract expects:
//
//   * mp_time_localtime_get(tm)  -- fills timeutils_struct_time_t
//   * mp_time_time_get()         -- returns time.time() value (float)
//   * mp_hal_time_ns()           -- declared in py/mphal.h
//
// Source of truth is timer.device GetSysTime(), already opened at startup
// by amiga_timer.c for the ticks_*/delay_* HAL. Resolution is microseconds
// and the clock tracks the system time set by setclock/Date/Locale (so a
// user who has run `Date` since boot sees the updated value here).
//
// battclock.resource is deliberately *not* used: at boot the system reads
// battclock once and sets the AmigaDOS system clock from it; thereafter
// the system clock is authoritative. Reading battclock directly during
// runtime would race the boot synchronisation and skip any user-side
// `Date` edits.

#include <stdint.h>
#include <stddef.h>

#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "py/obj.h"
#include "py/mphal.h"
#include "shared/timeutils/timeutils.h"

// AmigaOS counts seconds from 1978-01-01 00:00:00; CPython / Unix from
// 1970-01-01. 8 years = 2 leap (1972, 1976) + 6 non-leap = 2922 days.
#define AMIGA_EPOCH_TO_UNIX_EPOCH_SECONDS  (2922UL * 86400UL)

// TimerBase is published by amiga_timer.c once OpenDevice("timer.device", ...)
// succeeds. amiga_timer_open() runs from main() before mp_init(), so by the
// time any Python code calls into the time module TimerBase is non-NULL on
// any system that booted normally; the NULL fallback below is defensive.
extern struct Device *TimerBase;

static void amiga_get_unix_time(uint32_t *secs_out, uint32_t *micro_out) {
    if (TimerBase == NULL) {
        *secs_out = AMIGA_EPOCH_TO_UNIX_EPOCH_SECONDS;
        *micro_out = 0;
        return;
    }
    struct timeval tv;
    GetSysTime(&tv);
    *secs_out = (uint32_t)tv.tv_secs + AMIGA_EPOCH_TO_UNIX_EPOCH_SECONDS;
    *micro_out = (uint32_t)tv.tv_micro;
}

static void mp_time_localtime_get(timeutils_struct_time_t *tm) {
    uint32_t secs, micro;
    amiga_get_unix_time(&secs, &micro);
    (void)micro;
    timeutils_seconds_since_1970_to_struct_time((timeutils_timestamp_t)secs, tm);
}

static mp_obj_t mp_time_time_get(void) {
    uint32_t secs, micro;
    amiga_get_unix_time(&secs, &micro);
    #if MICROPY_PY_BUILTINS_FLOAT && MICROPY_FLOAT_IMPL == MICROPY_FLOAT_IMPL_DOUBLE
    return mp_obj_new_float((mp_float_t)secs + (mp_float_t)micro / MICROPY_FLOAT_CONST(1000000.0));
    #else
    return timeutils_obj_from_timestamp((mp_timestamp_t)secs);
    #endif
}

uint64_t mp_hal_time_ns(void) {
    uint32_t secs, micro;
    amiga_get_unix_time(&secs, &micro);
    return (uint64_t)secs * 1000000000ULL + (uint64_t)micro * 1000ULL;
}
