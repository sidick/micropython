#!/usr/bin/env python3
"""Generate .exp expected-output files for MicroPython tests using host CPython.

The on-device Amiga test runner (docs/amiga.md) has no CPython to diff against,
so it relies on .exp files for every test. Most tests in tests/basics/ ship
without one because the upstream run-tests.py uses CPython directly. This
tool fills the gap: for each .py test under the given directory that doesn't
already have a .exp, it runs CPython3 and saves stdout+stderr to <test>.py.exp.

Tests where CPython fails (e.g. those that import the `micropython` module
or use port-specific behaviour) are skipped — they would never produce a
useful comparison file anyway, and the upstream tree often hand-writes a
.exp for those cases. Existing .exp files are left untouched.

Generated .exp files use Unix LF line endings to stay consistent with both
the upstream tree (run-tests.py normalises CRLF to LF before comparing) and
with what CPython emits naturally. The on-device runner is responsible for
normalising the AmigaOS CRLF output before its own comparison.

Usage:
    tools/amiga-gen-exp.py tests/basics
    tools/amiga-gen-exp.py tests/basics tests/float tests/io
    MICROPY_CPYTHON3=python3.11 tools/amiga-gen-exp.py tests/basics
"""

import argparse
import os
import subprocess
import sys

CPYTHON3 = os.environ.get("MICROPY_CPYTHON3", "python3")


def gather_tests(dirs):
    py_files = []
    for d in dirs:
        if not os.path.isdir(d):
            sys.stderr.write("amiga-gen-exp.py: {!r} is not a directory\n".format(d))
            sys.exit(2)
        for root, _, files in os.walk(d):
            for name in files:
                if name.endswith(".py"):
                    py_files.append(os.path.join(root, name))
    py_files.sort()
    return py_files


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("dirs", nargs="+", help="test directories to process")
    parser.add_argument(
        "-f",
        "--force",
        action="store_true",
        help="regenerate .exp files even if one already exists",
    )
    parser.add_argument(
        "-v", "--verbose", action="store_true", help="print per-file progress"
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=30.0,
        help="CPython timeout per test (seconds, default 30)",
    )
    args = parser.parse_args()

    py_files = gather_tests(args.dirs)
    generated = 0
    kept_existing = 0
    cpython_failed = 0
    timed_out = 0

    for py in py_files:
        exp = py + ".exp"
        if not args.force and os.path.exists(exp):
            kept_existing += 1
            if args.verbose:
                print("keep   ", py)
            continue
        py_abs = os.path.abspath(py)
        try:
            result = subprocess.run(
                [CPYTHON3, "-BS", py_abs],
                cwd=os.path.dirname(py_abs) or ".",
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT,
                timeout=args.timeout,
            )
        except subprocess.TimeoutExpired:
            timed_out += 1
            if args.verbose:
                print("timeout", py)
            continue

        if result.returncode != 0:
            cpython_failed += 1
            if args.verbose:
                print("skip   ", py, "(CPython exit", result.returncode, ")")
            continue

        # Normalise to LF (CPython on macOS/Linux already emits LF, but be
        # defensive in case a host quirk produces CRLF).
        data = result.stdout.replace(b"\r\n", b"\n")
        with open(exp, "wb") as f:
            f.write(data)
        generated += 1
        if args.verbose:
            print("write  ", exp)

    print()
    print("Generated:      {}".format(generated))
    print("Kept existing:  {}".format(kept_existing))
    print("CPython failed: {}".format(cpython_failed))
    print("Timed out:      {}".format(timed_out))
    print("Total tests:    {}".format(len(py_files)))


if __name__ == "__main__":
    main()
