# Minimal sanity check for the AmigaOS port.
#
# Prints the runtime identity and the one-line AmigaOS summary so you
# can confirm the binary is alive and the platform helpers work.

import sys
import platform

print("hello from", sys.implementation.name, sys.implementation.version[:3])
print(platform.platform())
try:
    print(platform.amiga_info())
except OSError as e:
    # graphics.library not available (e.g. vamos).
    print("(no amiga_info -- chipset() failed:", e, ")")
