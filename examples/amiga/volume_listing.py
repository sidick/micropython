# Volume / assign introspection + pattern matching + tree walk.
#
# Shell-only; runs fine under both vamos and Amiberry. Useful when
# you need to know "what's mounted right now" without parsing
# `info` output.

import amiga
import os

print("=== Mounted volumes ===")
for v in amiga.volumes():
    print("  ", v)

print("\n=== Assigns (first 20) ===")
for name, target in list(amiga.assigns().items())[:20]:
    print("  %-12s -> %s" % (name, target))

# AmigaDOS pattern matching: ? is a single-char wildcard, # is
# any-count of the next char, so #? matches anything.
print("\n=== Sys: top-level .info files ===")
for path in amiga.match("Sys:#?.info"):
    print("  ", path)

# Recursive walk via os.walk, AmigaOS-aware via the frozen os.path.
print("\n=== os.walk of T: (RAM:T) ===")
count_dirs = count_files = 0
for dirpath, dirs, files in os.walk("T:"):
    count_dirs += len(dirs)
    count_files += len(files)
print("  dirs=%d files=%d" % (count_dirs, count_files))
