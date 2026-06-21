# Phase 33 wiring smoke -- frozen platform.py + underlying amiga.*
# accessors. Runs under vamos with no graphics.library, so chipset()
# may fail with OSError; we trap and skip that one check.

import platform
import amiga
import sys

# CPython-shaped surface.
assert platform.system() == "AmigaOS"
assert platform.node() == "Amiga"
assert platform.python_implementation() == "MicroPython"

m = platform.machine()
assert isinstance(m, str) and m.startswith("680"), m
assert platform.processor() == m  # same source

rel = platform.release()
assert isinstance(rel, str) and "." in rel, rel
assert platform.version() == "Kickstart " + rel

pv = platform.python_version()
expected = "%d.%d.%d" % tuple(sys.implementation.version[:3])
assert pv == expected, (pv, expected)

# CPython-shaped build info (exercised by extmod/platform_basic.py):
# python_compiler() -> str, libc_ver() -> (lib, ver) tuple.
assert isinstance(platform.python_compiler(), str)
lv = platform.libc_ver()
assert isinstance(lv, tuple) and len(lv) == 2, lv
assert all(isinstance(x, str) for x in lv), lv

# platform() should combine kickstart / cpu / python_version.
pl = platform.platform()
assert rel in pl and m in pl and pv in pl, pl

# Memory accessors return ints.
assert isinstance(platform.chipmem(), int)
assert isinstance(platform.fastmem(), int)
assert isinstance(platform.fpu(), str)

# amiga_info() needs chipset() which needs graphics.library; skip on
# vamos. On Amiberry it returns the formatted single-line summary.
try:
    info = platform.amiga_info()
    assert isinstance(info, str)
    for token in ("CPU:", "FPU:", "Chipset:", "Kickstart:", "Chip:", "Fast:"):
        assert token in info, (token, info)
    print("info=", info)
except OSError:
    print("amiga_info skipped (no graphics.library under vamos)")

print("OK")
