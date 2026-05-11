#!/bin/bash
# tools/amiga-vamos-repl.sh — launch the Amiga port's REPL under vamos
# from an interactive host terminal.
#
# The Amiga binary asks for raw console input via SetMode(stdin, 1), but
# vamos doesn't translate that into tcsetattr() on the host TTY. The
# result is that on macOS Terminal / iTerm / xterm the TTY stays in
# cooked mode: cursor keys are echoed literally as ^[[D and bytes are
# only delivered to vamos on newline, so readline never gets to do its
# job. (Pipe input is unaffected; only interactive sessions hit this.)
#
# This wrapper flips the host TTY into raw/no-echo before launching
# vamos and restores the original mode on exit, regardless of how the
# REPL was terminated. From the binary's side everything looks the same
# as a real Amiberry/AmigaOS console.

set -e

if [ ! -t 0 ]; then
    echo "amiga-vamos-repl.sh: stdin is not a tty; use vamos directly for pipe input." >&2
    exit 2
fi

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MPY_BIN="$REPO_ROOT/ports/amiga/build/micropython"

if [ ! -x "$MPY_BIN" ]; then
    echo "amiga-vamos-repl.sh: $MPY_BIN not built. Run 'make' in ports/amiga first." >&2
    exit 1
fi

# Snapshot the current TTY mode so we can restore it on exit. Also reset
# the screen line so a partial REPL prompt doesn't get clobbered if the
# user kills the program mid-line.
ORIG_STTY="$(stty -g)"
restore_tty() {
    stty "$ORIG_STTY"
    printf '\r\n'
}
trap restore_tty EXIT INT TERM

# -icanon : deliver bytes as typed (no line buffering)
# -echo   : don't echo; readline does its own echo + redraw
# -isig   : don't translate ^C/^Z into signals — the binary handles ^C
#           itself via mp_interrupt_char, and we want ^Z to be a no-op
#           rather than suspending vamos.
# -ixon   : don't intercept ^S/^Q for flow control.
# min 1 time 0 : read() returns as soon as one byte is available.
stty -icanon -echo -isig -ixon min 1 time 0

VOLS_DIR="$(mktemp -d -t amiga-vamos-vols.XXXXXX)"
cleanup_vols() {
    rm -rf "$VOLS_DIR"
}
trap 'cleanup_vols; restore_tty' EXIT

cd "$HOME/vamos"
exec pipenv run vamos -q --cpu 68020 -s 32 \
    --vols-base-dir "$VOLS_DIR" \
    -- "$MPY_BIN" "$@"
