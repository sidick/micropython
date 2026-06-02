# VfsAmiga (ports/amiga/vfs_amiga.c) targeted smoke test.
#
# Exercises file I/O operations that route through the port's
# dos.library wrapper: open() / read / write / seek / tell / readline /
# ilistdir / stat / rename / mkdir / rmdir / remove / context manager.
# Uses RAM: (vamos maps it to a host tempdir; on a real Amiga it's the
# RAM disk) so the test stays self-contained and doesn't disturb
# anything on disk.

import os
import _ospath

# Pick a scratch root nobody else should be using.
ROOT = "RAM:vfs_smoke_phase39"

# Clean any leftover from a previous interrupted run, then create
# our scratch tree.
def _cleanup():
    try:
        for name in os.listdir(ROOT):
            path = ROOT + "/" + name
            try:
                os.remove(path)
            except OSError:
                # Probably a sub-directory left over -- try rmdir.
                try:
                    os.rmdir(path)
                except OSError:
                    pass
        os.rmdir(ROOT)
    except OSError:
        pass


_cleanup()
os.mkdir(ROOT)
try:
    assert _ospath.isdir(ROOT)

    # --- write / read round-trip ---------------------------------------

    payload = b"hello\nVfsAmiga\nphase39\n"
    with open(ROOT + "/data.txt", "wb") as f:
        n = f.write(payload)
        assert n == len(payload), n

    with open(ROOT + "/data.txt", "rb") as f:
        got = f.read()
        assert got == payload, got

    # --- text mode -----------------------------------------------------

    with open(ROOT + "/text.txt", "w") as f:
        f.write("alpha\nbeta\ngamma\n")

    with open(ROOT + "/text.txt", "r") as f:
        assert f.readline() == "alpha\n"
        assert f.readline() == "beta\n"
        assert f.readline() == "gamma\n"
        assert f.readline() == ""  # EOF

    # readlines round-trip.
    with open(ROOT + "/text.txt", "r") as f:
        assert f.readlines() == ["alpha\n", "beta\n", "gamma\n"]

    # --- seek / tell ---------------------------------------------------

    with open(ROOT + "/data.txt", "rb") as f:
        assert f.tell() == 0
        f.seek(6)  # past "hello\n"
        assert f.tell() == 6
        assert f.read(9) == b"VfsAmiga\n"
        # Seek from current.
        f.seek(0, 0)
        assert f.tell() == 0
        # Seek from end.
        f.seek(0, 2)
        assert f.tell() == len(payload)

    # --- append mode ---------------------------------------------------

    with open(ROOT + "/data.txt", "ab") as f:
        f.write(b"appended\n")

    with open(ROOT + "/data.txt", "rb") as f:
        assert f.read() == payload + b"appended\n"

    # --- ilistdir yields names + types ---------------------------------

    entries = list(os.ilistdir(ROOT))
    names = [e[0] for e in entries]
    assert "data.txt" in names
    assert "text.txt" in names
    # Type field: 0x4000 = directory, 0x8000 = regular file (CPython
    # convention also used by extmod/vfs). data.txt is a file.
    for name, type_, inode, *rest in entries:
        if name == "data.txt":
            assert type_ == 0x8000, hex(type_)

    # --- stat ----------------------------------------------------------

    st = os.stat(ROOT + "/data.txt")
    # st_mode bit 0o100000 set -> regular file.
    assert st[0] & 0o100000, oct(st[0])
    # st_size matches the on-disk byte count.
    assert st[6] == len(payload) + len(b"appended\n"), st

    # stat() on a missing path raises OSError.
    try:
        os.stat(ROOT + "/nope.xyz")
        assert False, "expected OSError"
    except OSError:
        pass

    # --- rename --------------------------------------------------------

    os.rename(ROOT + "/text.txt", ROOT + "/renamed.txt")
    assert _ospath.isfile(ROOT + "/renamed.txt")
    assert not _ospath.exists(ROOT + "/text.txt")

    # --- subdir + chdir + getcwd --------------------------------------

    os.mkdir(ROOT + "/sub")
    assert _ospath.isdir(ROOT + "/sub")

    cwd_before = os.getcwd()
    os.chdir(ROOT + "/sub")
    cwd_after = os.getcwd()
    # The new cwd must reflect the change. AmigaDOS may report it with
    # different casing on the volume root, so we compare case-insensitive.
    assert cwd_after.lower().endswith("vfs_smoke_phase39/sub"), cwd_after

    # Relative open should now resolve inside /sub.
    with open("nested.txt", "w") as f:
        f.write("inside sub")
    assert _ospath.isfile(ROOT + "/sub/nested.txt")

    # Go back so cleanup can rm the scratch tree.
    os.chdir(cwd_before)
    os.remove(ROOT + "/sub/nested.txt")
    os.rmdir(ROOT + "/sub")

    # --- error paths ---------------------------------------------------

    # open() on a missing file in read mode raises OSError.
    try:
        open(ROOT + "/missing.dat", "rb").close()
        assert False, "expected OSError"
    except OSError:
        pass

    # mkdir on an existing directory raises OSError.
    try:
        os.mkdir(ROOT)
        assert False, "expected OSError"
    except OSError:
        pass

    # rmdir on a non-empty directory raises OSError.
    # (data.txt + renamed.txt are still in ROOT.)
    try:
        os.rmdir(ROOT)
        assert False, "expected OSError on non-empty rmdir"
    except OSError:
        pass

    # --- context-manager close ----------------------------------------

    f = open(ROOT + "/data.txt", "rb")
    with f:
        assert f.read(1) == b"h"
    # After __exit__, further reads should raise.
    try:
        f.read(1)
        assert False, "expected OSError on closed file"
    except (OSError, ValueError):
        pass

finally:
    _cleanup()

print("OK")
