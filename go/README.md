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

    // Arrays + STRING (v0.6.0):
    arr, _ := db.ReadDINTArray("Test_DINT_Arr", 10)              // []int32
    db.WriteBOOLArray("Test_Bool_Arr", []bool{true, false, true})
    name, _ := db.ReadString("Test_STRING")                       // string
    _ = arr; _ = name
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
| `tagtest`      | Canonical DINT Read/Write/Readback round-trip |
| `typetest`     | Cross-type sweep (every scalar + STRING + arrays + BOOL[] + 2-D/3-D) |
| `msgprobe`     | Raw `OCXcip_MessageSend` + response hexdump |
| `identity`     | Local + remote Identity dump |
| `connidentity` | Class-3 connected Identity (LFO + bare Identity + FC) |
| `conntest`     | Class-3 round-trip validator (N Identities) + `--bench` UCMM-vs-Class3 latency |
| `pooltest`     | v0.8.0 pool + keepalive validator (M workers × N requests through a pre-opened pool) |
| `routedident`  | v0.8.0 multi-hop Identity via Unconnected_Send (svc 0x52) + route_path |
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
│   ├── conn.go                TxRxOpen/Msg/Close (v0.7.0+ LFO via MessageSend)
│   ├── pool.go                PoolOpen/TxRx/Batch/Close (v0.8.0+)
│   ├── errors.go              BPErr* constants + sentinels + CIPError (v0.8.0+)
│   ├── shm/                   IPC layer (cgo for POSIX sems)
│   │   ├── consts.go
│   │   ├── shm.go             Open/Close, slot reserve/release
│   │   ├── call.go            Call dispatcher (mirrors bp_client_call)
│   │   ├── posix_sem.go       cgo wrapper for sem_open/post/wait/...
│   │   └── posix_sem_stub.go  non-cgo stub for vet/build on dev hosts
│   └── cip/                   per-opcode wire encoders/decoders
│       └── route.go           BuildUnconnectedSend (v0.8.0+ multi-hop)
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

v0.8.0.  Outbound tag I/O fully functional including arrays + BOOL[]
+ STRING (v0.6.0), class-3 connected messaging (v0.7.0), and the
v0.8.0 quality-of-life additions:

- Structured CIP-layer errors (`*CIPError` via `errors.As`).
- `PoolOpen` / `PoolTxRx` / `PoolBatch` / `PoolClose` — per-slot
  pool with keepalive goroutine.
- `cip.BuildUnconnectedSend` for multi-hop routes via svc 0x52.

Inherited limitation: small-buffer transport (~500 B envelope
inherited from `MessageSend`).  The 4002-byte chip-mailbox-0x204
path is shelved per [`docs/v0.8-large-buffer-re.md`](../docs/v0.8-large-buffer-re.md)
(operational blocker on rootless containers, not a missing-symbol
issue).
