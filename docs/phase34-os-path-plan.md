# Phase 34 — Frozen `os` extensions + AmigaOS-aware `os.path` step plan

Companion to the Phase 34 design block in
[docs/amiga.md](amiga.md#phase-34--frozen-os-extensions--amigaos-aware-ospath-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable PRs.

Phase 34 closes the `os` / `os.path` gap that surfaces when comparing
against OoZe1911's port. The C-side `os` module (registered as
`MP_REGISTER_EXTENSIBLE_MODULE`) merges with a frozen `os.py` so the
existing C entries stay live and the frozen file adds the rest:

```python
import os
os.makedirs("Work:scripts/sub/dir", exist_ok=True)
for root, dirs, files in os.walk("Work:"):
    print(root, len(files))
os.chmod("DH0:foo", 0)
mask = os.getprotect("DH0:foo")

os.path.join("Work:", "scripts", "foo.py")   # 'Work:scripts/foo.py'
os.path.abspath("foo.py")                    # uses cwd, volume-aware
os.path.isabs("Sys:Prefs")                   # True
os.path.normpath("Work:scripts/../bin")      # 'Work:bin'
```

## Phasing overview

```
Step 1: os.chmod + os.getprotect (C entries) + FIBF_* constants
                                          ↓
                              Step 2: Frozen os.py + _ospath.py
                                          ↓
                                Step 3: Docs flip + smoke tests
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | Two new C entries in `modos.c`: `os.chmod(path, mask)` (SetProtection) and `os.getprotect(path)` (Examine + read fib_Protection). `FIBF_*` constants exposed on the module. | New entries via the existing port-local module-globals append (modos already registers via the locals_dict callback that extmod/modos.c provides for extensible modules). | From the REPL under Amiberry: `os.getprotect("S:Startup-Sequence")` returns a non-zero mask; `os.chmod` round-trips. |
| **2** | Frozen `ports/amiga/modules/os.py` with `makedirs` (recursive mkdir, AmigaOS volume-aware) and `walk` (recursive listdir + stat tree generator). Frozen `ports/amiga/modules/_ospath.py` with `join` / `split` / `splitext` / `basename` / `dirname` / `exists` / `isfile` / `isdir` / `isabs` / `abspath` / `normpath`. `os.py` does `import _ospath as path` so `os.path` Just Works. | Two frozen modules. | `os.makedirs` + `os.walk` against a temp tree. `os.path.normpath` collapses `..` correctly across volume separators. |
| **3** | Docs flip, manifest verification, smoke test. | `docs/amiga.md` Phase 34 → ✅. `docs/amiga-testing.md` gains a short `os` / `os.path` subsection. | `tests/amiga/test_os_smoke.py` covers the surface and the volume-separator edge cases under vamos. |

Each step is small: Step 1 is ~50 LOC C, Step 2 is ~150 LOC Python,
Step 3 is paperwork.

---

## Step 1 — `os.chmod` + `os.getprotect` + `FIBF_*` constants

### Deliverables

- Two C functions in `modos.c`:
  ```c
  static mp_obj_t mp_os_chmod(mp_obj_t path_obj, mp_obj_t mask_obj);
  static mp_obj_t mp_os_getprotect(mp_obj_t path_obj);
  ```
- `os.chmod(path, mask)`:
  - Suppress AmigaDOS auto-requesters (`pr_WindowPtr = -1`) around
    the call so a path on an unmounted volume gets a clean OSError
    instead of a system dialog (matches `amiga.exists()` / `amiga.match()`).
  - `SetProtection(name, mask)`; raises OSError on failure with the
    errno mapping `amiga_dos_errno_from`.
- `os.getprotect(path)`:
  - `Lock(path, SHARED_LOCK)` → `Examine(lock, &fib)` →
    `fib.fib_Protection`. UnLock on the way out.
  - Same auto-requester suppression.
  - Returns the raw `fib_Protection` ULONG.
- `FIBF_*` constants added to the module globals via the same
  append mechanism Phase 20 uses for `getenv` / `putenv` / `unsetenv`:
  - `os.FIBF_READ`, `os.FIBF_WRITE`, `os.FIBF_EXECUTE`,
    `os.FIBF_DELETE`, `os.FIBF_ARCHIVE`, `os.FIBF_PURE`,
    `os.FIBF_SCRIPT`, `os.FIBF_HIDDEN`.

### Bit semantics caveat

AmigaDOS protection flags are *inverted* for the four classic
RWED bits — a *set* bit means "denied", a *clear* bit means
"allowed." The bits exposed by `FIBF_READ` / `WRITE` / `EXECUTE` /
`DELETE` follow the on-disk encoding, so `chmod(path, 0)` grants
all four (no bits set = nothing denied). `ARCHIVE`, `PURE`,
`SCRIPT`, `HIDDEN` follow the "set means yes" convention.

Document the inversion in the module docstring so callers aren't
surprised; this is the same convention `Protect` from the
AmigaShell uses.

### Verification

REPL on target:

```python
>>> import os
>>> oct(os.getprotect("S:Startup-Sequence"))   # readable, writable
'0o15'
>>> os.chmod("Ram Disk:test", 0)               # all allowed, no flags
>>> os.getprotect("Ram Disk:test")
0
```

Vamos: `dos.library`'s `SetProtection` / `Examine` work end-to-end
through `mp:` volumes, so the call paths are testable.

---

## Step 2 — Frozen `os.py` + `_ospath.py`

### Deliverables

#### `ports/amiga/modules/os.py`

```python
# Frozen extension for the C-side `os` module. The C entries
# (chdir, getcwd, listdir, mkdir, remove, rename, rmdir, stat,
# statvfs, getenv, putenv, unsetenv, chmod, getprotect, plus the
# FIBF_* constants) are already in this module's globals when
# this file is loaded -- the extensible-module mechanism merges
# them in.

import _ospath as path  # exposes amiga.os.path


def makedirs(name, exist_ok=False):
    """mkdir -p semantics, AmigaOS volume aware."""
    # walk Volume:dir1/dir2/dir3 component by component


def walk(top, topdown=True):
    """Recursive listdir + stat tree generator."""
    # follow same shape as CPython os.walk
```

`makedirs` handles `Work:scripts/sub/dir`:
- Split off `Work:` as the volume prefix (must exist).
- For each subsequent component, accumulate the path with `/` and
  `mkdir` it, tolerating `EEXIST` when `exist_ok=True`.

`walk` follows CPython's `os.walk` shape — `(dirpath, dirnames,
filenames)` triples, `topdown` ordering preserved. AmigaOS paths
join correctly when the parent ends with `:` (no extra `/`) vs
when it ends with anything else.

#### `ports/amiga/modules/_ospath.py`

Pure-Python helpers. Volume-aware policy: the first `:` in a path
is the volume terminator (`Sys:Prefs/Workbench`); subsequent `/`
characters separate components. A leading `:` is illegal in
AmigaOS so we don't need to worry about that edge case.

Functions:
- `join(*parts)` — concatenate with the right separator:
  - `join("Work:", "scripts")` → `"Work:scripts"` (no separator after `:`)
  - `join("Work:scripts", "foo.py")` → `"Work:scripts/foo.py"`
  - A part containing `:` resets the join (it's a fresh absolute path).
- `split(p)` → `(dirname, basename)`. Splits at the last `/` or
  `:`; the separator character is retained on the dirname side.
- `splitext(p)` → `(root, ext)`. Last `.` after the last separator.
- `basename(p)` → second element of `split`.
- `dirname(p)` → first element of `split`.
- `isabs(p)` — has a `:` (volume reference) anywhere in the path,
  not just at index 0. AmigaOS treats `Work:` and `:foo` and
  `Sys:Prefs/Workbench` all as absolute.
- `abspath(p)` — `p` if `isabs(p)`, else `join(getcwd(), p)`.
- `normpath(p)` — collapse `.` and `..` components without crossing
  the volume boundary (`Work:..` is meaningless and stays as-is).
- `exists`, `isfile`, `isdir` — wrap `stat()`.

### Verification

```python
>>> os.path.join("Work:", "scripts", "foo.py")
'Work:scripts/foo.py'
>>> os.path.normpath("Work:scripts/../bin/./tool")
'Work:bin/tool'
>>> os.path.isabs("Sys:")
True
>>> os.path.isabs("foo.py")
False
>>> os.path.split("Work:scripts/foo.py")
('Work:scripts', 'foo.py')
>>> os.path.split("Work:foo.py")
('Work:', 'foo.py')
>>> os.makedirs("Ram Disk:a/b/c", exist_ok=True)
```

---

## Step 3 — Docs + tests

### Deliverables

- `docs/amiga.md` Phase 34 status → ✅; section gains the
  full surface matrix (which calls / which module / what they
  return).
- `docs/amiga-testing.md` short `os` / `os.path` subsection
  with REPL examples and a note about the AmigaDOS inverted-bit
  semantics.
- `tests/amiga/test_os_smoke.py` — vamos-runnable:
  - `os.path.isabs("Sys:")` is True; `isabs("foo.py")` is False
  - `os.path.join` / `split` / `normpath` corner cases
  - `os.makedirs` round-trip against `mp:` tree
  - `os.chmod` / `os.getprotect` round-trip on a temp file
  - `os.walk` yields the expected `(dirpath, dirs, files)` shape

---

## Cross-cutting concerns

- **Volume-vs-directory separator policy.** `:` terminates the
  volume *once* per path; `/` separates directories after that.
  Pure-Python helpers in `_ospath.py` carry that invariant; the C
  entries already do (they pass the string straight through to
  `dos.library`).
- **AmigaDOS protection-bit inversion.** Document loudly in
  `_ospath.py` / `os.py` / `docs/amiga-testing.md`. Set bit ⇒
  denial for RWED; set bit ⇒ assertion for APSH.
- **No CPython-compat translation layer for `chmod`.** Posix
  rwx → AmigaDOS rwed is ambiguous and lossy; we take the
  AmigaDOS mask directly. CPython interop scripts that need
  cross-platform `chmod` should use `if sys.platform == 'amiga'`.
- **Extensible-module attribute lookup.** When the frozen `os.py`
  runs, the C-side globals are already populated, so functions
  defined in the frozen file shadow nothing — they extend. If a
  future C-side change adds e.g. a `mkdir` with a new signature,
  the frozen file's `makedirs` continues to call it through
  the merged namespace (no name change needed).

---

## Out-of-scope items reaffirmed

- Posix mode translation for `chmod` — ambiguous, see above.
- `os.path.expanduser` / `expandvars` — AmigaOS uses
  `dos.library GetVar` and there's no `~` concept.
- `os.walk` follow-symlinks / on-error callback — keep minimal
  shape; users wrap if needed.
- Full CPython `os.path` parity (`commonpath`, `relpath`,
  `samefile`, `getmtime`, etc.) — add later if a real call site
  needs them.
- `os.environ` mapping interface — Phase 20's `getenv` / `putenv`
  / `unsetenv` are enough for now.
