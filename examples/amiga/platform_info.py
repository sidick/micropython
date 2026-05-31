# Full system-identity probe.
#
# Walks the CPython-shaped `platform` surface plus the underlying
# `amiga` accessors that back `platform.amiga_info()`. Useful as a
# diagnostic when the binary moves between machines.

import platform
import amiga

print("system        :", platform.system())
print("machine       :", platform.machine())
print("processor     :", platform.processor())
print("release       :", platform.release())
print("version       :", platform.version())
print("python impl   :", platform.python_implementation())
print("python ver    :", platform.python_version())
print("platform      :", platform.platform())
print()
print("--- amiga.* direct accessors ---")
print("cpu           :", amiga.cpu())
print("fpu           :", amiga.fpu())
print("kickstart     :", amiga.kickstart())
print("chipmem (KB)  :", amiga.chipmem() // 1024)
print("fastmem (KB)  :", amiga.fastmem() // 1024)
try:
    print("chipset       :", amiga.chipset())
except OSError as e:
    # graphics.library not available (vamos).
    print("chipset       : <no graphics.library:", e, ">")
print()
print(platform.amiga_info())
