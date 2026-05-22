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

| Language | Status | Source | Container |
|---|---|---|---|
| C        | shipped (v0.5.0) | [`c/`](c/) | `bpclient-c-tagtest:dev` |
| Go       | shipped (v0.5.0) | [`go/`](go/) | `bpclient-go-tagtest:dev` |
| Python   | shipped (v0.5.0) | [`python/`](python/) | `bpclient-python-tagtest:dev` |

All three pass the canonical `tagtest` (read / write / readback against
`OCX_TEST` on an L85) and the `msgprobe` slot-sweep (Identity
Get_Attributes_All across slots 0..3 + the empty-slot rc=3 refusal)
**byte-identically** — see [`runprobe.py`](runprobe.py) for the
shared cross-language runner.

The class-3 connected `TxRx*` methods are kept in all three SDKs for
API parity but return engine code `0x1001` on cm1756 — the
`OCXCN_OpenClass3Connection` library is missing from the chip
image. Workaround documented in
[`docs/protocol.md`](docs/protocol.md) "Connected messaging — open
issues".

## What this gives you

```c
// C
bp_client_t *c;     bp_client_open(&c);
bp_client_open_session(c, NULL);
bp_tagdb_t  *db;    bp_tagdb_open(c, "P:1,S:2", &db);
bp_tagdb_build(db, NULL);
int32_t value;
bp_tagdb_read_dint(db, "OCX_TEST", &value);
bp_tagdb_write_dint(db, "OCX_TEST", 0xDEADBEEF);
```

```go
// Go (module github.com/complacentsee/1756-cmee1y1-clients/go)
import "github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"

c, _ := ocxbp.Open();           defer c.Close()
_, _ = c.OpenSession()
db, _ := c.OpenTagDB("P:1,S:2"); defer db.Close()
db.Build()
v, _ := db.ReadDINT("OCX_TEST")
db.WriteDINT("OCX_TEST", int32(uint32(0xDEADBEEF)))
```

```python
# Python (pip install bpclient)
import bpclient

with bpclient.Client() as c:
    c.open_session()
    db = c.open_tagdb("P:1,S:2")
    try:
        db.build()
        v = db.read_dint("OCX_TEST")
        db.write_dint("OCX_TEST", -559038737)  # int32 of 0xDEADBEEF
    finally:
        db.close()
```

All three speak the same wire protocol with the same semantics and
the same container plumbing. Use [`runprobe.py`](runprobe.py) to
diff the output of `tagtest` / `msgprobe` across languages and
confirm byte equivalence.

## Quick start (build + run on the HMI)

Each language's `build_image.py` POSTs a tarball to the HMI's
Docker daemon and tags the resulting image:

```sh
py c/build_image.py        # → bpclient-c-tagtest:dev
py go/build_image.py       # → bpclient-go-tagtest:dev
py python/build_image.py   # → bpclient-python-tagtest:dev
```

Then dispatch the `tagtest` (or any diagnostic) via the shared
runner — `runprobe.py` handles the `--ipc=host --pid=host
-v /dev/shm:/dev/shm` plumbing automatically:

```sh
py runprobe.py --image bpclient-c-tagtest:dev      tagtest
py runprobe.py --image bpclient-go-tagtest:dev     tagtest
py runprobe.py --image bpclient-python-tagtest:dev tagtest

py runprobe.py --image bpclient-go-tagtest:dev msgprobe --slot 1 --req "01 02 20 01 24 01"
```

The `--ipc=host --pid=host -v /dev/shm:/dev/shm` plumbing is
**mandatory** — see [`docs/container-plumbing.md`](docs/container-plumbing.md).

## Diagnostic CLI inventory

Each container ships the same set of binaries at `/usr/local/bin/`.
Override the default `tagtest` entrypoint via Docker's
`--entrypoint`, or pass the binary name as the first arg to
`runprobe.py`:

| Tool | Purpose |
|---|---|
| `tagtest`      | Canonical read / write / readback round-trip |
| `msgprobe`     | Raw `OCXcip_MessageSend` invocation + response hexdump |
| `identity`     | Local + remote Identity dump |
| `connidentity` | Class-3 connected Identity (NOT FUNCTIONAL on cm1756) |
| `pathprobe`    | `OCXcip_ParsePath` dispatch dump |
| `actnodes`     | Active-node bitmap |
| `modutil`      | Local switch / display / LED utilities |

## What this does NOT do

- **Inbound CIP intercept** — not architecturally possible on the
  cm1756 (the APex chip firmware handles all inbound CIP
  autonomously after boot). See the upstream reverse-engineering
  notes at [complacentsee/rockwell-bpgateway-re](https://github.com/complacentsee/rockwell-bpgateway-re/)
  (`docs/phase6d-inbound-truly-blocked.md`,
  `docs/phase6e-firmware-has-full-cip-stack.md`) for the proof.
- **Device management / firmware upgrade** — handled by the
  vendor's SystemManager service, not by this SDK.
- **Class-3 connected messaging** — the `OCXCN_*` library is
  missing from the cm1756 image. The TxRx wrappers exist for
  parity but return `0x1001`; use UCMM messages with a manual
  Large Forward Open (CIP service `0x5B`) for connected behavior
  on a per-PLC basis.

## Repository layout

```
.
├── docs/                    protocol spec + container plumbing + error codes + type map + UDT
├── c/                       libbpclient (.so/.a) + tagtest + diagnostic CLIs + Dockerfile
├── go/                      Go SDK (module .../go/ocxbp) + cmd binaries + Dockerfile
├── python/                  pure-Python SDK (bpclient) + examples + pytest + Dockerfile
├── runprobe.py              cross-language container runner (--image picks C/Go/Python)
└── CLAUDE.md                project conventions (incl. docs-with-code rule)
```

## Reverse-engineering record

This SDK is built from the wire protocol documented at
[github.com/complacentsee/rockwell-bpgateway-re](https://github.com/complacentsee/rockwell-bpgateway-re/).
That repo is the empirical record — every layout decision in
[`docs/protocol.md`](docs/protocol.md) here traces back to a
specific phase doc there.

## License

MIT — see [LICENSE](LICENSE).
