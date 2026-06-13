#!/usr/bin/env python3
"""Harvest AmigaOS struct layouts from NDK headers using bebbo's m68k cross-gcc.

Companion to `tools/amiga-fdgen.py` and `tools/amiga-taggen.py`: where those
derive library LVOs from `.fd` files and tag IDs from preprocessor macros, this
one derives struct sizes and field offsets from the NDK C headers.  The output
module (default `ports/amiga/modules/_amiga_structs.py`) is consumed at runtime
by `amiga.Struct(addr, layout)` to decode AmigaOS structures (Task, Library,
FileInfoBlock, IntuiMessage, ...) read out of target memory.

Sizes and offsets in NDK headers are determined by ABI rules plus alignment of
each member; they can't be regex-scraped without a real C compiler.  Instead
this tool:

1. Emits a C source containing `const long _x_<STRUCT>__SIZE = sizeof(struct X);`
   for every harvested struct, `const long _x_<STRUCT>__OFFSET__<field> =
   offsetof(struct X, path);` for every field, and (for inline fixed-byte
   strings) `const long _x_<STRUCT>__SIZEOF__<field> = sizeof(((X*)0)->path);`.
2. Compiles to assembly (`-S -O2 -m68020`) — the cross-gcc folds each constant
   expression to a literal `.long N`.
3. Parses the assembly to recover `(label, value)` pairs.
4. Stitches values back together with the type-code letters from the spec and
   emits a Python module mirroring the hand-curated format.

Run without arguments for the default behaviour (writes to stdout):

    tools/amiga-structgen.py -o ports/amiga/modules/_amiga_structs.py

Because the bebbo toolchain typically isn't on the host PATH, run this through
the same Docker wrapper used for the build, e.g.:

    docker run --rm -v "$PWD:/work" -w /work bebbo/amiga-build \\
        python3 tools/amiga-structgen.py \\
        -o ports/amiga/modules/_amiga_structs.py

The spec at the top of the file enumerates which structs and fields are
harvested.  To add a new struct: append an entry to `SPECS` listing the C
struct name, the header that declares it, a short comment, and the (py_key,
c_path, type_code) tuple for each field.  `c_path` is the member designator
passed to `offsetof` (use dots for embedded structs, e.g. `tc_Node.ln_Succ`).
`type_code` is the letter consumed by `amiga.Struct`:

    L/l  unsigned/signed 32-bit long
    H/h  unsigned/signed 16-bit half
    B/b  unsigned/signed 8-bit byte
    P    pointer (32-bit raw address)
    S    pointer to NUL-terminated string (auto-deref)
    s    inline fixed-byte NUL-terminated string; the generator resolves the
         size via sizeof() and emits `s<N>` (e.g. `s108` for fib_FileName)
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


# Specs to harvest.  Order is preserved in the output for readability.
#
# Each entry:
#   py_name   -- name of the emitted Python dict / `<name>_SIZE` constant
#   c_struct  -- C struct tag passed to `struct X` in offsetof/sizeof
#   header    -- NDK header that declares the struct (relative path)
#   comment   -- one-line description for the emitted top-level comment
#   fields    -- list of entries.  Each entry is either:
#                  * a string -- emitted verbatim as a `# ...` line inside the
#                    dict body, used for embedded-struct section dividers
#                  * a tuple (py_key, c_path, type_code) -- a struct field
#                  * a tuple (py_key, c_path, type_code, note) -- a struct
#                    field with an inline `# note` annotation
#                where:
#                  py_key    -- key in the emitted Python dict
#                  c_path    -- member designator for offsetof / sizeof
#                  type_code -- one of L/l/H/h/B/b/P/S/s (module docstring)
SPECS = [
    {
        "py_name": "NODE",
        "c_struct": "Node",
        "header": "exec/nodes.h",
        "comment": "head of every Exec list element",
        "fields": [
            ("ln_Succ", "ln_Succ", "L"),
            ("ln_Pred", "ln_Pred", "L"),
            ("ln_Type", "ln_Type", "B"),
            ("ln_Pri",  "ln_Pri",  "b"),
            ("ln_Name", "ln_Name", "S", "ptr to NUL-terminated string"),
        ],
    },
    {
        "py_name": "TASK",
        "c_struct": "Task",
        "header": "exec/tasks.h",
        "comment": "First 14 bytes are an embedded Node",
        "fields": [
            "Embedded Node",
            ("ln_Succ",       "tc_Node.ln_Succ", "L"),
            ("ln_Pred",       "tc_Node.ln_Pred", "L"),
            ("ln_Type",       "tc_Node.ln_Type", "B"),
            ("ln_Pri",        "tc_Node.ln_Pri",  "b"),
            ("ln_Name",       "tc_Node.ln_Name", "S"),
            "Task-specific fields",
            ("tc_Flags",      "tc_Flags",        "B"),
            ("tc_State",      "tc_State",        "B"),
            ("tc_IDNestCnt",  "tc_IDNestCnt",    "b"),
            ("tc_TDNestCnt",  "tc_TDNestCnt",    "b"),
            ("tc_SigAlloc",   "tc_SigAlloc",     "L"),
            ("tc_SigWait",    "tc_SigWait",      "L"),
            ("tc_SigRecvd",   "tc_SigRecvd",     "L"),
            ("tc_SigExcept",  "tc_SigExcept",    "L"),
            ("tc_TrapAlloc",  "tc_TrapAlloc",    "H"),
            ("tc_TrapAble",   "tc_TrapAble",     "H"),
            ("tc_ExceptData", "tc_ExceptData",   "P"),
            ("tc_ExceptCode", "tc_ExceptCode",   "P"),
            ("tc_TrapData",   "tc_TrapData",     "P"),
            ("tc_TrapCode",   "tc_TrapCode",     "P"),
            ("tc_SPReg",      "tc_SPReg",        "P"),
            ("tc_SPLower",    "tc_SPLower",      "P"),
            ("tc_SPUpper",    "tc_SPUpper",      "P"),
            ("tc_Switch",     "tc_Switch",       "P"),
            ("tc_Launch",     "tc_Launch",       "P"),
            "tc_MemEntry (struct List at offset 74) intentionally not unpacked.",
            ("tc_UserData",   "tc_UserData",     "P"),
        ],
    },
    {
        "py_name": "LIBRARY",
        "c_struct": "Library",
        "header": "exec/libraries.h",
        "comment": "every OpenLibrary base points here",
        "fields": [
            "Embedded Node",
            ("ln_Succ",      "lib_Node.ln_Succ", "L"),
            ("ln_Pred",      "lib_Node.ln_Pred", "L"),
            ("ln_Type",      "lib_Node.ln_Type", "B"),
            ("ln_Pri",       "lib_Node.ln_Pri",  "b"),
            ("ln_Name",      "lib_Node.ln_Name", "S"),
            "Library-specific fields",
            ("lib_Flags",    "lib_Flags",        "B"),
            ("lib_pad",      "lib_pad",          "B"),
            ("lib_NegSize",  "lib_NegSize",      "H"),
            ("lib_PosSize",  "lib_PosSize",      "H"),
            ("lib_Version",  "lib_Version",      "H"),
            ("lib_Revision", "lib_Revision",     "H"),
            ("lib_IdString", "lib_IdString",     "S"),
            ("lib_Sum",      "lib_Sum",          "L"),
            ("lib_OpenCnt",  "lib_OpenCnt",      "H"),
        ],
    },
    {
        "py_name": "DATESTAMP",
        "c_struct": "DateStamp",
        "header": "dos/dos.h",
        "comment": "AmigaDOS time representation",
        "fields": [
            ("ds_Days",   "ds_Days",   "L"),
            ("ds_Minute", "ds_Minute", "L"),
            ("ds_Tick",   "ds_Tick",   "L"),
        ],
    },
    {
        "py_name": "FILEINFOBLOCK",
        "c_struct": "FileInfoBlock",
        "header": "dos/dos.h",
        "comment": "Examine()/ExNext() output",
        "fields": [
            ("fib_DiskKey",      "fib_DiskKey",        "L"),
            ("fib_DirEntryType", "fib_DirEntryType",   "l",
                "signed: <0 = file, >0 = dir"),
            ("fib_FileName",     "fib_FileName",       "s",
                "NUL-terminated inline string"),
            ("fib_Protection",   "fib_Protection",     "L"),
            ("fib_EntryType",    "fib_EntryType",      "L"),
            ("fib_Size",         "fib_Size",           "L"),
            ("fib_NumBlocks",    "fib_NumBlocks",      "L"),
            "Embedded DateStamp (fib_Date)",
            ("fib_DateDays",     "fib_Date.ds_Days",   "L"),
            ("fib_DateMinute",   "fib_Date.ds_Minute", "L"),
            ("fib_DateTick",     "fib_Date.ds_Tick",   "L"),
            ("fib_Comment",      "fib_Comment",        "s",
                "NUL-terminated inline string"),
            ("fib_OwnerUID",     "fib_OwnerUID",       "H"),
            ("fib_OwnerGID",     "fib_OwnerGID",       "H"),
        ],
    },
    {
        "py_name": "INTUIMESSAGE",
        "c_struct": "IntuiMessage",
        "header": "intuition/intuition.h",
        "comment": "Intuition event delivery; embeds struct Message at the head",
        "fields": [
            "Embedded ExecMessage (struct Message)",
            ("ln_Succ",      "ExecMessage.mn_Node.ln_Succ", "L"),
            ("ln_Pred",      "ExecMessage.mn_Node.ln_Pred", "L"),
            ("ln_Type",      "ExecMessage.mn_Node.ln_Type", "B"),
            ("ln_Pri",       "ExecMessage.mn_Node.ln_Pri",  "b"),
            ("ln_Name",      "ExecMessage.mn_Node.ln_Name", "S"),
            ("mn_ReplyPort", "ExecMessage.mn_ReplyPort",    "P"),
            ("mn_Length",    "ExecMessage.mn_Length",       "H"),
            "IntuiMessage-specific fields",
            ("Class",        "Class",                       "L"),
            ("Code",         "Code",                        "H"),
            ("Qualifier",    "Qualifier",                   "H"),
            ("IAddress",     "IAddress",                    "P"),
            ("MouseX",       "MouseX",                      "h"),
            ("MouseY",       "MouseY",                      "h"),
            ("Seconds",      "Seconds",                     "L"),
            ("Micros",       "Micros",                      "L"),
            ("IDCMPWindow",  "IDCMPWindow",                 "P"),
            ("SpecialLink",  "SpecialLink",                 "P"),
        ],
    },
]


def find_gcc(name):
    full = shutil.which(name)
    if full is None:
        candidate = "/opt/amiga/bin/" + name
        if os.path.isfile(candidate):
            return candidate
        raise SystemExit("error: %s not found on PATH or in /opt/amiga/bin"
                         % name)
    return full


def collect_headers(specs):
    """Preserve first-seen order so the output comment lists headers in spec
    order rather than alphabetical."""
    seen, ordered = set(), []
    for s in specs:
        h = s["header"]
        if h not in seen:
            seen.add(h)
            ordered.append(h)
    return ordered


def iter_fields(spec):
    """Yield only the field tuples from a spec's `fields` list, skipping any
    string entries (which exist solely as comment dividers in the output)."""
    for entry in spec["fields"]:
        if isinstance(entry, str):
            continue
        yield entry


def build_c_source(specs, headers):
    """Generate the harvester C source.

    Three label families per struct:
      _x_<NAME>__SIZE          -- sizeof(struct X)
      _x_<NAME>__OFF__<field>  -- offsetof(struct X, path)
      _x_<NAME>__SZ__<field>   -- sizeof(((X*)0)->path), only for type 's'
    """
    lines = ["#include <stddef.h>", "#include <exec/types.h>"]
    for h in headers:
        lines.append("#include <%s>" % h)
    lines.append("")
    for s in specs:
        sname = s["c_struct"]
        pname = s["py_name"]
        lines.append("const long _x_%s__SIZE = "
                     "(long)sizeof(struct %s);" % (pname, sname))
        for entry in iter_fields(s):
            py_key, c_path, code = entry[0], entry[1], entry[2]
            lines.append("const long _x_%s__OFF__%s = "
                         "(long)offsetof(struct %s, %s);"
                         % (pname, py_key, sname, c_path))
            if code == "s":
                lines.append("const long _x_%s__SZ__%s = "
                             "(long)sizeof(((struct %s *)0)->%s);"
                             % (pname, py_key, sname, c_path))
        lines.append("")
    return "\n".join(lines) + "\n"


# bebbo m68k-amigaos-gcc prefixes every global with an extra '_', so the C
# identifier `_x_FOO` becomes `__x_FOO` in the emitted assembly.  Zero-valued
# `const long` globals land in BSS as `.skip 4`; non-zero ones land in .data
# (or .rodata) as `.long N`, so the regex accepts either form.
_ASM_LABEL_RE = re.compile(
    r"^__x_([A-Za-z_][A-Za-z0-9_]*):\s*\n"
    r"(?:[ \t]+(?:\.align|\.even|\.type|\.size|\.section|\.local|\.global)"
    r"[^\n]*\n)*"
    r"[ \t]+(?:\.long\s+(\S+)|\.skip\s+(\d+))",
    re.MULTILINE,
)


def compile_and_parse(gcc, include_dir, c_src):
    """Compile to assembly, parse `.long` literals, return a `{label: int}` dict."""
    with tempfile.TemporaryDirectory() as td:
        cpath = os.path.join(td, "structs.c")
        spath = os.path.join(td, "structs.s")
        with open(cpath, "w") as f:
            f.write(c_src)
        result = subprocess.run(
            [gcc, "-S", "-O2", "-m68020", "-w",
             "-I", include_dir, "-o", spath, cpath],
            capture_output=True, text=True,
        )
        if result.returncode != 0:
            sys.stderr.write(result.stderr)
            sys.stderr.write(
                "\namiga-structgen: cross-gcc rejected the harvester source. "
                "A field name in SPECS probably doesn't exist on this NDK; "
                "the stderr above identifies the offending line in structs.c "
                "(line numbers match the in-memory source generated by "
                "build_c_source()).\n"
            )
            raise SystemExit("amiga-structgen: compile failed")
        with open(spath) as f:
            asm = f.read()

    values = {}
    for m in _ASM_LABEL_RE.finditer(asm):
        name = m.group(1)
        long_val = m.group(2)
        skip_val = m.group(3)
        if long_val is not None:
            v = long_val.strip()
            try:
                if v.startswith("0x") or v.startswith("0X"):
                    values[name] = int(v, 16)
                else:
                    values[name] = int(v)
            except ValueError:
                # Symbolic reference (shouldn't happen for constant exprs we
                # emit); just skip rather than crash.
                continue
        else:
            # .skip N -- zero-initialised BSS, value is 0 regardless of N.
            values[name] = 0
    return values


def emit_python(specs, values, out, headers):
    """Render the Python output module.

    Field rows are formatted in the established hand-curated style:

        "key":  (N,  "L"),       # optional note

    Key + ":" is padded so the ``(`` lands at the same column for every field
    in the struct (with a two-space minimum gap past the longest key).  The
    offset+comma is right-padded inside the parens so the type-code letter
    lines up too.  String entries in `fields` are emitted as bare comment
    lines inside the dict body for embedded-struct section dividers.
    """
    out.write("# Auto-generated by tools/amiga-structgen.py -- do not edit.\n")
    out.write("# Sources (in spec order):\n")
    for h in headers:
        out.write("#   %s\n" % h)
    out.write("\n")
    out.write('"""Struct layouts harvested from the AmigaOS NDK for '
              '`amiga.Struct`.\n\n')
    out.write("Each ``<NAME>`` dict maps a field name to "
              "``(offset, type_code)``;\n")
    out.write("``<NAME>_SIZE`` is the matching ``sizeof(struct X)`` for "
              "callers that\n")
    out.write("want to ``AllocVec`` an instance themselves. Type codes are\n")
    out.write("documented on ``amiga.Struct``.\n")
    out.write('"""\n')

    total_fields = 0
    for s in specs:
        pname = s["py_name"]
        sname = s["c_struct"]
        header = s["header"]
        comment = s["comment"]

        size_label = "%s__SIZE" % pname
        if size_label not in values:
            raise SystemExit("amiga-structgen: missing sizeof for struct %s"
                             % sname)
        size = values[size_label]

        # First pass: walk entries, resolving field offsets/sizes and tagging
        # each entry as either a divider or a field row.
        rendered = []
        for entry in s["fields"]:
            if isinstance(entry, str):
                rendered.append(("divider", entry))
                continue
            py_key, c_path, code = entry[0], entry[1], entry[2]
            note = entry[3] if len(entry) > 3 else None
            off_label = "%s__OFF__%s" % (pname, py_key)
            if off_label not in values:
                raise SystemExit(
                    "amiga-structgen: missing offset for %s.%s (%s)"
                    % (sname, py_key, c_path))
            offset = values[off_label]
            if code == "s":
                sz_label = "%s__SZ__%s" % (pname, py_key)
                if sz_label not in values:
                    raise SystemExit(
                        "amiga-structgen: missing sizeof for %s.%s (%s)"
                        % (sname, py_key, c_path))
                code = "s%d" % values[sz_label]
            rendered.append(("field", py_key, offset, code, note))
            total_fields += 1

        # Second pass: width-align over the field rows only.
        field_rows = [r for r in rendered if r[0] == "field"]
        key_w = max(len('"%s":' % r[1]) for r in field_rows)
        off_w = max(len("%d," % r[2]) for r in field_rows)

        out.write("\n# struct %s (%s) -- %s.\n" % (sname, header, comment))
        out.write("%s_SIZE = %d\n" % (pname, size))
        out.write("%s = {\n" % pname)
        for row in rendered:
            if row[0] == "divider":
                out.write("    # %s\n" % row[1])
                continue
            _, py_key, offset, code, note = row
            # 2 spaces minimum gap between the longest key and `(`.
            key_str = ('"%s":' % py_key).ljust(key_w + 2)
            # 1 space minimum gap between the longest offset+"," and the code.
            off_str = ("%d," % offset).ljust(off_w + 1)
            line = '    %s(%s"%s"),' % (key_str, off_str, code)
            if note:
                line += "   # " + note
            out.write(line + "\n")
        out.write("}\n")

    return total_fields


def main(argv=None):
    p = argparse.ArgumentParser(
        description=__doc__.splitlines()[0],
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("--gcc", default=DEFAULT_GCC,
                   help="cross-compiler binary (default: %s)" % DEFAULT_GCC)
    p.add_argument("--ndk-include", default=DEFAULT_INCLUDE,
                   help="NDK include root (default: %s)" % DEFAULT_INCLUDE)
    p.add_argument("--output", "-o", default="-",
                   help="output file (default: stdout)")
    p.add_argument("--dump-c", action="store_true",
                   help="print the generated harvester C source to stderr "
                        "(for debugging spec changes)")
    args = p.parse_args(argv)

    gcc = find_gcc(args.gcc)
    inc = Path(args.ndk_include)
    if not inc.is_dir():
        p.error("--ndk-include: not a directory: %s" % inc)

    headers = collect_headers(SPECS)
    c_src = build_c_source(SPECS, headers)
    if args.dump_c:
        sys.stderr.write("---- generated harvester source ----\n")
        sys.stderr.write(c_src)
        sys.stderr.write("---- end harvester source ----\n")

    values = compile_and_parse(gcc, str(inc), c_src)

    out = sys.stdout if args.output == "-" else open(args.output, "w")
    try:
        total = emit_python(SPECS, values, out, headers)
    finally:
        if out is not sys.stdout:
            out.close()

    sys.stderr.write(
        "amiga-structgen: %d structs, %d fields emitted (%s)\n"
        % (len(SPECS), total,
           "stdout" if args.output == "-" else args.output)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
