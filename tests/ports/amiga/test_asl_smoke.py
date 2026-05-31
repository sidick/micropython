# Phase 31 wiring smoke test -- exercises argument validation and the
# C-to-Python plumbing for the `_asl` module + its `amiga.asl` facade.
# Runs under vamos with no GUI; vamos has no usable asl.library stub so
# the actual file-pick path can't be tested here. Use Amiberry
# (interactive) for end-to-end visual confirmation.

import _asl
import amiga

# Module registration + alias chain.
assert amiga.asl is _asl, "amiga.asl should alias _asl"
assert callable(_asl.file_request)

# multi=True + save=True is contradictory and must raise ValueError
# before any library call happens.
try:
    _asl.file_request(multi=True, save=True)
except ValueError as e:
    assert "multi" in str(e), e
else:
    raise AssertionError("expected ValueError for multi=True + save=True")

# Non-string title rejected by mp_obj_str_get_str.
try:
    _asl.file_request(title=42)
except TypeError:
    pass
else:
    raise AssertionError("expected TypeError for non-string title")

print("OK")
