#!/usr/bin/env python3
"""Parse bebbo NDK `.fd` files and emit a frozen Python signature table.

Phase 17 of the AmigaOS port wants every system library's call signatures
available to Python without a runtime dependency on the NDK being installed
at the target.  This tool walks one or more directories of `.fd` files
(the bebbo NDK ships them at `/opt/amiga/m68k-amigaos/ndk/lib/fd/`) and
writes a single Python module with a `LIBRARIES` dict keyed by openable
name (`"intuition.library"`, `"console.device"`, `"card.resource"`, ...)
whose values are dicts mapping function name to a `(lvo, regs_csv,
since)` tuple, where `since` is the AmigaOS NDK version that first
shipped the function (`""` if unknown).

To pull data from multiple NDK releases, repeat `--fd-dir`:

    tools/amiga-fdgen.py \\
        --fd-dir 3.1=/path/to/ndk31/lib/fd \\
        --fd-dir 3.5=/path/to/ndk35/lib/fd \\
        --fd-dir 3.9=/path/to/ndk39/lib/fd

Sources are merged in ascending-version order; each function's `since`
records the lowest version that listed it.  AmigaOS LVOs are append-only
(libraries never renumber existing entries), so any cross-version drift
in offset or register list is reported as a hard warning.

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


def emit_python(libraries, out, sources):
    out.write("# Auto-generated by tools/amiga-fdgen.py — do not edit.\n")
    out.write("# Sources (in merge order):\n")
    for version, path in sources:
        label = version if version else "(no version tag)"
        out.write(f"#   {label}: {path}\n")
    out.write(
        "#\n"
        "# LIBRARIES is keyed by the name a caller hands to OpenLibrary /\n"
        "# OpenDevice / OpenResource. Values are dicts of\n"
        "#   function_name -> (lvo, regs_csv, since)\n"
        "# where `lvo` is the signed offset from the library base (e.g. -96\n"
        "# for intuition.library DisplayBeep), `regs_csv` is a flat,\n"
        "# arg-order list of D0-D7 / A0-A5 register names (empty for\n"
        "# zero-arg functions), and `since` is the lowest AmigaOS NDK\n"
        "# version that listed the function — `''` when only a single\n"
        "# unversioned source was scanned.\n"
        "\n"
    )
    total_funcs = sum(len(fns) for fns in libraries.values())
    out.write(f"# {len(libraries)} libraries / devices / resources, "
              f"{total_funcs} function signatures.\n\n")
    out.write("LIBRARIES = {\n")
    for libname in sorted(libraries):
        out.write(f"    {libname!r}: {{\n")
        for fname in sorted(libraries[libname]):
            lvo, regs, since = libraries[libname][fname]
            regs_csv = ",".join(regs)
            out.write(
                f"        {fname!r}: ({lvo}, {regs_csv!r}, {since!r}),\n")
        out.write("    },\n")
    out.write("}\n")


def version_sortkey(version):
    """Numeric sort key for AmigaOS version strings (e.g. '3.1', '2.04')."""
    if not version:
        return ()
    out = []
    for part in version.split("."):
        try:
            out.append(int(part))
        except ValueError:
            # Non-numeric component — push to end of its component slot.
            out.append((1 << 31, part))
    return tuple(out)


def parse_source_spec(spec):
    """Split a `--fd-dir` argument into `(version, path)`.

    `VERSION=PATH` returns the version explicitly; bare `PATH` returns
    an empty version string.  A path that contains a literal `=` and no
    version prefix can be escaped by prepending `=` (e.g. `=/some=path`).
    """
    if "=" in spec:
        left, right = spec.split("=", 1)
        # Allow `=PATH` (empty version) for paths that contain `=`.
        return left, Path(right)
    return "", Path(spec)


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--fd-dir", action="append", metavar="[VERSION=]PATH",
                   help=("directory of .fd files; may be given multiple "
                         "times.  Prefix with `VERSION=` to tag every "
                         "function found there with that NDK version. "
                         f"Defaults to a single, untagged scan of "
                         f"{DEFAULT_FD_DIR}."))
    p.add_argument("--output", "-o", default="-",
                   help="output file path, or '-' for stdout (default)")
    p.add_argument("--include-private", action="store_true",
                   help="emit ##private functions too (default: drop them)")
    p.add_argument("--quiet", "-q", action="store_true",
                   help="suppress per-file warnings; only print the final summary")
    args = p.parse_args(argv)

    raw_sources = args.fd_dir or [DEFAULT_FD_DIR]
    sources = [parse_source_spec(s) for s in raw_sources]
    for version, path in sources:
        if not path.is_dir():
            p.error(f"--fd-dir: not a directory: {path}")
    # Process oldest version first so each function's `since` records the
    # earliest NDK release that listed it.  Untagged sources (version="")
    # sort before any tagged ones via the empty-tuple sort key.
    sources.sort(key=lambda vp: version_sortkey(vp[0]))

    libraries = {}
    total_warnings = 0
    dropped_private = 0
    total_fd_files = 0
    drift_warnings = []

    for version, fd_dir in sources:
        fd_paths = sorted(fd_dir.glob("*.fd"))
        if not fd_paths:
            p.error(f"no .fd files in {fd_dir}")
        total_fd_files += len(fd_paths)
        for path in fd_paths:
            base, functions, warns = parse_fd(path)
            if not args.quiet:
                for w in warns:
                    print(w, file=sys.stderr)
            total_warnings += len(warns)
            if not functions:
                continue
            openable = derive_openable_name(path.name, base)
            per_lib = libraries.setdefault(openable, {})
            for fn in functions:
                if not fn["public"] and not args.include_private:
                    dropped_private += 1
                    continue
                existing = per_lib.get(fn["name"])
                if existing is None:
                    per_lib[fn["name"]] = (fn["lvo"], fn["regs"], version)
                else:
                    old_lvo, old_regs, _ = existing
                    if old_lvo != fn["lvo"] or old_regs != fn["regs"]:
                        drift_warnings.append(
                            f"{path}:{fn['lineno']}: {openable}::{fn['name']}: "
                            f"signature drift across NDK versions — was "
                            f"({old_lvo}, {old_regs}), now "
                            f"({fn['lvo']}, {fn['regs']}); keeping the older "
                            f"entry"
                        )

    if not args.quiet:
        for w in drift_warnings:
            print(w, file=sys.stderr)

    # Drop libraries that ended up empty (e.g. all-private with the
    # default filter).
    libraries = {k: v for k, v in libraries.items() if v}

    out = sys.stdout if args.output == "-" else open(args.output, "w")
    try:
        emit_python(libraries, out, sources)
    finally:
        if out is not sys.stdout:
            out.close()

    summary_parts = [
        f"amiga-fdgen: {total_fd_files} .fd files across "
        f"{len(sources)} source dir(s)",
        f"{len(libraries)} openable names",
        f"{sum(len(v) for v in libraries.values())} functions emitted",
    ]
    if dropped_private:
        summary_parts.append(f"{dropped_private} private dropped")
    if total_warnings or drift_warnings:
        summary_parts.append(
            f"{total_warnings + len(drift_warnings)} warning(s)"
            + (f" ({len(drift_warnings)} drift)" if drift_warnings else "")
        )
    print("; ".join(summary_parts), file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
