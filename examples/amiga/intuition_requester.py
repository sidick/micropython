# Modal-requester dialogs via amiga.intuition.
#
# Wraps intuition.library's EasyRequestArgs. Best run on a real Amiga
# or Amiberry; vamos's intuition stub doesn't open a screen.

from amiga import intuition

# Single-button informational dialog. The button defaults to "OK".
intuition.message("MicroPython is alive.")

# Yes/No dialog -- returns True if the leftmost button was clicked.
agreed = intuition.auto_request("Continue with the demo?")
print("user agreed:", agreed)

# Multi-button requester -- returns the 0-based index of the picked
# button (counting from the LEFT, matching Python list ordering).
choice = intuition.easy_request(
    "Choose colour",
    "Pick your favourite primary:",
    ["Red", "Green", "Blue", "Cancel"],
)
print("picked index:", choice)
print("picked label:", ("Red", "Green", "Blue", "Cancel")[choice])
