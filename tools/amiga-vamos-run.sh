#!/bin/bash
# tools/amiga-vamos-run.sh — wrapper used as MICROPY_MICROPYTHON.
# run-tests.py invokes us with a path relative to tests/ (e.g.
# basics/string1.py); rewrite that to tests:basics/string1.py for vamos.
#
# run-tests.py also sets cwd to the test's directory so that relative
# paths like data/file1 resolve. Map that to a vamos --cwd under tests:
# so the binary sees the same working directory.
#
# Each invocation gets a private --vols-base-dir to avoid collisions
# between parallel workers on the auto RAM:/T:/etc volumes, and -q
# suppresses vamos's log output (which would otherwise pollute the
# captured stdout/stderr that run-tests.py diffs against .exp files).
set -e

REPO_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MPY_BIN="$REPO_ROOT/ports/amiga/build/micropython"
TESTS_DIR="$REPO_ROOT/tests"

ORIG_CWD="$PWD"
case "$ORIG_CWD" in
    "$TESTS_DIR")    AMIGA_CWD="tests:" ;;
    "$TESTS_DIR"/*)  AMIGA_CWD="tests:${ORIG_CWD#$TESTS_DIR/}" ;;
    *)               AMIGA_CWD="" ;;
esac

amiga_args=""
for arg in "$@"; do
    case "$arg" in
        "$TESTS_DIR"/*) amiga_args="$amiga_args tests:${arg#$TESTS_DIR/}" ;;
        */tests/*)      amiga_args="$amiga_args tests:${arg#*/tests/}" ;;
        *)              amiga_args="$amiga_args $arg" ;;
    esac
done

VOLS_DIR="$(mktemp -d -t amiga-vamos-vols.XXXXXX)"
trap 'rm -rf "$VOLS_DIR"' EXIT

cwd_args=""
[ -n "$AMIGA_CWD" ] && cwd_args="--cwd $AMIGA_CWD"

cd "$HOME/vamos"
exec pipenv run vamos -q --cpu 68020 \
    --vols-base-dir "$VOLS_DIR" \
    -V "tests:$TESTS_DIR" \
    $cwd_args \
    -- "$MPY_BIN" $amiga_args
