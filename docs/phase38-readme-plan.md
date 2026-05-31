# Phase 38 — `ports/amiga/README.md` step plan

Companion to the Phase 38 design block in
[docs/amiga.md](amiga.md#phase-38--portsamigareadmemd-planned).
That section answers *what* and *why*; this file is the
*step-by-step ship plan* — how to chunk the work into landable PRs.

The goal is an upstream-quality `ports/amiga/README.md`: same
shape as the other port READMEs (`rp2`, `stm32`, `esp32`, `unix`),
suitable for a first-time visitor to land in `ports/amiga/` and
understand what they're looking at. The in-port phase log,
testing runbook, and architecture notes stay where they already
are (`docs/amiga.md`, `docs/amiga-testing.md`).

## Phasing overview

```
Step 1: Draft README + cross-link from docs/amiga.md
                        ↓
                Step 2: Polish + close
```

| # | Step | Output |
|---|------|--------|
| **1** | Initial `ports/amiga/README.md` draft covering heading, intro, supported-features list, build instructions, deployment, accessing the REPL, and a brief testing pointer. Cross-link from the top of `docs/amiga.md` so the in-port doc points new readers at the README first. | New README file + one-line link addition. |
| **2** | Polish pass: verify the supported-features list against the current phase status table; check every link resolves; align the section headings with the most common other-port shape (`Building`, `Running`, `Testing`); flip `docs/amiga.md` Phase 38 → ✅. | Final README + status flip. |

---

## Step 1 — Draft

### Deliverables

A `ports/amiga/README.md` with the following sections, in order:

1. **`MicroPython port to AmigaOS 3.x`** (heading, `====` underline)
2. **Intro paragraph(s)** — what the port is, AmigaOS 3.x / 3.1+
   minimum, 68020+ CPU target, what binary it produces, how it
   launches (Shell or Workbench).
3. **Supported features** — bullet list. Group roughly:
   - Core language: dynamic heap, 68k native code emitter, frozen
     modules, persistent REPL history, line editing, Ctrl+C
     handling.
   - File system: Pythonic VFS, AmigaDOS path semantics.
   - AmigaOS APIs: `amiga.library` (proxy + `.fd` trampoline),
     `amiga.intuition`, `amiga.asl`, `amiga.icon`, `amiga.catalog`,
     ARexx (in / out / `RexxClient`), env-var integration, volume /
     assign introspection, AmigaDOS pattern matching, `timer.device`
     -backed `time`.
   - Networking: `socket` via `bsdsocket.library`.
   - TLS: AmiSSL v5 (variant-gated).
   - `urequests` frozen HTTP/HTTPS client.
   - `platform.amiga_info()` and `platform.*` identity.
   - `os` / `os.path`: standard surface plus `chmod` / `getprotect`
     / `FIBF_*` / `makedirs` / `walk` / AmigaOS-aware path helpers.
4. **Building** — bebbo gcc 6.5 toolchain pointer (link to bebbo's
   GitHub), `make -C mpy-cross` prerequisite, `cd ports/amiga &&
   make`, the three variants and how to select via `VARIANT=`.
   Mention `tools/amiga-build.sh` as the CI-mirror Docker route
   for portability.
5. **Deploying** — copy the binary to an Amiga drive; protect the
   bits; on Amiberry, drop into a hard-file mount. One paragraph
   each.
6. **Accessing the REPL** — invoke from Shell (`micropython` or
   `micropython script.py`); brief Workbench-launch note
   (double-click; output via tooltype `SCRIPT=`).
7. **Testing** — vamos (host, headless) and Amiberry (full
   emulation) one-paragraph each, pointing to
   `docs/amiga-testing.md` for the full runbook.
8. **Further reading** — short link list:
   `docs/amiga.md` (design and phase status),
   `docs/amiga-testing.md` (testing runbook).

### Implementation notes

- Match the style of `ports/rp2/README.md` and
  `ports/stm32/README.md`: setext-style `====` underline for the
  H1, `----` for H2, plain markdown otherwise. Code blocks
  use ```` ``` ```` fences; shell prompts as `$`.
- Don't reference Phase numbers in the README -- those are
  internal to `docs/amiga.md`. The README describes the port as
  it exists *now*, not the path that got us there.
- Don't repeat what `docs/amiga-testing.md` already covers;
  point to it instead.
- A "Limitations" subsection is welcome but should be terse and
  factual (e.g. "No `@micropython.native` try/except; no
  `@micropython.viper` multi-register locals — both bounded by
  the 68k emitter rework"). Detail belongs in `docs/amiga.md`.

### Verification

Open the file in a Markdown renderer; check every link resolves;
read it top-to-bottom from the perspective of someone who has
never seen the port before and ask "could I build this in 30
minutes?"

---

## Step 2 — Polish + close

### Deliverables

- Re-check the supported-features list against the live phase
  status table in `docs/amiga.md` (anything still planned should
  stay off the README's "what it has today" list).
- Cross-link from `docs/amiga.md`'s Overview to `ports/amiga/README.md`
  so the in-port doc points new readers at the README first.
- `docs/amiga.md` Phase 38 status → ✅.

### Verification

- `grep -rn "ports/amiga/README.md"` across `docs/` and
  `ports/amiga/` confirms cross-references are in place.
- Open both `docs/amiga.md` and the new README; the README is
  the entry point, `docs/amiga.md` is the depth.

---

## Cross-cutting concerns

- **No emoji.** Other-port READMEs are plain markdown; we match.
- **AmigaShell prompts.** When showing a sample, use `1>` as the
  prompt to mirror AmigaDOS conventions (not `$`).
- **No fork-specific URLs.** The README should read as if it's
  already in upstream -- no `sidick/micropython` references, no
  `docs/phase*-plan.md` links (those are work-in-flight planning
  docs, not user-facing). Reference `docs/amiga.md` and
  `docs/amiga-testing.md` only.
- **Length budget.** ~150–200 lines. If a section grows past 30
  lines, consider whether it belongs in `docs/amiga.md` instead.

---

## Out-of-scope items reaffirmed

- Examples directory (`examples/amiga/`) — separate work if a
  caller asks. Other ports don't always have one.
- A "What's new" / changelog — git log is authoritative.
- Phase plan summaries — `docs/amiga.md` already does this.
- Architectural deep-dives (trampoline ABI, FD parser, native
  emitter codegen) — too much for a README.
