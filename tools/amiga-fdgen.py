#!/usr/bin/env python3
"""Parse bebbo NDK `.fd` files and emit a frozen Python signature table.

Phase 17 of the AmigaOS port wants every system library's call signatures
available to Python without a runtime dependency on the NDK being installed
at the target.  This tool walks a directory of `.fd` files (the bebbo NDK
ships them at `/opt/amiga/m68k-amigaos/ndk/lib/fd/`) and writes a single
Python module with a `LIBRARIES` dict keyed by openable name
(`"intuition.library"`, `"console.device"`, `"card.resource"`, ...) whose
values are dicts mapping function name to a `(lvo, regs_csv)` tuple.

The `.fd` syntax (see http://aminet.net/package/dev/misc/fd2pragma):

  ##base _IntuitionBase
  ##bias 30
  ##public
  * Comments start with a star.
  OpenIntuition()()                              # 0 args, LVO -30
  Intuition(iEvent)(a0)                          # 1 arg in A0, LVO -36
  AddGadget(window,gadget,position)(a0/a1,d0)    # 3 args, LVO -42
  ##private
  someInternalFunction()()                       # still consumes a slot
  ##end

`##bias N` sets the *starting* offset for subsequent slot indexing; each
function (public or private) consumes 6 bytes of the library's jump
table.  A file may emit multiple `##bias` directives to skip reserved
holes — each one resets the slot counter.

By default, private functions are dropped from the output (they are not
callable from external code and just take up space).  Pass
`--include-private` to keep them.
"""

import argparse
import os
import re
import sys
from pathlib import Path

DEFAULT_FD_DIR = "/opt/amiga/m68k-amigaos/ndk/lib/fd"

# A function line.  Permissive on whitespace and on what counts as a
# register-list separator (the standard files use `/` within argument
# groups and `,` between groups, but a flat split on either is enough —
# the result is a flat list of registers in arg order).
FUNC_RE = re.compile(r"^([A-Za-z_]\w*)\s*\(([^)]*)\)\s*\(([^)]*)\)\s*$")

# Filename → openable-name override.  Used when the .fd filename doesn't
# follow the `xxx_lib.fd → xxx.<kind>` rule.  Kept tiny on purpose; new
# entries go here only if a real library refuses the default derivation.
NAME_OVERRIDES = {
    # NDK: cardres_lib.fd actually exposes card.resource.
    "cardres": "card",
}


def parse_fd(path):
    """Parse one .fd file.

    Returns (base_directive, list_of_functions) where each function is a
    dict {name, lvo, regs (tuple), public (bool), lineno}.
    """
    base = None
    bias = None
    slot = 0
    public = True
    functions = []
    warnings = []

    with open(path, "r") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.strip()
            if not line or line.startswith("*"):
                continue
            if line.startswith("##"):
                parts = line.split(None, 1)
                directive = parts[0]
                arg = parts[1].strip() if len(parts) > 1 else ""
                if directive == "##base":
                    base = arg
                elif directive == "##bias":
                    try:
                        bias = int(arg)
                    except ValueError:
                        warnings.append(f"{path}:{lineno}: bad ##bias arg {arg!r}")
                        continue
                    slot = 0
                elif directive == "##public":
                    public = True
                elif directive == "##private":
                    public = False
                elif directive == "##end":
                    break
                else:
                    warnings.append(f"{path}:{lineno}: unknown directive {directive}")
                continue

            m = FUNC_RE.match(line)
            if not m:
                warnings.append(f"{path}:{lineno}: unparsed line: {line!r}")
                continue
            if bias is None:
                warnings.append(f"{path}:{lineno}: function before ##bias")
                continue

            name, args_csv, regs_csv = m.groups()
            args = [a for a in re.split(r"\s*,\s*", args_csv) if a]
            regs = [r.lower() for r in re.split(r"[,/]\s*|\s+", regs_csv) if r]
            lvo = -(bias + slot * 6)
            slot += 1

            if len(args) != len(regs):
                warnings.append(
                    f"{path}:{lineno}: {name}: arg/reg count mismatch "
                    f"({len(args)} args vs {len(regs)} regs); skipping"
                )
                continue
            for r in regs:
                if r not in {"d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
                             "a0", "a1", "a2", "a3", "a4", "a5"}:
                    warnings.append(
                        f"{path}:{lineno}: {name}: register {r!r} not in "
                        f"D0-D7/A0-A5 (a6 is the library base, a7 is sp); "
                        f"skipping"
                    )
                    break
            else:
                functions.append({
                    "name": name,
                    "lvo": lvo,
                    "regs": tuple(regs),
                    "public": public,
                    "lineno": lineno,
                })

    return base, functions, warnings


def derive_openable_name(filename, base_directive):
    """Return the name a caller passes to OpenLibrary / OpenDevice / OpenResource."""
    stem = Path(filename).stem
    if stem.endswith("_lib"):
        stem = stem[:-4]
    stem = NAME_OVERRIDES.get(stem, stem)

    kind = "library"
    if base_directive:
        if base_directive.endswith("Device"):
            kind = "device"
        elif base_directive.endswith("Resource"):
            kind = "resource"
    return f"{stem}.{kind}"


def emit_python(libraries, out, fd_dir):
    out.write(
        "# Auto-generated by tools/amiga-fdgen.py — do not edit.\n"
        f"# Source: {fd_dir}\n"
        "#\n"
        "# LIBRARIES is keyed by the name a caller hands to OpenLibrary /\n"
        "# OpenDevice / OpenResource. Values are dicts of\n"
        "#   function_name -> (lvo, regs_csv)\n"
        "# where `lvo` is the signed offset from the library base (e.g. -96\n"
        "# for intuition.library DisplayBeep) and `regs_csv` is a flat,\n"
        "# arg-order list of D0-D7 / A0-A5 register names (empty for\n"
        "# zero-arg functions).\n"
        "\n"
    )
    total_funcs = sum(len(fns) for fns in libraries.values())
    out.write(f"# {len(libraries)} libraries / devices / resources, "
              f"{total_funcs} function signatures.\n\n")
    out.write("LIBRARIES = {\n")
    for libname in sorted(libraries):
        out.write(f"    {libname!r}: {{\n")
        for fname in sorted(libraries[libname]):
            lvo, regs = libraries[libname][fname]
            regs_csv = ",".join(regs)
            out.write(f"        {fname!r}: ({lvo}, {regs_csv!r}),\n")
        out.write("    },\n")
    out.write("}\n")


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--fd-dir", default=DEFAULT_FD_DIR,
                   help=f"directory of .fd files (default: {DEFAULT_FD_DIR})")
    p.add_argument("--output", "-o", default="-",
                   help="output file path, or '-' for stdout (default)")
    p.add_argument("--include-private", action="store_true",
                   help="emit ##private functions too (default: drop them)")
    p.add_argument("--quiet", "-q", action="store_true",
                   help="suppress per-file warnings; only print the final summary")
    args = p.parse_args(argv)

    fd_dir = Path(args.fd_dir)
    if not fd_dir.is_dir():
        p.error(f"--fd-dir: not a directory: {fd_dir}")

    libraries = {}
    total_warnings = 0
    dropped_private = 0
    fd_paths = sorted(fd_dir.glob("*.fd"))
    if not fd_paths:
        p.error(f"no .fd files in {fd_dir}")

    for path in fd_paths:
        base, functions, warns = parse_fd(path)
        if not args.quiet:
            for w in warns:
                print(w, file=sys.stderr)
        total_warnings += len(warns)
        if not functions:
            continue
        openable = derive_openable_name(path.name, base)
        per_lib = {}
        for fn in functions:
            if not fn["public"] and not args.include_private:
                dropped_private += 1
                continue
            per_lib[fn["name"]] = (fn["lvo"], fn["regs"])
        if per_lib:
            libraries[openable] = per_lib

    out = sys.stdout if args.output == "-" else open(args.output, "w")
    try:
        emit_python(libraries, out, fd_dir)
    finally:
        if out is not sys.stdout:
            out.close()

    summary = (
        f"amiga-fdgen: {len(fd_paths)} .fd files, "
        f"{len(libraries)} openable names, "
        f"{sum(len(v) for v in libraries.values())} functions emitted"
    )
    if dropped_private:
        summary += f" ({dropped_private} private dropped)"
    if total_warnings:
        summary += f"; {total_warnings} warning(s)"
    print(summary, file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
