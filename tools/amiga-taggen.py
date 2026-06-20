#!/usr/bin/env python3
"""Harvest AmigaOS tag IDs from NDK headers using bebbo's m68k cross-gcc.

Companion to `tools/amiga-fdgen.py`: where that tool derives library
LVOs from `.fd` files, this one derives tag-name → integer-ID mappings
from the NDK C headers.  The output module (default
`ports/amiga/modules/_amiga_tags.py`) is consumed at runtime by
`amiga.taglist(**named)` to translate kwargs like `WA_Width=640` into
the numeric tag IDs expected by `OpenWindowTagList` and friends.

Tag values in the headers are `<NAME>_Dummy + offset` macros where the
`Dummy` base is itself `TAG_USER + N`, so they can't be regex-scraped
without a real C preprocessor.  Instead this tool:

1. Runs `m68k-amigaos-gcc -E -dM` over a generated C source that
   includes every header in `DEFAULT_HEADERS`, collecting every macro
   name that looks like `<UPPER>_<TagName>`.
2. Generates a second C file containing
   `const ULONG _x_<NAME> = <NAME>;` for each candidate.
3. Compiles to assembly (`-S -O2`) — the compiler evaluates the
   constant expression and emits a literal `.long N` for each.
4. Parses the assembly to recover `(name, value)` pairs.
5. Filters to values in the TAG_USER range (high bit set), keeping
   the special small `TAG_*` markers (0-3, TAG_USER itself).
6. Emits a Python module with `TAGS = {...}`.

Run without arguments for the default behaviour:

    tools/amiga-taggen.py -o ports/amiga/modules/_amiga_tags.py
"""

import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


DEFAULT_GCC = "m68k-amigaos-gcc"

DEFAULT_INCLUDE = "/opt/amiga/m68k-amigaos/ndk-include"

# Headers to scan.  These cover the namespaces users actually hit
# (Intuition GUI, ASL file requesters, GadTools, datatypes, layouts,
# locale, prefs).  Add more here as new tag families come up.
DEFAULT_HEADERS = [
    "utility/tagitem.h",
    "intuition/intuition.h",
    "intuition/screens.h",
    "intuition/classusr.h",
    "intuition/imageclass.h",
    "intuition/gadgetclass.h",
    "intuition/icclass.h",
    "intuition/pointerclass.h",
    "intuition/sysiclass.h",
    "intuition/cghooks.h",
    "libraries/gadtools.h",
    "libraries/asl.h",
    "libraries/iffparse.h",
    "libraries/locale.h",
    "datatypes/datatypes.h",
    "datatypes/datatypesclass.h",
    "workbench/icon.h",
    "workbench/workbench.h",
    "graphics/displayinfo.h",
]


def filter_existing_headers(include_dir, headers, verbose=False):
    """Drop any header in `headers` that's not present under
    `include_dir`, warning to stderr."""
    out = []
    for h in headers:
        if (Path(include_dir) / h).is_file():
            out.append(h)
        elif verbose:
            sys.stderr.write("amiga-taggen: skipping missing header %s\n" % h)
    return out


# A "candidate tag" name: an uppercase namespace prefix, an
# underscore, then a capitalised tag identifier.  Matches WA_Width,
# SA_DisplayID, GTMN_FullMenu, ICA_TARGET, ASLFR_TitleText, etc.
# The PREFIX must be 2+ uppercase/digit chars to avoid grabbing
# things like `_Pixels` or `A_Foo`.
TAG_NAME_RE = re.compile(r"^([A-Z][A-Z0-9]{1,15})_([A-Z][A-Za-z0-9_]*)$")

# Always emit these regardless of value (they're the tag-protocol
# markers, not tag IDs themselves).
ALWAYS_KEEP = {"TAG_DONE", "TAG_END", "TAG_IGNORE", "TAG_MORE", "TAG_SKIP", "TAG_USER"}


def find_gcc(name):
    full = shutil.which(name)
    if full is None:
        # Fall back to the standard bebbo install path.
        candidate = "/opt/amiga/bin/" + name
        if os.path.isfile(candidate):
            return candidate
        raise SystemExit("error: %s not found on PATH or in /opt/amiga/bin" % name)
    return full


def gather_candidates(gcc, include_dir, headers, verbose=False):
    """Run gcc -E -dM and return the sorted set of macro names that
    look like tag IDs."""
    src = "\n".join("#include <%s>" % h for h in headers) + "\n"
    with tempfile.NamedTemporaryFile("w", suffix=".c", delete=False) as f:
        f.write(src)
        c_path = f.name
    try:
        result = subprocess.run(
            [gcc, "-E", "-dM", "-I", include_dir, c_path],
            capture_output=True,
            text=True,
        )
    finally:
        os.unlink(c_path)
    if result.returncode != 0:
        sys.stderr.write(result.stderr)
        raise SystemExit("amiga-taggen: preprocessor pass failed")
    names = set()
    for line in result.stdout.splitlines():
        if not line.startswith("#define "):
            continue
        parts = line.split(maxsplit=2)
        if len(parts) < 2:
            continue
        name = parts[1]
        # Skip function-like macros (name(args)).
        if "(" in name:
            continue
        if name in ALWAYS_KEEP or TAG_NAME_RE.match(name):
            names.add(name)
    if verbose:
        sys.stderr.write("amiga-taggen: %d candidate names\n" % len(names))
    return sorted(names)


_ASM_LABEL_RE = re.compile(
    # bebbo m68k-amigaos-gcc prefixes every global with an extra '_',
    # so the C identifier `_x_FOO` becomes `__x_FOO` in the assembly.
    r"^__x_([A-Za-z_][A-Za-z0-9_]*):\s*\n"
    r"(?:[ \t]+(?:\.align|\.even|\.type|\.size|\.section|\.local|\.global)[^\n]*\n)*"
    r"[ \t]+\.long\s+([^\n]+)$",
    re.MULTILINE,
)


def evaluate(gcc, include_dir, headers, candidates, verbose=False):
    """Generate `const ULONG _x_<NAME> = <NAME>;` for each candidate,
    compile to assembly, and pull the resolved values out of the
    `.long` directives.  Returns a `{name: integer}` dict.

    If the batch compile fails (one candidate isn't a valid constant
    expression), recurses by halves to isolate and drop the bad ones.
    """
    if not candidates:
        return {}
    src_lines = ["#include <exec/types.h>"]
    for h in headers:
        src_lines.append("#include <%s>" % h)
    src_lines.append("")
    for name in candidates:
        src_lines.append("const ULONG _x_%s = (ULONG)(%s);" % (name, name))
    src = "\n".join(src_lines) + "\n"

    with tempfile.TemporaryDirectory() as td:
        c_path = os.path.join(td, "tags.c")
        s_path = os.path.join(td, "tags.s")
        with open(c_path, "w") as f:
            f.write(src)
        result = subprocess.run(
            [gcc, "-S", "-O2", "-m68020", "-w", "-I", include_dir, "-o", s_path, c_path],
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            # Bisect: split the candidates in two and retry each half.
            if len(candidates) == 1:
                if verbose:
                    sys.stderr.write(
                        "amiga-taggen: dropping %s: %s\n"
                        % (
                            candidates[0],
                            result.stderr.strip().splitlines()[-1]
                            if result.stderr.strip()
                            else "compile failed",
                        )
                    )
                return {}
            mid = len(candidates) // 2
            left = evaluate(gcc, include_dir, headers, candidates[:mid], verbose=verbose)
            right = evaluate(gcc, include_dir, headers, candidates[mid:], verbose=verbose)
            left.update(right)
            return left
        with open(s_path) as f:
            asm = f.read()

    values = {}
    for m in _ASM_LABEL_RE.finditer(asm):
        name = m.group(1)
        val_str = m.group(2).strip()
        # Try to parse — accept decimal, 0x hex, and bebbo's `0x` prefix.
        try:
            if val_str.startswith("0x") or val_str.startswith("0X"):
                val = int(val_str, 16)
            elif val_str.startswith("-"):
                val = int(val_str)
                if val < 0:
                    val += 0x100000000
            else:
                val = int(val_str)
        except ValueError:
            # Symbolic reference — not a constant.
            continue
        values[name] = val & 0xFFFFFFFF
    return values


def emit_python(values, out, headers):
    """Write a Python module of the form `TAGS = {...}` to `out`."""
    out.write("# Auto-generated by tools/amiga-taggen.py — do not edit.\n")
    out.write("# Source headers (in order, all under the 3.2 NDK):\n")
    for h in headers:
        out.write("#   %s\n" % h)
    out.write("#\n")
    out.write("# `TAGS` maps each tag-name (e.g. `WA_Width`) to its\n")
    out.write("# 32-bit integer ID.  Consumed at runtime by\n")
    out.write("# `amiga.taglist(**named)` to turn kwargs into the\n")
    out.write("# numeric tag/value pairs expected by OpenWindowTagList\n")
    out.write("# and friends.\n\n")
    out.write("# %d tag IDs total.\n\n" % len(values))
    out.write("# The four universal TAG_* markers from utility/tagitem.h\n")
    out.write("# get pulled in by the harvester too — they're not tags\n")
    out.write("# you'd ever pass via name, but TAG_USER is occasionally\n")
    out.write("# useful when constructing tag IDs by hand.\n")
    out.write("TAG_USER = 0x80000000\n")
    out.write("TAG_DONE = 0\n")
    out.write("TAG_IGNORE = 1\n")
    out.write("TAG_MORE = 2\n")
    out.write("TAG_SKIP = 3\n\n")
    out.write("TAGS = {\n")
    # Also expose the markers by name so users can `amiga.taglist(\n"
    # TAG_MORE=...)` if they really want to.
    out.write("    'TAG_DONE': 0,\n")
    out.write("    'TAG_IGNORE': 1,\n")
    out.write("    'TAG_MORE': 2,\n")
    out.write("    'TAG_SKIP': 3,\n")
    out.write("    'TAG_USER': 0x80000000,\n")
    for name in sorted(values):
        out.write("    %r: 0x%08x,\n" % (name, values[name]))
    out.write("}\n")


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument(
        "--gcc", default=DEFAULT_GCC, help="cross-compiler binary (default: %s)" % DEFAULT_GCC
    )
    p.add_argument(
        "--ndk-include",
        default=DEFAULT_INCLUDE,
        help="NDK include root (default: %s)" % DEFAULT_INCLUDE,
    )
    p.add_argument(
        "--include",
        action="append",
        default=None,
        metavar="HEADER",
        help="add a header to scan; may be given multiple times.  "
        "If any --include is supplied, the default header "
        "list is replaced; otherwise the default set is used.",
    )
    p.add_argument("--output", "-o", default="-", help="output file (default: stdout)")
    p.add_argument(
        "--keep-all",
        action="store_true",
        help="emit every resolved candidate regardless of "
        "value; default filters to values that look like "
        "tag IDs (high bit set, plus the TAG_* markers)",
    )
    p.add_argument("--quiet", "-q", action="store_true", help="suppress per-symbol drop warnings")
    args = p.parse_args(argv)

    gcc = find_gcc(args.gcc)
    inc = Path(args.ndk_include)
    if not inc.is_dir():
        p.error("--ndk-include: not a directory: %s" % inc)
    headers = args.include if args.include else list(DEFAULT_HEADERS)

    verbose = not args.quiet
    headers = filter_existing_headers(str(inc), headers, verbose=verbose)
    if not headers:
        p.error("none of the requested headers exist under %s" % inc)
    candidates = gather_candidates(gcc, str(inc), headers, verbose=verbose)
    values = evaluate(gcc, str(inc), headers, candidates, verbose=verbose)

    if not args.keep_all:
        kept = {}
        for name, val in values.items():
            if name in ALWAYS_KEEP:
                continue  # emitted as literal constants in the header
            # Plausible tag IDs have bit 31 set (TAG_USER base) or the
            # very small marker values.  Drop everything else — it's
            # almost certainly an unrelated flag constant that happened
            # to match the regex.
            if val & 0x80000000:
                kept[name] = val
        values = kept

    out = sys.stdout if args.output == "-" else open(args.output, "w")
    try:
        emit_python(values, out, headers)
    finally:
        if out is not sys.stdout:
            out.close()

    sys.stderr.write(
        "amiga-taggen: %d candidates → %d tag IDs emitted "
        "(%s)\n"
        % (
            len(candidates),
            len(values),
            "stdout" if args.output == "-" else args.output,
        )
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
