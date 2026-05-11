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

# Variant selection — same rules as amiga-vamos-run.sh. Vamos emulates
# 68020 (soft-float) and 68040 (built-in FPU) but has no 68881/68882
# emulation, so the 68020fpu variant needs Amiberry or real hardware.
AMIGA_VARIANT="${AMIGA_VARIANT:-standard}"
# RAM sized to fit the variant's heap plus headroom — see amiga-vamos-run.sh.
case "$AMIGA_VARIANT" in
    standard)       VAMOS_CPU="68020"; VAMOS_RAM_KIB=2048 ;;
    minimal)        VAMOS_CPU="68020"; VAMOS_RAM_KIB=2048 ;;
    68040)          VAMOS_CPU="68040"; VAMOS_RAM_KIB=4096 ;;
    68020fpu)
        echo "amiga-vamos-repl.sh: the '68020fpu' variant builds 68881 FPU instructions" >&2
        echo "  but vamos has no 68881 emulation. Use Amiberry, FS-UAE, or real" >&2
        echo "  hardware to test this variant. (For host-side testing of FPU codegen," >&2
        echo "  use AMIGA_VARIANT=68040 instead — vamos's 68040 has an emulated FPU.)" >&2
        exit 2
        ;;
    *)
        echo "amiga-vamos-repl.sh: unknown AMIGA_VARIANT='$AMIGA_VARIANT'" >&2
        echo "  Supported: standard, minimal, 68020fpu (Amiberry only), 68040" >&2
        exit 2
        ;;
esac

MPY_BIN="$REPO_ROOT/ports/amiga/build-$AMIGA_VARIANT/micropython"
if [ ! -x "$MPY_BIN" ]; then
    echo "amiga-vamos-repl.sh: $MPY_BIN not built." >&2
    echo "  Run: (cd ports/amiga && make VARIANT=$AMIGA_VARIANT)" >&2
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
exec pipenv run vamos -q --cpu "$VAMOS_CPU" -m "$VAMOS_RAM_KIB" -s 32 \
    --vols-base-dir "$VOLS_DIR" \
    -- "$MPY_BIN" "$@"
