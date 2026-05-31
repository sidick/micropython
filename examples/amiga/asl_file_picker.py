# ASL file requester via amiga.asl.
#
# Opens the standard AmigaOS file picker and prints the user's
# selection. Real Amiga / Amiberry only; vamos has no asl.library
# implementation.

from amiga import asl

selected = asl.file_request(
    title="Pick a Python script",
    initial_drawer="SYS:",
    pattern="#?.py",
)
if selected is None:
    print("user cancelled")
else:
    print("you picked:", selected)

# Save-style requester: the typed filename can be a new file.
save_path = asl.file_request(
    title="Save where?",
    initial_drawer="RAM:",
    initial_file="output.txt",
    save=True,
)
print("save target:", save_path)

# Multi-select returns a list of full paths.
picks = asl.file_request(
    title="Pick a few files",
    initial_drawer="C:",
    multi=True,
)
if picks:
    print("you picked %d file(s):" % len(picks))
    for p in picks:
        print(" -", p)
