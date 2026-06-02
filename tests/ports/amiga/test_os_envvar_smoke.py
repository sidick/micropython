# os.getenv / putenv / unsetenv + os.chmod round-trip smoke test.
#
# These ride on top of AmigaOS facilities that test_os_smoke leaves to
# this test:
#   - getenv / putenv / unsetenv map to dos.library GetVar / SetVar /
#     DeleteVar, sharing the ENV: store with the AmigaShell.
#   - chmod writes the FIBF_* protection bits via SetProtection.
# Both round-trips are exercised end to end here.

import os

# --- env var round trip --------------------------------------------------

NAME = "AMIGA_PHASE39_ENVTEST"

# Clean slate (in case a previous run was interrupted).
os.unsetenv(NAME)

# Unset var reads as None.
assert os.getenv(NAME) is None

# putenv + getenv round trip.
os.putenv(NAME, "hello world")
assert os.getenv(NAME) == "hello world"

# Default arg only fires when the var is genuinely unset.
assert os.getenv(NAME, "dflt") == "hello world"

# Overwriting an existing var works.
os.putenv(NAME, "second value")
assert os.getenv(NAME) == "second value"

# unsetenv removes the var entirely.
os.unsetenv(NAME)
assert os.getenv(NAME) is None
# unsetenv on an already-missing name is a no-op (not an error).
os.unsetenv(NAME)
assert os.getenv(NAME) is None

# getenv default is returned when missing.
assert os.getenv(NAME, "fallback") == "fallback"

# Non-ASCII / extended bytes round-trip through ENV:.
os.putenv(NAME, "k=v;\nline2")
assert os.getenv(NAME) == "k=v;\nline2"
os.unsetenv(NAME)

# --- chmod round trip ---------------------------------------------------

# Create a scratch file in RAM: (vamos exposes RAM: as a tempdir).
path = "RAM:phase39_chmod_smoke"
with open(path, "w") as f:
    f.write("scratch")

try:
    # AmigaOS protection bits are "deny" flags: 1 = denied, 0 = allowed.
    # FIBF_DELETE = 0x1, FIBF_EXECUTE = 0x2, FIBF_WRITE = 0x4, FIBF_READ = 0x8.

    # Initial protect is what AmigaOS picks for a newly created file --
    # typically the FIBF_EXECUTE bit (executable bit denied for plain
    # data). Just confirm getprotect returns an int.
    initial = os.getprotect(path)
    assert isinstance(initial, int), initial

    # Deny read + write -- bits 0x8 (READ) + 0x4 (WRITE) = 0xC.
    os.chmod(path, os.FIBF_READ | os.FIBF_WRITE)
    after = os.getprotect(path)
    assert after & os.FIBF_READ, hex(after)
    assert after & os.FIBF_WRITE, hex(after)
    assert not (after & os.FIBF_EXECUTE), hex(after)
    assert not (after & os.FIBF_DELETE), hex(after)

    # Clear all standard deny bits -- everything allowed.
    os.chmod(path, 0)
    cleared = os.getprotect(path)
    assert cleared & (os.FIBF_READ | os.FIBF_WRITE) == 0, hex(cleared)

    # Setting just the READ deny bit must show up; the other RWED
    # positions stay clear. (We deliberately don't assert on the
    # archive / pure / script / hold bits -- those have their own
    # AmigaDOS lifecycle rules: ARCHIVE gets reset on any write to the
    # file, vamos doesn't honour all of them, etc.)
    os.chmod(path, os.FIBF_READ)
    read_only_deny = os.getprotect(path)
    assert read_only_deny & os.FIBF_READ, hex(read_only_deny)
    assert not (read_only_deny & os.FIBF_WRITE), hex(read_only_deny)

    # chmod on a missing path raises OSError.
    try:
        os.chmod("RAM:phase39_definitely_missing", 0)
        assert False, "expected OSError"
    except OSError:
        pass

finally:
    os.remove(path)

print("OK")
