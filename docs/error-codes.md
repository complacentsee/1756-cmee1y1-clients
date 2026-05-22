# Error codes

The server writes a signed 32-bit error code to `slot + 0x50` (the
`errorcode` header field) on every response. `0` = success;
non-zero = failure. The values are inherited from the vendor's
`OCX_ERR_*` constants in `include/ocxbpapi.h` so traces are
cross-referenceable with vendor logs.

Each language SDK maps these to its idiomatic error mechanism:

- **C** ([`c/include/bpclient.h`](../c/include/bpclient.h)): `int`
  return values; `BP_OK` (0) or a `BP_ERR_*` constant.  Convert to
  a string with `bp_strerror(rc)`.  When the return code is
  `BP_ERR_CIP_STATUS`, the structured CIP-layer fields are available
  via `bp_client_last_cip_error(client, &out)` (see below).
- **Go** ([`go/ocxbp/errors.go`](../go/ocxbp/errors.go)): named
  sentinels (`ocxbp.ErrParamRange`, `ocxbp.ErrNoFreeSlot`, …)
  plus `ocxbp.EngineError{Code int}` for engine codes that aren't
  in the `BP_ERR_*` table, plus `*ocxbp.CIPError` for CIP-layer
  rejections.  Use `errors.As` / `errors.Is`.  Map back to the C
  int with `ocxbp.ErrCode(err)`.
- **Python** ([`python/src/bpclient/errors.py`](../python/src/bpclient/errors.py)):
  exception hierarchy rooted at `BpError`; one subclass per
  `BP_ERR_*` code (`BpParamRange`, `BpNoFreeSlot`, …) plus
  `BpEngine(code)` for non-`BP_ERR_*` engine codes and `BpCipError`
  for CIP-layer rejections.  Map back to the C int with
  `bpclient.err_code(exc)`.

The per-language error names line up so a `BP_ERR_PARAM_RANGE` from
the engine surfaces as `ocxbp.ErrParamRange` in Go and
`bpclient.BpParamRange` in Python.

## Catalog

| Code | Symbol | Meaning | Recovery |
|---|---|---|---|
| `0`        | `OK` | Success | – |
| `-1`       | `GENERIC` | Unspecified failure | Check server logs; often follows a SIGSEGV or unhandled exception in the engine |
| `-200`     | `SEND_REQUEST` | `sem_post(/bpReqN)` failed at server-side | Restart bpServer (or just retry — sometimes transient) |
| `-201`     | `RECV_ANSWER` | `sem_wait(/bpRespN)` failed at server-side | Same as above |
| `-300`     | `NULL_ARG` | Required argument was NULL | Caller bug; fix request |
| `-301`     | `PENDING` | Server hasn't replied yet (still working on the request) | NOT an error code per se — this is the sentinel value the slot is initialized to. If you see this AFTER your sem_timedwait returned, the server crashed mid-request. |
| `-303`     | `NOT_OPEN` | `OCXcip_Open` wasn't called first, OR the IPC connection was lost | Call `Open()` again |
| `-305`     | `PARAM_RANGE` | A request field is out of valid range (commonly a bad path) | Check path string format (`P:1,S:2`, not `1,2`); verify tag name exists |
| `-311`     | `SLOT_TOO_LARGE` | Response wouldn't fit in the slot (>0x4B000 bytes) | Reduce batch size or read fewer elements |
| `-400`     | `CIP_STATUS` | CIP-layer rejection (transport succeeded; PLC returned non-zero `general_status`) | See "Structured CIP-layer errors" below — retrieve `(service, status, ext_status, slot)` and decide based on the table. |
| `-103001`  | `NO_FREE_SLOT` | All 16 slots are owned by other clients | Wait and retry; if persistent, check for stuck clients (other procs with crashed sessions) |
| `-101802`  | `CLIENT_OPEN` | shm_open / ftruncate / mmap failed | Check that bpServer is running; verify `/dev/shm/bpShmem` exists |

## Per-request result codes (in `AccessTagData`)

In addition to the slot-level `errorcode`, each request in an
`OCXcip_AccessTagData` batch has its own `result` field at descriptor
`+0x118`. These follow standard **CIP General Status** codes:

| CIP Status | Meaning |
|---|---|
| `0x00` | Success |
| `0x04` | Path segment error (bad tag name) |
| `0x05` | Path destination unknown |
| `0x08` | Service not supported |
| `0x0E` | Attribute not settable (write to read-only) |
| `0x13` | Not enough data |
| `0x15` | Too much data |
| `0x1C` | Attribute not retrievable |
| `0x26` | Path size invalid |

The slot-level `errorcode` is 0 when the batch as a whole was
processed (network round-trip completed); individual request
failures show up only in the per-request `result` fields.

## Structured CIP-layer errors (v0.8.0+)

The TxRx* connected-messaging family (`bp_client_txrx_open` /
`Client.TxRxOpen` / `client.txrx_open`, and the matching `close`
calls) returns `BP_ERR_CIP_STATUS` (-400) when the transport
round-trip succeeded but the PLC rejected the request at the CIP
layer.  The structured fields are exposed per-language:

**C** — `bp_client_last_cip_error()` after a `BP_ERR_CIP_STATUS`
return; reading clears the record:

```c
int rc = bp_client_txrx_open(client, &spec, &cid, &cs);
if (rc == BP_ERR_CIP_STATUS) {
    bp_cip_status_t e;
    bp_client_last_cip_error(client, &e);
    if (e.status == 0x01 && e.ext_status == 0x0100) {
        /* "Connection in use" — let PLC idle-time-out and retry. */
    }
    fprintf(stderr, "rejected: %s\n",
            bp_cip_status_string(e.status, e.ext_status));
}
```

**Go** — `*ocxbp.CIPError` via `errors.As`:

```go
if _, _, err := c.TxRxOpen(spec); err != nil {
    var ce *ocxbp.CIPError
    if errors.As(err, &ce) {
        if ce.Status == 0x01 && ce.ExtStatus == 0x0100 {
            // "Connection in use" — retry after idle timeout.
        }
        log.Printf("rejected: %s",
            ocxbp.CIPStatusString(ce.Status, ce.ExtStatus))
    }
}
```

**Python** — `bpclient.BpCipError` with `.service / .status /
.ext_status / .slot` attributes:

```python
try:
    cid, cs = client.txrx_open(spec)
except bpclient.BpCipError as e:
    if e.status == 0x01 and e.ext_status == 0x0100:
        # "Connection in use" — retry after idle timeout.
        ...
    print(f"rejected: {bpclient.cip_status_message(e.status, e.ext_status)}")
```

### Forward_Open / Forward_Close failure modes

`bp_client_txrx_open` sends a Large Forward Open (CIP svc `0x5B`).
`bp_client_txrx_close` sends Forward_Close (CIP svc `0x4E`).  When
either is rejected the SDK returns `BP_ERR_CIP_STATUS` and records
the structured fields described above.  Common modes:

| LFO/FC reply | CIP `general_status` | Meaning | Recovery |
|---|---|---|---|
| `0xDB 00 00 …`              | `0x00` | Success | – |
| `0xDB 00 02 …`              | `0x02` | Resource unavailable | Most common cause is `spec.conn_params` requesting an oversized buffer. SDK caps at 4002 B; values above the OCX-negotiated 4000 may still trip the PLC for some object classes. Pass `conn_params=0` for the safe default. |
| `0xDB 00 05 …`              | `0x05` | Path destination unknown | Slot has no device, or the device doesn't accept Forward_Open on Msg Router (class 2 inst 1). Verify with `actnodes` / `connidentity --slot N`. |
| `0xDB 00 01 ext=0x0100`     | `0x01` | Connection in use | PLC has a stale connection from a prior session; let it idle-time-out (~40 s with multiplier=3) or restart bpServer. |
| `0xDB 00 01 ext=0x0103`     | `0x01` | Transport class unsupported | Controller firmware rejected `0xA3`. |
| `0xDB 00 01 ext=0x0113`     | `0x01` | No more connections available on target | Free a slot on the PLC; this often hits during long-running test sweeps. |
| `0xDB 00 09 ext=0x0316`     | `0x09` | Invalid attribute | Connection path has a malformed segment. Should not happen with the canonical `20 02 24 01` body. |
| `0xCE 00 01 ext=0x0107`     | `0x01` | Forward_Close: connection ID not found | The PLC had already cleaned up by idle timeout. Safe to ignore — the SDK frees its cached state regardless of FC outcome. |
| `0xCE 00 01 ext=0x0119`     | `0x01` | Forward_Close: conn ID mismatch | Serial/vendor/orig-serial in the FC don't match the PLC's table. The SDK frees its cached state regardless; the connection will idle-time-out on the PLC. |

The full status/ext table is in the SDK-side helpers
(`bp_cip_status_string`, `ocxbp.CIPStatusString`,
`bpclient.cip_status_message`) and stays in sync with this doc.

## How to test recovery flows

- **`-301 PENDING` after sem_timedwait**: kill `bpServer` from the
  HMI (you can't — but as a thought experiment) and retry your call.
  You should get `-303 NOT_OPEN` on the next call.
- **`-305 PARAM_RANGE` on path**: deliberately call
  `OpenTagDB("1,2")` instead of `OpenTagDB("P:1,S:2")` — guaranteed
  `-305`.
- **`-103001 NO_FREE_SLOT`**: spawn 16 concurrent clients each
  holding a slot. The 17th gets `-103001`.

These are covered by the test suites in each language SDK.
