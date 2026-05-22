# 1756-cmee1y1-clients

SDKs in **C**, **Go**, and **Python** for outbound tag I/O against the
Rockwell **1756-CMEE1Y1** Embedded Edge Compute card, communicating
with the stock `bpServer` via its POSIX shared-memory IPC.

> The 1756-CMEE1Y1 is the Embedded Edge Compute card variant
> embedded in ASEM cm1756-series panel HMIs (firmware artifacts use
> the shortname `cmee1`; full Rockwell catalog includes the `Y1`
> product-variant suffix). The wire protocol is identical across the
> 1756-EEC product family and the underlying Online Development APex2
> chip family, so this SDK should work against other variants too —
> only the validation harness is pinned to this specific catalog.

## Status

| Language | Status | Notes |
|---|---|---|
| C        | 🟡 in progress | initial impl + tagtest + Dockerfile |
| Go       | 🔲 planned | port of [complacentsee/rockwell-bpgateway-re/go/ocxbp](https://github.com/complacentsee/rockwell-bpgateway-re/tree/main/go) |
| Python   | 🔲 planned | pure Python via `posix_ipc` + `mmap` |

Track progress + design decisions in [docs/](docs/).

## What this gives you

```c
// C
bp_client_t *c;     bp_client_open(&c);
bp_tagdb_t  *db;    bp_tagdb_open(c, "P:1,S:2", &db);
bp_tagdb_build(db, NULL);
int32_t value;
bp_tagdb_read_dint(db, "OCX_TEST", &value);
bp_tagdb_write_dint(db, "OCX_TEST", 0xDEADBEEF);
```

```go
// Go
c, _ := bpclient.Open();      defer c.Close()
db, _ := c.OpenTagDB("P:1,S:2"); defer db.Close()
db.Build()
v, _ := db.ReadDINT("OCX_TEST")
db.WriteDINT("OCX_TEST", 0xDEADBEEF)
```

```python
# Python
with bpclient.open() as c:
    with c.open_tagdb("P:1,S:2") as db:
        db.build()
        v = db.read_dint("OCX_TEST")
        db.write_dint("OCX_TEST", 0xDEADBEEF)
```

All three are **interchangeable** — same wire protocol, same
semantics. Same container plumbing. Test against the canonical
[`tagtest`](validation/) program to verify.

## Quick start (container, Python)

```yaml
# docker-compose.yml
services:
  bpclient:
    image: ghcr.io/complacentsee/1756-cmee1y1-clients-python:latest
    ipc: host
    pid: host
    volumes:
      - /dev/shm:/dev/shm
    platform: linux/arm64
    command: python -m bpclient.examples.tagtest --path "P:1,S:2"
```

The `--ipc=host --pid=host -v /dev/shm:/dev/shm` plumbing is
**mandatory** — see [`docs/container-plumbing.md`](docs/container-plumbing.md).

## What this does NOT do

- **Inbound CIP intercept** — not architecturally possible on the
  cm1756 (the APex chip firmware handles all inbound CIP
  autonomously after boot). See the upstream reverse-engineering
  notes at [complacentsee/rockwell-bpgateway-re](https://github.com/complacentsee/rockwell-bpgateway-re/)
  (`docs/phase6d-inbound-truly-blocked.md`,
  `docs/phase6e-firmware-has-full-cip-stack.md`) for the proof.
- **Device management / firmware upgrade** — handled by the
  vendor's SystemManager service, not by this SDK.
- **Class-3 connected messaging** — possible Phase 7 addition;
  not in v0.1.

## Repository layout

```
.
├── docs/                    protocol spec + container plumbing + error codes + type map
├── c/                       libbpclient (.so/.a) + tagtest + Dockerfile
├── go/                      Go SDK + tagtest + Dockerfile
├── python/                  pure-Python SDK + tagtest + Dockerfile
├── validation/              cross-language validator (runs all three, diffs output)
└── .github/workflows/       CI (build/lint only — HMI validation runs locally)
```

## Reverse-engineering record

This SDK is built from the wire protocol documented at
[github.com/complacentsee/rockwell-bpgateway-re](https://github.com/complacentsee/rockwell-bpgateway-re/).
That repo is the empirical record — every layout decision in
[`docs/protocol.md`](docs/protocol.md) here traces back to a
specific phase doc there.

## License

MIT — see [LICENSE](LICENSE).
