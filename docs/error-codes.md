# Error codes

The server writes a signed 32-bit error code to `slot + 0x50` (the
`errorcode` header field) on every response. `0` = success;
non-zero = failure. The values are inherited from the vendor's
`OCX_ERR_*` constants in `include/ocxbpapi.h` so traces are
cross-referenceable with vendor logs.

Each language SDK maps these to its idiomatic error mechanism:

- **C** ([`c/include/bpclient.h`](../c/include/bpclient.h)): `int`
  return values; `BP_OK` (0) or a `BP_ERR_*` constant.  Convert to
  a string with `bp_strerror(rc)`.
- **Go** ([`go/ocxbp/errors.go`](../go/ocxbp/errors.go)): named
  sentinels (`ocxbp.ErrParamRange`, `ocxbp.ErrNoFreeSlot`, …)
  plus `ocxbp.EngineError{Code int}` for engine codes that aren't
  in the `BP_ERR_*` table.  Use `errors.As` / `errors.Is`.  Map
  back to the C int with `ocxbp.ErrCode(err)`.
- **Python** ([`python/src/bpclient/errors.py`](../python/src/bpclient/errors.py)):
  exception hierarchy rooted at `BpError`; one subclass per
  `BP_ERR_*` code (`BpParamRange`, `BpNoFreeSlot`, …) plus
  `BpEngine(code)` for non-`BP_ERR_*` engine codes.  Map back to
  the C int with `bpclient.err_code(exc)`.

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

## Forward_Open / Forward_Close failure modes (v0.7.0+ TxRx)

`bp_client_txrx_open` / `Client.TxRxOpen` / `client.txrx_open` send
a Large Forward Open (CIP svc `0x5B`) and parse the reply.  Common
failures surfaced as `BP_ERR_GENERIC` with a stderr breakdown of
the CIP status byte:

| LFO reply | CIP `general_status` | Meaning | Recovery |
|---|---|---|---|
| `0xDB 00 00 …`              | `0x00` | Success | – |
| `0xDB 00 02 …`              | `0x02` | Resource unavailable | Most common cause is `spec.conn_params` requesting an oversized buffer. SDK caps at 4002 B; values above the OCX-negotiated 4000 may still trip the PLC for some object classes. Pass `conn_params=0` for the safe default. |
| `0xDB 00 05 …`              | `0x05` | Path destination unknown | Slot has no device, or the device doesn't accept Forward_Open on Msg Router (class 2 inst 1). Verify with `actnodes` / `connidentity --slot N`. |
| `0xDB 00 01 ext=…`          | `0x01` | Connection failure (with ext status) | Common ext codes: `0x0100` "Connection in use" — the PLC still has a stale connection from a prior session; let it idle-time-out (~40 s with multiplier=3) or restart bpServer. `0x0103` "Transport class unsupported" — controller firmware rejected `0xA3`. `0x0107` "Connection ID not found in close" (FC-only). |
| `0xDB 00 09 ext=0x0316`     | `0x09` | Invalid attribute | Connection path has a malformed segment. Should not happen with the canonical `20 02 24 01` body. |

`bp_client_txrx_close` / FC failures: PLC's `Forward_Close` reply
(`0xCE`) returns CIP general_status `0x01 ext=0x0107` if the
connection serial / vendor / orig serial don't match its table.
The SDK frees the cached state regardless of FC outcome — so a
stale FC failure doesn't block subsequent re-open.

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
