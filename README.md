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
| C        | shipped (v0.7.0) | [`c/`](c/) | `bpclient-c-tagtest:dev` |
| Go       | shipped (v0.7.0) | [`go/`](go/) | `bpclient-go-tagtest:dev` |
| Python   | shipped (v0.7.0) | [`python/`](python/) | `bpclient-python-tagtest:dev` |

All three pass `tagtest`, the `msgprobe` slot-sweep (Identity
Get_Attributes_All across slots 0..3 + the empty-slot rc=3 refusal),
the `typetest` cross-type sweep (scalars × 11 + STRING + DINT[1-D]
+ BOOL[] + DINT[2-D] + DINT[3-D]), and the v0.7.0 `conntest`
class-3 round-trip — all **byte-identically** modulo timings.
See [`runprobe.py`](runprobe.py) for the shared cross-language
runner.

Class-3 connected messaging (`TxRx*` / `txrx_*`) is **functional**
as of v0.7.0: each SDK builds Large Forward Open + Forward_Close
internally and routes through `MessageSend` (chip mailbox 0x200,
UCMM transport).  The OEM `OCXcip_TxRx*` entry points are not used
— they dispatch to `OCXCN_OpenClass3Connection` in a library
missing from the cm1756 image.  Wire format in
[`docs/protocol.md`](docs/protocol.md) "Connected messaging —
wire format".

Known v0.7.0 limitation: small-buffer transport only (~500 B
envelope, same as `MessageSend`).  The 4002-byte connected-data
path via chip mailbox 0x204 is v0.8 territory — see
[`docs/v0.8-large-buffer-re.md`](docs/v0.8-large-buffer-re.md).

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
diff the output of `tagtest` / `typetest` / `msgprobe` / `conntest`
across languages and confirm byte equivalence.

### Class-3 connected messaging (v0.7.0)

A connection lifecycle in each SDK; the body bytes are sent
byte-for-byte over the connection (no sequence-number prepending
— see [protocol notes](docs/protocol.md)):

```c
// C
bp_conn_spec_t spec = { .app_handle = 1,
    .encoded_path = (uint8_t[]){0x01, 2}, .path_size = 2 };
bp_client_txrx_open(c, &spec, NULL, NULL);
uint8_t req[] = {0x01, 0x02, 0x20, 0x01, 0x24, 0x01};  // Identity GAA
uint8_t resp[256]; uint16_t got = 0;
bp_client_txrx_msg(c, &spec, req, sizeof(req), resp, sizeof(resp), &got);
bp_client_txrx_close(c, &spec);
```

```go
// Go
spec := &ocxbp.ConnSpec{AppHandle: 1, EncodedPath: []byte{0x01, 2}, PathSize: 2}
c.TxRxOpen(spec)
resp := make([]byte, 256)
got, _ := c.TxRxMsg(spec, []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01},
                     resp, uint16(len(resp)))
c.TxRxClose(spec)
```

```python
# Python
spec = bpclient.ConnSpec(app_handle=1, encoded_path=b"\x01\x02", path_size=2)
c.txrx_open(spec)
resp = c.txrx_msg(spec, bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01]), 256)
c.txrx_close(spec)
```

### Connection pool + keepalive (v0.8.0)

For callers that do many short conversations against the same PLC,
the per-slot pool keeps N class-3 connections open and round-robins
among them.  An optional keepalive thread pings idle entries with
Identity GAA to dodge the PLC's ~40 s idle timeout.  Mirrors the
sibling apex2d daemon's `slot_pool_keepalive_idle`.

```c
// C
bp_pool_spec_t ps = { .slot = 2, .size = 4, .keepalive_ms = 10000, .conn_params = 0 };
bp_client_pool_open(c, &ps);
uint8_t req[]  = {0x01, 0x02, 0x20, 0x01, 0x24, 0x01};
uint8_t resp[256]; uint16_t got = 0;
bp_client_pool_txrx(c, 2, req, sizeof(req), resp, sizeof(resp), &got);
bp_client_pool_close(c, 2);
```

```go
// Go — call from multiple goroutines for true concurrent sends.
c.PoolOpen(&ocxbp.PoolSpec{Slot: 2, Size: 4, KeepaliveMs: 10000})
resp := make([]byte, 256)
got, _ := c.PoolTxRx(2, []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01},
                      resp, uint16(len(resp)))
c.PoolClose(2)
```

```python
# Python — call from multiple threads for true concurrent sends.
c.pool_open(bpclient.PoolSpec(slot=2, size=4, keepalive_ms=10000))
resp = c.pool_txrx(2, bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01]), 256)
c.pool_close(2)
```

Benchmark from the cm1756 chassis (slot 2 = L85, 4 conns, 200 Identity
round-trips):

| Mode | C | Go | Python |
|---|---|---|---|
| Single-conn `TxRxMsg` (sequential) | ~1000 req/s | ~750 req/s  | ~600 req/s  |
| Pool fanout (8 worker threads)     | ~3900 req/s | ~2300 req/s | ~1500 req/s |
| `pool_batch`  (Phase 4)            | ~4800 req/s | ~2700 req/s | ~1800 req/s |

`pool_batch` submits N requests in a single call and internally fans
them out across the pool with `min(pool.size, len(reqs))` worker
threads — same throughput as manual fanout but a single-call API.

```c
// C — submit 200 Identity GAA requests, get them back in order.
bp_batch_item_t items[200];
for (int i = 0; i < 200; i++) {
    items[i].req = identity_req;  items[i].req_size = sizeof(identity_req);
    items[i].resp = bufs[i];      items[i].resp_capacity = 256;
}
bp_client_pool_batch(c, /*slot*/2, items, 200);
// items[i].rc + items[i].resp_len populated.
```

```go
// Go
items := make([]ocxbp.BatchItem, 200)
for i := range items { items[i].Req = identityReq }
_ = c.PoolBatch(2, items, 256)
// items[i].Resp + items[i].Err populated.
```

```python
# Python
results = c.pool_batch(2, [identity_req] * 200, 256)
# results[i] is (resp_bytes, exception) — exactly one is None.
```

### Multi-hop routes via Unconnected_Send (v0.8.0)

To target a device beyond the local backplane (EtherNet/IP node off
the L85, ControlNet, DH+, etc.), wrap the target's CIP request inside
`Unconnected_Send` (CIP svc `0x52`) targeting the slot device's
Connection Manager.  No SDK or wire-protocol change; the chip just
forwards the bytes.  See
[`docs/protocol.md#multi-hop-routes--unconnected_send-service-0x52`](docs/protocol.md)
for the byte layout.

```c
// C — Identity GAA routed through L85 (slot 2) back to itself.
uint8_t embedded[] = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };
uint8_t route[8];  size_t roff = 0;
bp_route_append_port(route, sizeof(route), &roff, /*port*/1, /*link*/2);
uint8_t wrapped[256];
int wlen = bp_build_unconnected_send(wrapped, sizeof(wrapped),
                                       embedded, sizeof(embedded),
                                       route, roff, 5000);
// Send via MessageSend / TxRxMsg / pool_txrx — the chip is transport-agnostic.
```

```go
// Go
route := make([]byte, 8); off := 0
cip.AppendPortSegment(route, &off, 1, 2)
wrapped := make([]byte, 256)
wlen := cip.BuildUnconnectedSend(wrapped, embedded, route[:off], 5000)
```

```python
# Python
route = bpclient.port_segment(1, 2)
wrapped = bpclient.build_unconnected_send(embedded, route, 5000)
```

### Arrays + STRING

v0.6.0 ships full array + Logix-STRING parity in every SDK:

```go
// Go
vals, _ := db.ReadDINTArray("Test_DINT_Arr", 10)         // []int32
db.WriteREALArray("Test_REAL_Arr", []float32{1, 2, 3, 4, 5})
db.WriteBOOLArray("Test_Bool_Arr", []bool{true, false, ...})
name, _ := db.ReadString("Test_STRING")                   // string
db.WriteString("Test_STRING", "hello")
```

```python
# Python
vals = db.read_dint_array("Test_DINT_Arr", 10)            # list[int]
db.write_real_array("Test_REAL_Arr", [1.0, 2.0, 3.0, 4.0, 5.0])
db.write_bool_array("Test_Bool_Arr", [True, False, ...])
name = db.read_string("Test_STRING")                       # str
db.write_string("Test_STRING", "hello")
```

Logix `BOOL[]` is packed as `ceil(N/32)` DWORDs on the wire (CIP type
`0xD3`); the helpers handle pack/unpack transparently. STRING reads
both `tag.LEN` and `tag.DATA` in two IPC round-trips and works with
the default `STRING`, `STRING_32`, `STRING_512`, and any LEN+DATA-
shaped UDT.

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
| `tagtest`      | Canonical DINT read / write / readback round-trip |
| `typetest`     | Cross-type sweep — every scalar + STRING + 1-D/2-D/3-D arrays + BOOL[] |
| `msgprobe`     | Raw `OCXcip_MessageSend` invocation + response hexdump |
| `identity`     | Local + remote Identity dump |
| `connidentity` | Class-3 connected Identity (LFO + bare Identity + FC) |
| `conntest`     | Class-3 round-trip validator (N Identities) + `--bench` UCMM-vs-Class3 latency |
| `pooltest`     | v0.8.0 pool + keepalive validator (M workers × N requests through a pre-opened pool) |
| `routedident`  | v0.8.0 multi-hop Identity via Unconnected_Send (svc 0x52) + route_path |
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
- **Large-buffer (>500 B) connected messaging** — v0.7.0 ships
  small-buffer class-3 (LFO + bare CIP + FC routed through
  `MessageSend`).  The chip's mailbox 0x204 path that delivers
  4002-byte packets is not reachable through `OCXcip_MessageSend`
  and is tracked separately in
  [`docs/v0.8-large-buffer-re.md`](docs/v0.8-large-buffer-re.md).

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
