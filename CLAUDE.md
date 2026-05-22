# Project conventions for AI agents (Claude Code / Codex / etc.)

This file is read at session start for any agent operating in this
repository. Keep it short; durable rules live here.

## Docs are part of every code change

When you ship code that adds a language port, a new opcode, a CLI
binary, a public API method, or a wire-protocol detail, the same
turn must sweep the affected docs. Specifically:

- **Top [`README.md`](README.md)** — status table, quickstart per
  language, diagnostic CLI inventory, repository layout.
- **[`docs/protocol.md`](docs/protocol.md)** — the single source
  of truth for the wire bytes; update this FIRST when changing any
  offset / opcode / payload, then implement against the new spec.
  The `## Implementations` table should list every language SDK
  with paths to its dispatcher + encoders.
- **[`docs/container-plumbing.md`](docs/container-plumbing.md)** —
  compose snippets for every language must stay in sync with
  whatever each `Dockerfile` actually installs (CLI names at
  `/usr/local/bin/*`).
- **[`docs/error-codes.md`](docs/error-codes.md)** — the per-
  language error-mechanism rows must match the actual surface
  (Go: sentinels + `EngineError`, Python: `BpError` hierarchy +
  `BpEngine`, C: `int` rc + `bp_strerror`).
- **Per-language READMEs** (`c/README.md`, `go/README.md`,
  `python/README.md`) — quickstart, CLI inventory, version /
  status note.
- **Release commits** (anything touching `c/CMakeLists.txt`
  `VERSION`, `python/pyproject.toml` `version`, or git tags)
  ALWAYS pair the version bump with a doc sweep in the same
  commit series, before the tag lands.

Catching doc drift after the fact is more expensive than keeping
docs in sync inline; treat docs as part of the deliverable, not
follow-up work.

## Wire protocol: doc is canonical

`docs/protocol.md` IS the spec. If you find an inconsistency
between this doc and an implementation, **the doc is wrong**
until corrected here — fix the doc first, commit, then propagate
to whichever implementation drifts.

Implementations must not introduce protocol details that aren't
documented here first. Validate cross-language correctness with
`py runprobe.py --image bpclient-{c,go,python}-tagtest:dev
tagtest` and `msgprobe` (the slot-sweep matrix at the bottom of
[`docs/protocol.md`](docs/protocol.md) is the canonical battery).

## Per-phase commits

Each phase / opcode area / CLI binary lands as its own commit so
the history reads as a recipe. Commit messages reference the C
commit anchors (`556baa9` Phase 4 MessageSend, `3da423e` Phase 5
TxRx-broken) so the cross-language semantic alignment is
traceable. Don't squash phases together.

## Go SDK: cgo policy

`go/ocxbp/shm/posix_sem.go` is the SDK's only cgo file. POSIX
named semaphores have no pure-Go equivalent in
`golang.org/x/sys/unix`, and replicating the glibc `sem_t`
internals via raw futex syscalls would be brittle. Don't expand
cgo use to other files without re-asking the user.

## What's NOT in scope

- Inbound CIP intercept (chip firmware owns this, not the host).
- Class-3 connected messaging via `OCXcip_TxRx*` — the
  `OCXCN_OpenClass3Connection` library is missing from the cm1756
  image, so the calls return `0x1001`. Workaround: a manual Large
  Forward Open (CIP service `0x5B`) via UCMM. Don't try to "fix"
  the broken path — document and move on.
- Firmware update / SystemManager territory.

## Where validation lives

- `runprobe.py` (repo root) dispatches a CLI binary in any of the
  three language images on the HMI Docker daemon at
  `http://10.0.0.166:2375`. `--image` picks the language.
- Building: `py {c,go,python}/build_image.py` from the repo root.
- The canonical regression is "diff the C, Go, Python `tagtest`
  outputs — modulo `dt=` timings, they must be byte-identical".

## Memory pointers (for Claude-Code agents)

If your session has access to per-user memory, see also:

- `feedback-docs-with-code` — the durable rule mirrored here.
- `feedback-iteration-style` — granular per-phase commits.
- `go-port-cgo-sems` — why posix_sem.go is the cgo exception.
- `user-rockwell-bpgateway` — high-level project context.
