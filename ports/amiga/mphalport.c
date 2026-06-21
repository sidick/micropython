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

// MICROPY_INTERNAL_WFE backend: mp_event_wait_ms()/_indefinite() call this
// to suspend until the next event. Without it the default is a no-op, so
// select()/poll() (extmod/modselect.c) busy-spin while waiting. Sleep in
// short slices instead: a slice is capped so the poll loop re-checks socket
// readiness, its own timeout, and Ctrl-C at least every ~40 ms, and an
// "indefinite" (mp_uint_t)-1 wait can never hang.
void amiga_internal_wfe(mp_uint_t timeout_ms) {
    if (timeout_ms == 0) {
        return; // caller only wanted pending events serviced
    }
    ULONG ticks = (ULONG)(timeout_ms / 20); // dos Delay(): 50 ticks/second
    if (ticks < 1) {
        ticks = 1; // ~20 ms minimum slice
    } else if (ticks > 2) {
        ticks = 2; // ~40 ms cap
    }
    Delay(ticks);
    amiga_check_ctrl_c();
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

// Set when FGetC() reports a genuine end-of-stream (a serial-client
// disconnect over the TCP console, or a closed pipe), as opposed to a
// deliberate Ctrl-D / Ctrl-\ keystroke. Both surface to pyexec as Ctrl-D,
// but only the real keystroke should soft-reset the raw REPL; a disconnect
// must let the REPL loop exit cleanly rather than spin re-reading EOF. The
// soft-reset loop in main.c consults this via amiga_stdin_hit_eof().
static bool amiga_stdin_eof = false;

bool amiga_stdin_hit_eof(void) {
    bool e = amiga_stdin_eof;
    amiga_stdin_eof = false;
    return e;
}

// Bulk-buffered serial input. The console is in raw mode during the REPL
// (main.c SetMode(stdin_fh, 1)), so Read() returns the bytes currently
// available rather than waiting for a line terminator. FGetC's byte-at-a-
// time path is a DOS packet round-trip per character on an interactive
// (unbuffered) stream, too slow to drain the serial RX during a fast burst
// -- the flow-control-less TCP link then overruns the emulated UART and
// silently drops bytes, which is what garbled test source mid-run over
// run-tests.py -t. Pulling the whole available burst into our own buffer in
// one Read() drains fast enough to keep up at much higher line speeds.
//
// Default buffer: 1 KiB (compile-time constant). One Read() can sweep up to
// this many already-queued bytes per refill; larger just means fewer refills
// on a big burst, smaller costs more refills -- 1 KiB comfortably covers the
// serial RX at the paced line speeds this port uses.
#define AMIGA_RX_BUF 1024
static unsigned char amiga_rx_buf[AMIGA_RX_BUF];
static int amiga_rx_len;   // bytes currently in amiga_rx_buf
static int amiga_rx_pos;   // next byte to hand out

// Block for at least one byte, then sweep up everything else already
// buffered without blocking. Read(Input(), buf, n) on the raw console
// returns the bytes available now (<= n); WaitForChar(.., 0) gates the
// follow-up reads so we never block waiting to fill the whole buffer.
// Returns 0 on success, -1 on genuine EOF / error.
static int amiga_rx_refill(void) {
    LONG n = Read(Input(), amiga_rx_buf, 1);
    if (n <= 0) {
        return -1;
    }
    int len = (int)n;
    while (len < AMIGA_RX_BUF && WaitForChar(Input(), 0)) {
        n = Read(Input(), amiga_rx_buf + len, AMIGA_RX_BUF - len);
        if (n <= 0) {
            break;
        }
        len += (int)n;
    }
    amiga_rx_len = len;
    amiga_rx_pos = 0;
    return 0;
}

static int amiga_rx_byte(void) {
    if (amiga_rx_pos >= amiga_rx_len) {
        if (amiga_rx_refill() < 0) {
            return -1;
        }
    }
    return amiga_rx_buf[amiga_rx_pos++];
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

    int c = amiga_rx_byte();
    if (amiga_stdin_first_nl_skip) {
        amiga_stdin_first_nl_skip = false;
        if (c == '\r' || c == '\n') {
            c = amiga_rx_byte();
        }
    }
    if (c < 0) {
        amiga_stdin_eof = true;  // genuine end-of-stream / client disconnect
        return 4;  // -> Ctrl+D
    }
    if (c == 28) {
        return 4;  // Ctrl+\ -> Ctrl+D (deliberate keystroke, not a disconnect)
    }
    if (c == 0x9b) {
        amiga_stdin_pending = '[';
        return 0x1b;  // ESC
    }
    if (c == mp_interrupt_char) {
        mp_sched_keyboard_interrupt();
    }
    return c;
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
