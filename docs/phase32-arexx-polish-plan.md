# Phase 32 — ARexx polish step plan

Companion to the Phase 32 design block in
[docs/amiga.md](amiga.md#phase-32--arexx-polish-planned). That
section answers *what* and *why*; this file is the *step-by-step
ship plan* — how to chunk the work into landable PRs.

Phase 32 adds three things on top of the existing ARexx surface
(`amiga.rexx_send`, `amiga.rexx_open` / `recv` / `reply` from
Phase 18):

```python
import amiga

# (1) Cheap existence check.
if amiga.rexx_exists("IBROWSE"):
    rc, html = amiga.rexx_send("IBROWSE", "QUERY ITEM=URL")

# (2) Public-port enumeration.
for port in amiga.rexx_list():
    print(port)

# (3) Persistent client with a cached reply MsgPort.
with amiga.RexxClient("DOPUS.1") as ib:
    ib.send("LISTER NEW")
    rc, path = ib.send("LISTER QUERY 1 PATH", check=False)
```

The first two are tiny wrappers around `FindPort` / `SysBase->PortList`
walking. The third amortises the `CreateMsgPort` / `DeleteMsgPort`
cost across many sends — invisible for a single call, real when
driving a host (`DOpus`, `IBrowse`, `YAM`, ...) in a tight loop.

## Phasing overview

```
Step 1: rexx_exists + rexx_list (one-shot helpers)
                                          ↓
                              Step 2: RexxClient (persistent client)
                                          ↓
                                  Step 3: docs + smoke
```

| # | Step | Output | On-target smoke |
|---|------|--------|-----------------|
| **1** | `amiga.rexx_exists(name)` → `bool` (FindPort with Forbid/Permit). `amiga.rexx_list()` → `list[str]` (walks `SysBase->PortList`). | Two new entries in `amiga_module_globals_table`. | From the REPL: `amiga.rexx_exists("MICROPYTHON.1")` after `amiga.rexx_open()` returns True; `amiga.rexx_list()` returns a list containing our own port. |
| **2** | `amiga.rexx_client_open()` / `_close(h)` / `_send(h, host, cmd)` C primitives (factor out the body of `amiga_rexx_send_fn` so the client reuses it). Frozen `RexxClient` class in `amiga.py` wraps them with `__enter__` / `__exit__` / `__del__` / `.send(check=True/False)`. | Three new C entries; new Python class; cleanup chain wired into `amiga_rexx_shutdown` so a forgotten close doesn't leak the MsgPort on exit. | From the REPL: open `RexxClient("MICROPYTHON.1")` (which talks to our own inbound port via Phase 18), `.send("noop")` returns `(rc, result)`. With-block exits cleanly. |
| **3** | Docs flip, manifest verification, arg-shape tests. | `docs/amiga.md` Phase 32 → ✅, `docs/amiga-testing.md` gains an ARexx-polish subsection. | vamos-runnable arg-shape / module-import test; on-target reality check noted as interactive. |

Each step is small: Step 1 is ~40 LOC C, Step 2 is ~120 LOC C +
~40 LOC Python, Step 3 is paperwork.

---

## Step 1 — `rexx_exists` + `rexx_list`

### Deliverables

- Two new C entries in `modamiga.c`, both gated on the existing
  `MICROPY_PY_AMIGA` block:

  ```c
  static mp_obj_t amiga_rexx_exists(mp_obj_t name_obj) {
      const char *name = mp_obj_str_get_str(name_obj);
      Forbid();
      bool ok = (FindPort((CONST_STRPTR)name) != NULL);
      Permit();
      return mp_obj_new_bool(ok);
  }
  static MP_DEFINE_CONST_FUN_OBJ_1(amiga_rexx_exists_obj, amiga_rexx_exists);

  static mp_obj_t amiga_rexx_list(void) {
      mp_obj_t list = mp_obj_new_list(0, NULL);
      Forbid();
      struct List *plist = &SysBase->PortList;
      struct Node *n;
      for (n = plist->lh_Head; n->ln_Succ != NULL; n = n->ln_Succ) {
          if (n->ln_Name != NULL) {
              mp_obj_list_append(list,
                  mp_obj_new_str(n->ln_Name, strlen(n->ln_Name)));
          }
      }
      Permit();
      return list;
  }
  static MP_DEFINE_CONST_FUN_OBJ_0(amiga_rexx_list_obj, amiga_rexx_list);
  ```

- Registered in `amiga_module_globals_table` alongside the other
  `rexx_*` entries:
  ```c
  { MP_ROM_QSTR(MP_QSTR_rexx_exists), MP_ROM_PTR(&amiga_rexx_exists_obj) },
  { MP_ROM_QSTR(MP_QSTR_rexx_list),   MP_ROM_PTR(&amiga_rexx_list_obj) },
  ```

### Concurrency / safety

- `Forbid()` / `Permit()` braces so the port list can't mutate
  mid-walk. `Forbid` is light enough that wrapping the whole list
  walk is fine; we're allocating Python objects inside the
  Forbid window, which is borderline but the readline / vstr GC
  paths used by `mp_obj_new_str` don't issue any wait/Forbid-
  unsafe calls. (Phase 18's existing `amiga_rexx_open_fn` already
  does Python allocs inside a Forbid block, so this matches the
  precedent.)
- Empty list (no public ports — degenerate case, doesn't happen on
  a real boot) returns `[]` rather than raising.

### Verification

REPL on target (Amiberry):

```python
>>> import amiga
>>> amiga.rexx_exists("WORKBENCH")
True
>>> amiga.rexx_exists("DOES_NOT_EXIST")
False
>>> sorted(amiga.rexx_list())
['MICROPYTHON.1', 'WORKBENCH', ...]
```

Under vamos: `WORKBENCH` doesn't exist (no Intuition); `rexx_list()`
returns whatever stubs vamos publishes. The vamos smoke just
checks the entries are callable and return the right shapes.

---

## Step 2 — `RexxClient` persistent client

### Deliverables

#### C-side primitives in `modamiga.c`

Refactor the one-shot send path (`amiga_rexx_send_fn`) so the
core "wrap command in argstring + PutMsg + Wait" loop becomes a
helper that takes an externally-owned reply port:

```c
static mp_obj_t amiga_rexx_send_via_port(struct MsgPort *reply,
                                         const char *host,
                                         mp_buffer_info_t *cmd_bi);
```

`amiga_rexx_send_fn` becomes a thin wrapper that `CreateMsgPort`'s,
calls the helper, then `DeleteMsgPort`'s.

Three new entry points:

| Function | Signature | Behaviour |
|---|---|---|
| `amiga.rexx_client_open()` | `() -> int` | `CreateMsgPort()`; registers in the at-exit chain; returns the address as an `int` handle. |
| `amiga.rexx_client_close(h)` | `(int) -> None` | Unregisters from the chain, `DeleteMsgPort(h)`. Tolerates `h == 0`. |
| `amiga.rexx_client_send(h, host, cmd)` | `(int, str, bytes) -> (int, bytes \| None)` | Sends `cmd` to `host` using the cached reply port at `h`. Same return shape as `amiga.rexx_send`. |

The at-exit chain ensures a forgotten close doesn't leak the
`MsgPort` on process exit; same pattern as the existing
`amiga_rexx_shutdown()`.

#### Python `RexxClient` class in `amiga.py`

```python
class RexxClient:
    """Persistent ARexx client.

    Holds an open reply MsgPort across many sends. Use when
    driving a host (DOpus, IBrowse, ...) in a tight loop. For a
    single send, `amiga.rexx_send()` or `amiga.rexx()` is enough.
    """

    __slots__ = ("_host", "_handle")

    def __init__(self, host):
        self._host = host
        self._handle = _c.rexx_client_open()

    def send(self, command, check=True):
        if not self._handle:
            raise ValueError("RexxClient: closed")
        if isinstance(command, str):
            command = command.encode("latin-1")
        rc, result = _c.rexx_client_send(self._handle, self._host, command)
        if check and rc != 0:
            raise OSError(rc, "rexx %r at %s rc=%d" % (command, self._host, rc))
        if check:
            return result if result is not None else b""
        return (rc, result)

    @property
    def host(self):
        return self._host

    def close(self):
        if self._handle:
            _c.rexx_client_close(self._handle)
            self._handle = 0

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc_val, exc_tb):
        self.close()
        return False

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def __repr__(self):
        state = "closed" if not self._handle else "open"
        return "<RexxClient host=%r %s>" % (self._host, state)
```

### Verification

REPL on target:

```python
>>> from amiga import RexxClient, rexx_open, rexx_port_name
>>> rexx_open()  # so we have a port to talk to
'MICROPYTHON.1'
>>> with RexxClient("MICROPYTHON.1") as c:
...     # ... server-side serve loop would handle this
...     pass
>>> # tear-down on __exit__ is the test
```

For a real-world smoke, drive a host that's actually up — DOpus
or YAM — and confirm a tight loop of 100 sends is noticeably
faster than 100 calls through `amiga.rexx_send`.

---

## Step 3 — Docs + tests

### Deliverables

- `docs/amiga.md` Phase 32 → ✅; section gains a "Status — done"
  block listing the three new entries and noting the cleanup-chain
  registration.
- `docs/amiga-testing.md` gains an ARexx-polish subsection under
  Amiberry, with the `RexxClient` REPL example.
- `tests/ports/amiga/test_rexx_polish.py` — vamos-runnable:
  - `amiga.rexx_exists("nonsense_no_such_port")` returns `False`
  - `amiga.rexx_list()` returns a `list[str]`
  - `RexxClient` instantiates, has `.send` / `.close` / `__enter__`
  - `RexxClient.send` on a closed instance raises `ValueError`

### Variant gating

All three shipped variants include the additions —
`rexxsyslib.library` is part of any Workbench install and there's
no external SDK. Per-variant text-segment growth: ~600 bytes.

---

## Cross-cutting concerns

- **No Forbid leaks.** Both `rexx_exists` and `rexx_list` open
  Forbid blocks; the bodies match the precedent in
  `amiga_rexx_open_fn`. `RexxClient.send` does not Forbid —
  it just `PutMsg` + `Wait` like `amiga_rexx_send_fn`.
- **At-exit cleanup chain.** Open `RexxClient` instances register
  in a linked list rooted in `modamiga.c`. `amiga_rexx_shutdown`
  (already called by `main.c`) walks the chain and closes each
  port. So a script that opens a client, faults, and never
  hits `__del__` still has its `MsgPort` reclaimed on exit.
- **`check=True` default.** Matches the existing `amiga.rexx`
  helper — easier to compose with normal Python control flow than
  the lower-level `amiga.rexx_send` tuple shape.
- **`Ctrl+C` during `.send` is deferred** — same rationale as
  Phase 18's `amiga_rexx_send_fn`: abandoning the wait mid-flight
  would have the host `PutMsg` into a freed reply port. The
  client's reply port is `Wait`-ed on synchronously and Ctrl+C
  is latched, raised after the reply lands.

---

## Out-of-scope items reaffirmed

- Migrating the existing `amiga.rexx_*` flat surface into an
  `amiga.rexx.*` sub-module — breaking change for the inbound
  server side (Phase 18); not worth it
- Async send (fire-and-forget without waiting for reply) — easy
  add later but not in scope for Phase 32
- Reshaping `amiga.rexx_send` / `amiga.rexx` to use the client
  class internally — the per-call MsgPort cost is invisible for
  single sends; the polish is for loops
- Bidirectional Port objects (a single object that's both a
  server and a client) — separate phase if needed
