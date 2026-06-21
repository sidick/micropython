"""AmigaOS-aware `os.path` helpers.

Re-exported as `os.path` by the frozen `os.py`.  Models the AmigaOS
path policy:

* The *first* `:` in a path terminates the volume reference
  (``Sys:Prefs/Workbench``); subsequent `/` characters separate
  components.
* A path with `:` at position 0 (``:foo``) references the current
  volume -- treated as absolute.
* No path component can contain `:`; only the volume terminator
  does.

Differences from CPython's `os.path`:

* `join` doesn't insert a separator after a colon (``join("Work:",
  "scripts") == "Work:scripts"``) but does after anything else.
* `isabs` is True for *any* path containing `:` -- not just position
  0 -- matching AmigaDOS conventions.
* `normpath` won't traverse a `..` past the volume boundary (it
  preserves volume references verbatim).
* `expanduser`, `expandvars`, `commonpath`, `relpath`, `samefile`
  aren't included; users who need them can wrap `dos.library`
  GetVar themselves.
"""

import os as _os


def _split_volume(p):
    """Return (volume_prefix, rest) where `volume_prefix` ends with `:`
    if `p` contains a `:`, otherwise the empty string.

    `volume_prefix` is the part *including* the trailing `:`, so
    rebuilding via `volume_prefix + rest` round-trips.
    """
    i = p.find(":")
    if i == -1:
        return "", p
    return p[: i + 1], p[i + 1 :]


def isabs(p):
    """True if `p` is an absolute AmigaOS path.

    Anything with a `:` qualifies -- ``Work:foo``, ``Sys:``, ``:bar``
    are all absolute references.  Pure relative paths (no colon)
    return False.
    """
    return ":" in p


def join(*parts):
    """Concatenate path components AmigaOS-style.

    A part containing `:` resets the join (it's a fresh absolute
    reference).  A separator is inserted between two components
    unless the running prefix already ends with `:` or `/`.

        >>> join("Work:", "scripts", "foo.py")
        'Work:scripts/foo.py'
        >>> join("Work:scripts", "foo.py")
        'Work:scripts/foo.py'
        >>> join("foo", "bar")
        'foo/bar'
        >>> join("Work:", "bin", "C:tool")
        'C:tool'
    """
    result = ""
    for p in parts:
        if not p:
            continue
        if ":" in p:
            result = p
            continue
        if not result:
            result = p
        elif result.endswith(":") or result.endswith("/"):
            result = result + p
        else:
            result = result + "/" + p
    return result


def split(p):
    """Split `p` into ``(dirname, basename)``.

    The separator character is retained on the dirname side, so
    ``join(*split(p)) == p`` when `p` doesn't end with a separator.

        >>> split("Work:scripts/foo.py")
        ('Work:scripts', 'foo.py')
        >>> split("Work:foo.py")
        ('Work:', 'foo.py')
        >>> split("foo.py")
        ('', 'foo.py')
        >>> split("Work:")
        ('Work:', '')
    """
    # Look for the last separator (either '/' or ':').  Preserve the
    # ':' on the dirname side because AmigaDOS uses ':' as the volume
    # terminator, not as a directory separator.
    i_slash = p.rfind("/")
    i_colon = p.rfind(":")
    sep = max(i_slash, i_colon)
    if sep == -1:
        return "", p
    if sep == i_colon:
        # ':' is part of the volume reference; keep it on dirname.
        return p[: sep + 1], p[sep + 1 :]
    return p[:sep], p[sep + 1 :]


def splitext(p):
    """Split `p` into ``(root, ext)`` at the last `.` after the
    last path separator.  An empty `ext` is returned if the
    basename has no `.`, or if the only `.` is the first character
    (dot-file convention).

        >>> splitext("Work:scripts/foo.py")
        ('Work:scripts/foo', '.py')
        >>> splitext("foo")
        ('foo', '')
        >>> splitext("Work:.profile")
        ('Work:.profile', '')
    """
    head, tail = split(p)
    dot = tail.rfind(".")
    if dot <= 0:  # no dot, or leading dot only
        return p, ""
    if head:
        return join(head, tail[:dot]), tail[dot:]
    return tail[:dot], tail[dot:]


def basename(p):
    return split(p)[1]


def dirname(p):
    return split(p)[0]


def normpath(p):
    """Collapse `.` and `..` components without crossing the volume
    boundary.

    AmigaDOS uses `/` at the start of a component for "parent
    directory" (e.g. ``foo//bar`` is "../bar" relative to foo's
    parent's parent).  We honour the POSIX-style `..` convention,
    which is what callers writing portable code reach for, but
    leave the AmigaDOS `/` convention untouched.
    """
    prefix, rest = _split_volume(p)
    components = []
    for part in rest.split("/"):
        if part == "" or part == ".":
            continue
        if part == "..":
            if components:
                components.pop()
            # else: clamp at the volume boundary; no parent of "Work:"
            continue
        components.append(part)
    body = "/".join(components)
    if not prefix and not body:
        return "."
    return prefix + body


def abspath(p):
    """`p` if already absolute, otherwise prepended with the current
    working directory."""
    if isabs(p):
        return normpath(p)
    return normpath(join(_os.getcwd(), p))


def exists(p):
    """True if `p` resolves to a file, directory or volume."""
    try:
        _os.stat(p)
        return True
    except OSError:
        return False


def isdir(p):
    """True if `p` is a directory (or a volume)."""
    try:
        mode = _os.stat(p)[0]
    except OSError:
        return False
    return bool(mode & 0x4000)


def isfile(p):
    """True if `p` is a regular file."""
    try:
        mode = _os.stat(p)[0]
    except OSError:
        return False
    return bool(mode & 0x8000)
