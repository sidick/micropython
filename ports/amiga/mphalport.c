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
    // AmigaOS console emits CSI sequences as a single byte 0x9B followed by
    // parameters, where xterm-style consoles use ESC '['. shared/readline/
    // only understands the latter, so when we see 0x9B we return ESC now and
    // hand '[' to the next call. Other bytes pass through unchanged, so
    // hosts that already emit ESC '[' (e.g. vamos's host xterm pass-through)
    // are unaffected.
    static int pending = -1;
    if (pending >= 0) {
        int c = pending;
        pending = -1;
        return c;
    }

    // FGetC blocks efficiently (suspends the task) until a key arrives.
    // In raw mode, Ctrl+C arrives as ASCII 3 so no separate break-signal
    // poll is needed here; the VM hook handles Ctrl+C during computation.
    LONG c = FGetC(Input());
    if (c < 0 || c == 28) {
        return 4;  // EOF or Ctrl+\ -> Ctrl+D
    }
    if (c == 0x9b) {
        pending = '[';
        return 0x1b;  // ESC
    }
    if ((int)c == mp_interrupt_char) {
        mp_sched_keyboard_interrupt();
    }
    return (int)c;
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
