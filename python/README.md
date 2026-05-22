# bpclient (Python) — outbound CIP for the 1756-CMEE1Y1

Python SDK for outbound CIP tag I/O against a 1756 ControlLogix
PLC, dispatched through the ASEM EEC card's `bpServer` userland IPC.

API mirrors the C SDK (libbpclient) shape-for-shape; uses idiomatic
Python (dataclasses, exceptions, return values).

## Quick example

```python
import bpclient

with bpclient.Client() as c:
    c.open_session()
    db = c.open_tagdb("P:1,S:2")  # ControlLogix in backplane slot 2
    try:
        n = db.build()
        print(f"{n} symbols on the PLC")

        v = db.read_dint("OCX_TEST")
        db.write_dint("OCX_TEST", 0xCAFEBABE - (1 << 32))
        assert db.read_dint("OCX_TEST") == 0xCAFEBABE - (1 << 32)
        db.write_dint("OCX_TEST", v)  # restore

        # Arrays + STRING (v0.6.0):
        arr = db.read_dint_array("Test_DINT_Arr", 10)
        db.write_bool_array("Test_Bool_Arr", [True, False, True])
        name = db.read_string("Test_STRING")
    finally:
        db.close()
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
py python/build_image.py
py runprobe.py --image bpclient-python-tagtest:dev tagtest
```

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
| `symcache`     | v0.9.0 symbol-cache validator (cold / warm / preload timing) |
| `multitagtest` | v0.9.0 read_tags + write_tags mixed-type batch round-trip |
| `pathprobe`    | `OCXcip_ParsePath` dispatch dump |
| `actnodes`     | Active-node bitmap |
| `modutil`      | Local switch / display / LED utilities |

Override at run time with `--entrypoint /usr/local/bin/<tool>`,
or pass `<tool>` to `runprobe.py`.

## Test suite

```
cd python
pip install -e . pytest posix_ipc
pytest tests/                                          # unit tests, no PLC
BP_PLC_PATH=P:1,S:2 pytest tests/                      # + end-to-end
```

* `test_slot_dispatch.py` — unit tests with a synthetic IPC fake
  (Phase 4 / Phase 2 wire correctness assertions).
* `test_ipc_open.py` — integration; skipped if `/dev/shm/bpShmem`
  isn't present.
* `test_tagtest_e2e.py` — integration; skipped unless `BP_PLC_PATH`
  is set.

## Status

v0.9.0.  Outbound tag I/O fully functional including v0.9.0's
application-layer ergonomic surface on top of the v0.8.0 transport
primitives:

- `db.lookup_symbol(name)` / `db.preload_symbols()` — per-client
  symbol cache amortizes the per-symbol IPC.
- `db.read_tags(names)` / `db.write_tags({name: value})` —
  mixed-type scalar batch read/write in one AccessTagData round-trip.
  `write_tags` validates value types against the symbol's data_type
  pre-IPC (bool checked BEFORE int, since Python's bool inherits
  from int).
- Pool auto-reopen on dead entries — long-running scan loops
  survive transient PLC hiccups; backoff 1 s → 2 s → ... cap 30 s.

(v0.8.0 added `BpCipError`, the per-slot connection pool with
keepalive, `pool_batch`, and multi-hop routes via Unconnected_Send.)

Inherited limitation: small-buffer transport (~500 B envelope
inherited from `message_send`).  The 4002-byte chip-mailbox-0x204
path is shelved per [`docs/v0.8-large-buffer-re.md`](../docs/v0.8-large-buffer-re.md)
(operational blocker on rootless containers, not a missing-symbol
issue).
