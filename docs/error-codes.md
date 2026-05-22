# Error codes

The server writes a signed 32-bit error code to `slot + 0x50` (the
`errorcode` header field) on every response. `0` = success;
non-zero = failure. The values are inherited from the vendor's
`OCX_ERR_*` constants in `include/ocxbpapi.h` so traces are
cross-referenceable with vendor logs.

Each language SDK maps these to its idiomatic error mechanism:

- **C**: `int` return values. Convert to a string with `bp_strerror(rc)`.
- **Go**: `Errno int32` type implementing the `error` interface.
- **Python**: `BackplaneError(code, message)` exception class.

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
