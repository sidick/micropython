#include <time.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"

// Called by MICROPY_VM_HOOK_LOOP every 1024 bytecodes.
void amiga_check_ctrl_c(void) {
    if (CheckSignal(SIGBREAKF_CTRL_C)) {
        mp_sched_keyboard_interrupt();
    }
}

int mp_hal_stdin_rx_chr(void) {
    BPTR in = Input();
    for (;;) {
        // Wait up to 200 ms for a character, checking Ctrl+C between polls.
        if (WaitForChar(in, 200000L)) {
            LONG c = FGetC(in);
            if (c < 0) {
                return 4;  // EOF -> Ctrl+D
            }
            if (c == 28) {
                return 4;  // Ctrl+\ -> Ctrl+D (AmigaOS EOF convention)
            }
            if (c == mp_interrupt_char) {
                mp_sched_keyboard_interrupt();
            }
            return (int)c;
        }
        // Poll Ctrl+C between WaitForChar timeouts.
        amiga_check_ctrl_c();
    }
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    LONG written = Write(Output(), (APTR)str, (LONG)len);
    return written < 0 ? 0 : (mp_uint_t)written;
}

// Use dos.library Delay() (50 Hz ticks) for millisecond delays.
// This yields to other AmigaOS tasks rather than busy-waiting.
void mp_hal_delay_ms(mp_uint_t ms) {
    if (ms >= 20) {
        Delay((LONG)((ms + 9) / 20));
    }
    // Sub-20ms remainder: too short for Delay(); just return.
    // For accurate sub-tick delays, timer.device would be needed.
}

void mp_hal_delay_us(mp_uint_t us) {
    // Delay() has 20ms resolution; busy-wait with clock() for short delays.
    clock_t end = clock() + (clock_t)us * (CLOCKS_PER_SEC / 1000000L);
    while (clock() < end) {
    }
}
