# amiga.Library proxy + .fd-driven trampoline.
#
# Opens intuition.library, calls DisplayBeep(NULL) to flash the
# screen, then closes. The .fd signature is loaded automatically from
# the frozen NDK-derived table; no per-function declarations needed.

import amiga

with amiga.library("intuition.library", 37) as intuition:
    # DisplayBeep is documented as taking a Screen* in A0; passing 0
    # means "the default public screen", which is normally Workbench.
    intuition.DisplayBeep(0)
    print("beep sent")

# A second example -- exec.library's AvailMem with a flags arg in D1.
with amiga.library("exec.library", 33) as execlib:
    chip_free = execlib.AvailMem(2)  # MEMF_CHIP = 2
    fast_free = execlib.AvailMem(4)  # MEMF_FAST = 4
    print("chip free:", chip_free, "fast free:", fast_free)
