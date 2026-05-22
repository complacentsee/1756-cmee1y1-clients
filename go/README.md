# ocxbp (Go) — outbound CIP for the 1756-CMEE1Y1

Go SDK for outbound CIP tag I/O against a 1756 ControlLogix PLC,
dispatched through the ASEM EEC card's `bpServer` userland IPC.

API mirrors the [C SDK](../c/) (libbpclient) shape-for-shape; uses
idiomatic Go (return-value + `error` instead of out-param + rc).

> One of three implementations of the same wire protocol — the
> [C SDK](../c/) and [Python SDK](../python/) live alongside this
> one. All three pass `tagtest` + `msgprobe` byte-identically; see
> [`runprobe.py`](../runprobe.py) for the shared cross-language
> runner.

## Quick example

```go
package main

import (
    "fmt"
    "log"

    "github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
    c, err := ocxbp.Open()
    if err != nil { log.Fatal(err) }
    defer c.Close()

    if _, err := c.OpenSession(); err != nil { log.Fatal(err) }

    db, err := c.OpenTagDB("P:1,S:2")  // ControlLogix in backplane slot 2
    if err != nil { log.Fatal(err) }
    defer db.Close()

    n, _ := db.Build()
    fmt.Printf("%d symbols on the PLC\n", n)

    v, _ := db.ReadDINT("OCX_TEST")
    db.WriteDINT("OCX_TEST", int32(uint32(0xDEADBEEF)))
    db.WriteDINT("OCX_TEST", v) // restore
}
```

## Container plumbing

This SDK requires the bpServer's POSIX shared memory + named
semaphores. The container must be launched with:

```
--ipc=host --pid=host -v /dev/shm:/dev/shm
```

See [`docs/container-plumbing.md`](../docs/container-plumbing.md)
for details.

## Build + run on the HMI

```
py go/build_image.py
py runprobe.py --image bpclient-go-tagtest:dev tagtest
```

The image is a two-stage build: `golang:1.22-bookworm` for compilation
(cgo needs gcc + glibc) and `gcr.io/distroless/cc-debian12` for
runtime (carries glibc + dynamic loader for the cgo POSIX-sem wrapper).

## Diagnostic CLIs (all under `/usr/local/bin/` in the container)

| Tool           | Mirror of |
|---|---|
| `tagtest`      | Canonical Read/Write/Readback round-trip |
| `msgprobe`     | Raw `OCXcip_MessageSend` + response hexdump |
| `identity`     | Local + remote Identity dump |
| `connidentity` | Class-3 connected Identity (NOT FUNCTIONAL on cm1756) |
| `pathprobe`    | `OCXcip_ParsePath` dispatch dump |
| `actnodes`     | Active-node bitmap |
| `modutil`      | Local switch / display / LED utilities |

Override at run time with `--entrypoint /usr/local/bin/<tool>`,
or pass `<tool>` as the first arg to `runprobe.py`.

## Layout

```
go/
├── go.mod / go.sum            module .../go
├── ocxbp/                     public API package
│   ├── client.go              Client (Open, OpenSession, SHM accessor)
│   ├── tagdb.go               TagDB
│   ├── access.go              scalar read/write helpers
│   ├── identity.go            GetIDLocal / GetDeviceID / GetActiveNodes
│   ├── module.go              switch / LED / display
│   ├── message.go             MessageSend (UCMM)
│   ├── conn.go                TxRx (NOT FUNCTIONAL — see Phase 5 notes)
│   ├── errors.go              BPErr* constants + sentinels + ErrCode helper
│   ├── shm/                   IPC layer (cgo for POSIX sems)
│   │   ├── consts.go
│   │   ├── shm.go             Open/Close, slot reserve/release
│   │   ├── call.go            Call dispatcher (mirrors bp_client_call)
│   │   ├── posix_sem.go       cgo wrapper for sem_open/post/wait/...
│   │   └── posix_sem_stub.go  non-cgo stub for vet/build on dev hosts
│   └── cip/                   per-opcode wire encoders/decoders
└── cmd/                       diagnostic CLI binaries (one main.go per tool)
```

The `ocxbp/shm/posix_sem.go` cgo wrapper is the SDK's only cgo
file — `golang.org/x/sys/unix` doesn't expose POSIX named-sem
support, and replicating glibc's sem_t internals via raw futex
syscalls would be brittle across glibc versions. See the file
header for the agreed-upon scope.

## Thread safety

`ocxbp.Client` is safe for concurrent use: each `Call` reserves
its own slot across all 16 slots, gated by the cross-process
`/bpShm` semaphore and a process-local `sync.Mutex`.

## Status

v0.5.0. Outbound tag I/O is fully functional. The class-3
connected `TxRx*` methods are kept for API parity but return
engine code `0x1001` on cm1756 — see
[`docs/protocol.md`](../docs/protocol.md) "Connected messaging
— open issues".
