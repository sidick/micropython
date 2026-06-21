#!/bin/bash
# ports/amiga/tools/amiga-vamos-run.sh — wrapper used as MICROPY_MICROPYTHON.
#
# tests/run-tests.py invokes its target as
#
#     MICROPY_MICROPYTHON [-X opt ...] /abs/path/to/tests/<dir>/<test>.py
#
# with cwd set to /abs/path/to/tests/<dir>. This wrapper mounts the
# repo's tests/ directory as `mp:` inside vamos, sets vamos's cwd to
# the matching mp:<dir>, and replaces any test-script argument with
# its basename. The Amiga binary then sees sys.argv[0] / __file__ as
# just the filename (e.g. "argv.py"), exactly matching what
# ports/amiga/tools/amiga-gen-exp.py records when it runs host CPython with
# cwd=<dirname> and argv=<basename>.
#
# The mount name `mp:` is purely internal -- it doesn't appear in any
# captured test output and isn't something a user has to type.
#
# Each invocation gets a private --vols-base-dir to avoid collisions
# between parallel workers on the auto RAM:/T:/etc volumes, and -q
# suppresses vamos's log output (which would otherwise pollute the
# captured stdout/stderr that run-tests.py diffs against .exp files).

set -e

REPO_ROOT="$(cd "$(dirname "$0")/../../.." && pwd)"
TESTS_DIR="$REPO_ROOT/tests"

# Variant selection. Pick the build to launch and the matching vamos --cpu
# flag. Vamos emulates 68020 (soft-float) and 68040 (with built-in FPU);
# it has no 68881/68882 emulation, so the 68020fpu variant can only be
# tested under Amiberry or real hardware.
AMIGA_VARIANT="${AMIGA_VARIANT:-standard}"
# RAM (KiB) sized for the variant's heap plus headroom for the binary,
# stack, and AmigaOS overhead. Vamos's default is ~1 MiB which is too
# small for the 68040 variant's 1 MiB heap.
case "$AMIGA_VARIANT" in
    standard)       VAMOS_CPU="68020"; VAMOS_RAM_KIB=2048 ;;
    68040)          VAMOS_CPU="68040"; VAMOS_RAM_KIB=4096 ;;
    68020fpu)
        echo "amiga-vamos-run.sh: the '68020fpu' variant builds 68881 FPU instructions" >&2
        echo "  but vamos has no 68881 emulation. Use Amiberry, FS-UAE, or real" >&2
        echo "  hardware to test this variant. (For host-side testing of FPU codegen," >&2
        echo "  use AMIGA_VARIANT=68040 instead — vamos's 68040 has an emulated FPU.)" >&2
        exit 2
        ;;
    *)
        echo "amiga-vamos-run.sh: unknown AMIGA_VARIANT='$AMIGA_VARIANT'" >&2
        echo "  Supported: standard, 68020fpu (Amiberry only), 68040" >&2
        exit 2
        ;;
esac

MPY_BIN="$REPO_ROOT/ports/amiga/build-$AMIGA_VARIANT/micropython"
if [ ! -x "$MPY_BIN" ]; then
    echo "amiga-vamos-run.sh: $MPY_BIN not built." >&2
    echo "  Run: (cd ports/amiga && make VARIANT=$AMIGA_VARIANT)" >&2
    exit 1
fi

amiga_args=""
test_cwd=""
for arg in "$@"; do
    case "$arg" in
        # A test script under tests/: pass just the basename, and
        # remember its directory (relative to tests/) so we can point
        # vamos's cwd at it. With cwd set to the test's directory and
        # the script invoked by basename, sys.argv[0] / __file__ inside
        # the test see just the filename -- matching what
        # ports/amiga/tools/amiga-gen-exp.py records via host CPython.
        "$TESTS_DIR"/*.py)
            rel="${arg#$TESTS_DIR/}"
            d="$(dirname "$rel")"
            [ "$d" = "." ] && d=""
            test_cwd="mp:$d"
            amiga_args="$amiga_args $(basename "$arg")"
            ;;
        # Anything else (e.g. -X emit=bytecode): pass through.
        *) amiga_args="$amiga_args $arg" ;;
    esac
done

# Prefer the test-derived cwd; otherwise track whatever cwd
# tests/run-tests.py set on the host before invoking us, so that
# relative paths inside the test (e.g. open("data/file1")) resolve
# the same way as on the host.
if [ -n "$test_cwd" ]; then
    AMIGA_CWD="$test_cwd"
else
    case "$PWD" in
        "$TESTS_DIR")    AMIGA_CWD="mp:" ;;
        "$TESTS_DIR"/*)  AMIGA_CWD="mp:${PWD#$TESTS_DIR/}" ;;
        *)               AMIGA_CWD="mp:" ;;
    esac
fi

VOLS_DIR="$(mktemp -d -t amiga-vamos-vols.XXXXXX)"
trap 'rm -rf "$VOLS_DIR"' EXIT

cd "$HOME/vamos"
# Vamos defaults to an 8 KiB stack -- enough for normal Python but
# not for stress/import tests that nest several frames deep. 32 KiB
# matches a typical "Stack 32768" AmigaDOS environment, which is
# closer to what users run real MicroPython under.
exec pipenv run vamos -q --cpu "$VAMOS_CPU" -m "$VAMOS_RAM_KIB" -s 32 \
    --vols-base-dir "$VOLS_DIR" \
    -V "mp:$TESTS_DIR" \
    --cwd "$AMIGA_CWD" \
    -- "$MPY_BIN" $amiga_args
