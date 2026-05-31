"""Subset of CPython's `platform` module for AmigaOS.

Surfaces AmigaOS identity (CPU / FPU / chipset / Kickstart / memory)
through the standard CPython-shaped API.  Modeled on OoZe1911's
port for surface compatibility -- scripts written against their
`platform` should run unchanged here.

    >>> import platform
    >>> platform.system()
    'AmigaOS'
    >>> platform.machine()
    '68020'
    >>> platform.amiga_info()
    'CPU: 68020 | FPU: 68881 | Chipset: AGA | Kickstart: 45.57 | Chip: 1856KB | Fast: 14336KB'

The CPU / FPU strings reflect runtime detection
(`SysBase->AttnFlags`), not the compile-time `-m680XX` flag, so a
`standard` (68020 / soft-float) binary running on a 68040 reports
``'68040'`` / ``'68040'``.

Memory values from `chipmem()` / `fastmem()` are *currently free*
bytes (`AvailMem(MEMF_*)`), not total installed -- matches what a
user wants from "how much do I have available right now."
"""

import sys
import amiga as _amiga


def system():
    return "AmigaOS"


def node():
    # AmigaOS has no NodeName concept (no hostname). Constant string
    # matches OoZe1911's behaviour.
    return "Amiga"


def machine():
    return _amiga.cpu()


def processor():
    return _amiga.cpu()


def release():
    return _amiga.kickstart()


def version():
    return "Kickstart " + _amiga.kickstart()


def python_implementation():
    return "MicroPython"


def python_version():
    v = sys.implementation.version
    return "%d.%d.%d" % (v[0], v[1], v[2])


def platform():
    return "AmigaOS-%s-%s-MicroPython_%s" % (
        _amiga.kickstart(), _amiga.cpu(), python_version())


def fpu():
    return _amiga.fpu()


def chipset():
    return _amiga.chipset()


def chipmem():
    return _amiga.chipmem()


def fastmem():
    return _amiga.fastmem()


def amiga_info():
    """One-line dump: ``CPU | FPU | Chipset | Kickstart | Chip | Fast``."""
    return "CPU: %s | FPU: %s | Chipset: %s | Kickstart: %s | Chip: %dKB | Fast: %dKB" % (
        _amiga.cpu(), _amiga.fpu(), _amiga.chipset(), _amiga.kickstart(),
        _amiga.chipmem() // 1024, _amiga.fastmem() // 1024)
