# Read a Workbench icon, mutate it, write it back.
#
# Demonstrates amiga.icon.read / icon.write / icon.new and the
# tooltype mapping. Operates on RAM: so it doesn't touch the user's
# system disk.

from amiga import icon

# Build a fresh project icon from the system default image.
new = icon.new(
    icon.WBPROJECT,
    default_tool="C:Ed",
    stack_size=8192,
    tooltypes={
        "WINDOW": "CON:0/0/640/256/Test",
        "FILETYPE": b"ASCII",
        "DONOTWAIT": None,  # flag-style: present with no '='
    },
)
print("new icon: type=%s default_tool=%r stack=%d"
      % (new.type, new.default_tool, new.stack_size))

icon.write("RAM:demo", new)
new.close()
print("wrote RAM:demo.info")

# Read it back and confirm the round trip.
back = icon.read("RAM:demo")
print("read back: type=%s default_tool=%r stack=%d"
      % (back.type, back.default_tool, back.stack_size))
print("tooltypes :")
for k, v in back.tooltypes.items():
    print("  %-10s = %r" % (k, v))
back.close()

# Inspect an existing system icon -- pick whichever exists on this
# install. Sys:Prefs is the safest bet on a stock Workbench.
try:
    sys_icon = icon.read("Sys:Prefs")
    print("\nSys:Prefs:")
    print("  type     :", sys_icon.type)
    print("  position : (%d, %d)" % (sys_icon.current_x, sys_icon.current_y))
    print("  tooltypes:", list(sys_icon.tooltypes))
    sys_icon.close()
except OSError as e:
    print("\nSys:Prefs.info unavailable:", e)
