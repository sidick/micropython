# Phase 30 wiring smoke test — exercises argument validation and the
# C-to-Python plumbing for the `_intuition` module + its `amiga.intuition`
# facade. Runs under vamos with no GUI; vamos has a no-op EasyRequest
# stub so the actual modal-popup path can't be tested here. Use Amiberry
# (interactive) for end-to-end visual confirmation.

import _intuition
import amiga

# Module registration + alias chain.
assert _intuition is amiga.intuition, "amiga.intuition should alias _intuition"
assert callable(_intuition.easy_request)
assert callable(_intuition.auto_request)
assert callable(_intuition.message)

# Empty buttons list → TypeError before any library work happens.
try:
    _intuition.easy_request("t", "b", [])
except TypeError as e:
    assert "button" in str(e), e
else:
    raise AssertionError("expected TypeError for empty buttons list")

# Non-iterable buttons → TypeError from mp_getiter.
try:
    _intuition.easy_request("t", "b", 5)
except TypeError:
    pass
else:
    raise AssertionError("expected TypeError for non-iterable buttons")

# Calling through the facade works (raises OSError on hosts without
# intuition.library, which is fine -- the import + dispatch is what
# we're checking).
try:
    _intuition.easy_request("Title", "Body.", ["OK"])
except OSError:
    pass

print("OK")
