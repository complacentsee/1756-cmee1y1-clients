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
| `tagtest`      | Canonical Read/Write/Readback round-trip |
| `msgprobe`     | Raw `OCXcip_MessageSend` + response hexdump |
| `identity`     | Local + remote Identity dump |
| `connidentity` | Class-3 connected Identity (NOT FUNCTIONAL on cm1756) |
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

Phase 5 (2026-05). Outbound tag I/O is fully functional. The
class-3 connected `TxRx*` methods are kept for API parity but
return engine code `0x1001` on cm1756 — see
[`docs/protocol.md`](../docs/protocol.md) "Connected messaging
— open issues".
