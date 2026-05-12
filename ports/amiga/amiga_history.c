// Phase 24: persistent REPL history.
//
// Reads the readline ring back from disk on startup so ↑ recalls
// commands from a previous session, and writes it back out at exit.
//
// The default path is `S:MicroPython.history` — `S:` is the
// conventional AmigaOS spot for shell-style user dot-files (it sits
// alongside `Shell-Startup` and `user-startup`), survives reboots,
// and avoids polluting `ENVARC:` (which is for preferences, not
// frequently-written logs).  Override with the `MICROPYHISTORY`
// environment variable, read at startup via dos.library `GetVar`.
//
// File format: one history entry per line, oldest first, plain
// AmigaDOS line endings.  Bumping `MICROPY_READLINE_HISTORY_SIZE`
// only changes how much we keep; old files with more entries truncate
// naturally on the next save.

#include <string.h>

#include <dos/dos.h>
#include <proto/dos.h>
#include <proto/exec.h>

#include "py/runtime.h"
#include "py/mpstate.h"
#include "shared/readline/readline.h"

#define AMIGA_HISTORY_DEFAULT_PATH "S:MicroPython.history"
#define AMIGA_HISTORY_PATH_MAX     256
#define AMIGA_HISTORY_LINE_MAX     512

static void amiga_history_resolve_path(char *out, size_t cap) {
    // `MICROPYHISTORY` overrides the default.  GetVar(flags=0) is a
    // local var; falls back to ENV:/ENVARC: per dos.library convention
    // so the override semantics match Phase 14's `MICROPYHEAP`.
    LONG n = GetVar((STRPTR)"MICROPYHISTORY", (STRPTR)out, cap, 0);
    if (n > 0) {
        // GetVar may include a trailing newline (it does so for ENV:
        // files); trim it.
        size_t len = (size_t)n;
        while (len > 0 && (out[len - 1] == '\n' || out[len - 1] == '\r')) {
            out[--len] = '\0';
        }
        return;
    }
    strncpy(out, AMIGA_HISTORY_DEFAULT_PATH, cap - 1);
    out[cap - 1] = '\0';
}

// Read one line from `fh` into `buf` (NUL-terminated, max cap-1 chars,
// LF/CRLF stripped).  Returns the number of bytes read into buf, or 0
// on EOF / no-bytes, or -1 on I/O error.
static int amiga_history_read_line(BPTR fh, char *buf, size_t cap) {
    size_t pos = 0;
    while (pos < cap - 1) {
        char c;
        LONG got = Read(fh, &c, 1);
        if (got < 0) {
            return -1;
        }
        if (got == 0) {
            // EOF
            if (pos == 0) {
                return 0;
            }
            break;
        }
        if (c == '\n') {
            break;
        }
        if (c == '\r') {
            continue;  // tolerate CRLF
        }
        buf[pos++] = c;
    }
    buf[pos] = '\0';
    return (int)pos;
}

void amiga_history_load(void) {
    char path[AMIGA_HISTORY_PATH_MAX];
    amiga_history_resolve_path(path, sizeof(path));

    // Suppress AmigaDOS auto-requesters on the Open (e.g. if `S:` is
    // on a temporarily-absent volume); we want a silent no-op, not a
    // "Please insert volume" pop-up.
    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR fh = Open((STRPTR)path, MODE_OLDFILE);
    me->pr_WindowPtr = saved_wp;
    if (fh == 0) {
        return;  // no history file yet; first run
    }
    char line[AMIGA_HISTORY_LINE_MAX];
    for (;;) {
        int n = amiga_history_read_line(fh, line, sizeof(line));
        if (n <= 0) {
            break;
        }
        readline_push_history(line);
    }
    Close(fh);
}

void amiga_history_save(void) {
    char path[AMIGA_HISTORY_PATH_MAX];
    amiga_history_resolve_path(path, sizeof(path));

    struct Process *me = (struct Process *)FindTask(NULL);
    APTR saved_wp = me->pr_WindowPtr;
    me->pr_WindowPtr = (APTR)-1;
    BPTR fh = Open((STRPTR)path, MODE_NEWFILE);
    me->pr_WindowPtr = saved_wp;
    if (fh == 0) {
        return;  // can't write — silently skip
    }
    // readline_hist[0] is the most recent entry; write in reverse so
    // the file is chronological.  On reload, readline_push_history()
    // builds the ring back up in correct most-recent-first order.
    for (int i = MICROPY_READLINE_HISTORY_SIZE - 1; i >= 0; i--) {
        const char *entry = MP_STATE_PORT(readline_hist)[i];
        if (entry == NULL || entry[0] == '\0') {
            continue;
        }
        Write(fh, (APTR)entry, (LONG)strlen(entry));
        Write(fh, (APTR)"\n", 1);
    }
    Close(fh);
}
