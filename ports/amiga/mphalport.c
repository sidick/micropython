/*
 * This file is part of the MicroPython project, http://micropython.org/
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2026 Simon Dick
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>
#include "py/mphal.h"
#include "py/runtime.h"
#include "shared/runtime/interrupt_char.h"

// Initial GC heap size, captured by main.c after argv/env/tooltype
// parsing and read by the banner injector below.
extern size_t amiga_initial_heap;

// Called by MICROPY_VM_HOOK_LOOP every 1024 bytecodes.
void amiga_check_ctrl_c(void) {
    if (CheckSignal(SIGBREAKF_CTRL_C)) {
        mp_sched_keyboard_interrupt();
    }
}

// One-slot push-back buffer used by the AmigaOS CSI translation
// (0x9b -> ESC '[') in mp_hal_stdin_rx_chr.
static int amiga_stdin_pending = -1;

// Armed by main.c before entering the REPL. The first call to
// mp_hal_stdin_rx_chr after this flag is set silently discards a leading
// CR or LF (the leftover from the shell's "micropython<Enter>" command
// line). The flag is consumed on the first real call regardless of
// whether the discard fired.
static bool amiga_stdin_first_nl_skip = false;

void amiga_arm_stdin_first_nl_skip(void) {
    amiga_stdin_first_nl_skip = true;
}

int mp_hal_stdin_rx_chr(void) {
    // AmigaOS console emits CSI sequences as a single byte 0x9B followed by
    // parameters, where xterm-style consoles use ESC '['. shared/readline/
    // only understands the latter, so when we see 0x9B we return ESC now and
    // hand '[' to the next call. Other bytes pass through unchanged, so
    // hosts that already emit ESC '[' (e.g. vamos's host xterm pass-through)
    // are unaffected.
    if (amiga_stdin_pending >= 0) {
        int c = amiga_stdin_pending;
        amiga_stdin_pending = -1;
        return c;
    }

    // FGetC blocks efficiently (suspends the task) until a key arrives.
    // In raw mode, Ctrl+C arrives as ASCII 3 so no separate break-signal
    // poll is needed here; the VM hook handles Ctrl+C during computation.
    LONG c = FGetC(Input());
    if (amiga_stdin_first_nl_skip) {
        amiga_stdin_first_nl_skip = false;
        if (c == '\r' || c == '\n') {
            c = FGetC(Input());
        }
    }
    if (c < 0 || c == 28) {
        return 4;  // EOF or Ctrl+\ -> Ctrl+D
    }
    if (c == 0x9b) {
        amiga_stdin_pending = '[';
        return 0x1b;  // ESC
    }
    if ((int)c == mp_interrupt_char) {
        mp_sched_keyboard_interrupt();
    }
    return (int)c;
}

// One-shot banner injection: when armed, fires after the next N writes.
// Used by main.c to slot a Heap-info line into the upstream
// pyexec_friendly_repl banner without modifying shared code. pyexec
// emits three writes -- MICROPY_BANNER_NAME_AND_VERSION, "; <machine>",
// "\r\n" -- before the "Type help()" line, so arming with N=3 lands
// the inject between them. If pyexec ever changes the write count,
// update the call site.
static int amiga_banner_inject_countdown = 0;

void amiga_arm_banner_inject(int after_n_writes) {
    amiga_banner_inject_countdown = after_n_writes;
}

mp_uint_t mp_hal_stdout_tx_strn(const char *str, size_t len) {
    // shared/readline emits the 3-byte CSI "erase line from cursor"
    // (ESC [ K) after backing the cursor up to delete characters. On
    // AmigaOS the console wraps the cursor up across row boundaries
    // when it's stepped past column 0, so deleting characters that had
    // wrapped to a second screen row lands the cursor back at the
    // start of input on the first row -- and ESC [ K only clears that
    // first row, leaving the wrap-overflow visible below.
    //
    // Substitute ESC [ J (erase from cursor to end of screen) so the
    // wrap-overflow rows are wiped too. The REPL is the only thing in
    // the CON: window so there's nothing below the prompt that we'd
    // accidentally clobber.
    if (len == 3 && str[0] == '\x1b' && str[1] == '[' && str[2] == 'K') {
        str = "\x1b[J";
    }
    LONG written = Write(Output(), (APTR)str, (LONG)len);
    if (amiga_banner_inject_countdown > 0) {
        if (--amiga_banner_inject_countdown == 0) {
            char buf[80];
            int n = snprintf(buf, sizeof(buf),
                "Heap: %luK initial; system free: %luK\r\n",
                (unsigned long)(amiga_initial_heap / 1024),
                (unsigned long)(AvailMem(MEMF_ANY) / 1024));
            if (n > 0 && n < (int)sizeof(buf)) {
                Write(Output(), (APTR)buf, (LONG)n);
            }
        }
    }
    return written < 0 ? 0 : (mp_uint_t)written;
}

// mp_hal_delay_ms / mp_hal_delay_us live in amiga_timer.c (Phase 23 —
// timer.device + ReadEClock).
