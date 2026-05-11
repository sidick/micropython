// timer.device-backed timing for the Amiga port (Phase 23).
//
// Replaces the Phase 8 newlib clock()-based mp_hal_ticks_* / mp_hal_delay_us
// with two AmigaOS primitives:
//
//   * ReadEClock()        -- sub-microsecond monotonic counter (the E-Clock
//                            ticks at ~709 kHz PAL / ~715 kHz NTSC, sourced
//                            from the same xtal as CIA-B). Used for ticks_*
//                            and for sub-millisecond busy-wait delays where
//                            the IORequest round-trip would dominate.
//   * timer.device        -- UNIT_MICROHZ + TR_ADDREQUEST. DoIO() suspends
//                            the task until the deadline, so other tasks
//                            (Workbench, dos.library, the CIA timer ISR
//                            itself) keep running. Used for delays >=
//                            BUSY_WAIT_THRESHOLD_US.
//
// The IORequest object is single-instance; the port is single-threaded so
// reusing it across calls is fine. ReadEClock() requires TimerBase to be
// non-NULL, which is set up here from io_Device after OpenDevice succeeds.

#include <stdbool.h>
#include <stdint.h>

#include <exec/io.h>
#include <exec/ports.h>
#include <devices/timer.h>
#include <proto/exec.h>
#include <proto/timer.h>

#include "py/mphal.h"

// Library base referenced by the bebbo proto/timer.h inlines (ReadEClock,
// etc.). The inline macros expand to a -60(a6) jump through this pointer.
// Filled in below after OpenDevice("timer.device", ...).
struct Device *TimerBase = NULL;

static struct MsgPort      *timer_port = NULL;
static struct timerequest  *timer_req  = NULL;
static ULONG                eclock_freq = 0;

// IO request round-trip overhead is several microseconds (msgport reply,
// task wake, signal poll). For delays below this we busy-wait against the
// E-Clock instead: same effective accuracy, no scheduler hop.
#define BUSY_WAIT_THRESHOLD_US 200

bool amiga_timer_open(void) {
    timer_port = CreateMsgPort();
    if (timer_port == NULL) {
        return false;
    }
    timer_req = (struct timerequest *)CreateIORequest(
        timer_port, sizeof(*timer_req));
    if (timer_req == NULL) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
        return false;
    }
    if (OpenDevice((CONST_STRPTR)"timer.device", UNIT_MICROHZ,
                   (struct IORequest *)timer_req, 0) != 0) {
        DeleteIORequest((struct IORequest *)timer_req);
        DeleteMsgPort(timer_port);
        timer_req = NULL;
        timer_port = NULL;
        return false;
    }
    TimerBase = timer_req->tr_node.io_Device;

    // Cache the E-Clock frequency. ReadEClock returns it on every call but
    // we hit this on the hot path of ticks_* / delay_us, so a stored copy
    // is worth the 8 bytes.
    struct EClockVal ev;
    eclock_freq = ReadEClock(&ev);
    return true;
}

void amiga_timer_close(void) {
    if (timer_req != NULL) {
        // All TR_ADDREQUEST traffic goes through DoIO (synchronous), so
        // there's never an in-flight request to abort here.
        CloseDevice((struct IORequest *)timer_req);
        DeleteIORequest((struct IORequest *)timer_req);
        timer_req = NULL;
    }
    if (timer_port != NULL) {
        DeleteMsgPort(timer_port);
        timer_port = NULL;
    }
    TimerBase = NULL;
    eclock_freq = 0;
}

static inline uint64_t eclock_ticks(void) {
    struct EClockVal ev;
    ReadEClock(&ev);
    return ((uint64_t)ev.ev_hi << 32) | ev.ev_lo;
}

mp_uint_t mp_hal_ticks_ms(void) {
    if (eclock_freq == 0) {
        return 0;
    }
    return (mp_uint_t)((eclock_ticks() * 1000ULL) / eclock_freq);
}

mp_uint_t mp_hal_ticks_us(void) {
    if (eclock_freq == 0) {
        return 0;
    }
    return (mp_uint_t)((eclock_ticks() * 1000000ULL) / eclock_freq);
}

mp_uint_t mp_hal_ticks_cpu(void) {
    // Best proxy for a CPU-cycle-resolution counter on m68k. Returns the
    // low half of the E-Clock value directly so callers see the cheapest
    // possible monotonic counter; wraps at 2^32 ticks (~1.7 h @ 709 kHz).
    if (eclock_freq == 0) {
        return 0;
    }
    struct EClockVal ev;
    ReadEClock(&ev);
    return (mp_uint_t)ev.ev_lo;
}

void mp_hal_delay_us(mp_uint_t us) {
    if (us == 0 || eclock_freq == 0) {
        return;
    }
    if (us < BUSY_WAIT_THRESHOLD_US) {
        // Convert us -> eclock ticks, rounding up so we never under-wait.
        uint64_t now = eclock_ticks();
        uint64_t wait = ((uint64_t)us * eclock_freq + 999999ULL) / 1000000ULL;
        uint64_t deadline = now + wait;
        while (eclock_ticks() < deadline) {
        }
        return;
    }
    timer_req->tr_node.io_Command = TR_ADDREQUEST;
    timer_req->tr_time.tv_secs    = us / 1000000U;
    timer_req->tr_time.tv_micro   = us % 1000000U;
    DoIO((struct IORequest *)timer_req);
}

void mp_hal_delay_ms(mp_uint_t ms) {
    if (ms == 0) {
        return;
    }
    if (eclock_freq == 0) {
        // Pre-init fallback: dos.library Delay() at 50 Hz granularity.
        // Should not happen in practice (amiga_timer_open is called before
        // mp_init), but keeps the HAL contract intact.
        if (ms >= 20) {
            Delay((LONG)((ms + 9) / 20));
        }
        return;
    }
    timer_req->tr_node.io_Command = TR_ADDREQUEST;
    timer_req->tr_time.tv_secs    = ms / 1000U;
    timer_req->tr_time.tv_micro   = (ms % 1000U) * 1000U;
    DoIO((struct IORequest *)timer_req);
}
