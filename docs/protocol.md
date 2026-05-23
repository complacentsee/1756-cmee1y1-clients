# Wire protocol spec

This is the **single source of truth** for the bpServer userland IPC.
The C, Go, and Python SDKs each implement directly against this
document — none of them shares code with the others.

If you find an inconsistency between this doc and an implementation,
**the doc is wrong** until corrected here. Implementations must not
introduce protocol details that aren't documented here first.

## Source attribution

Every layout choice in this document traces to specific
reverse-engineering work in `complacentsee/rockwell-bpgateway-re`:

- Slot layout: `include/ocxbpapi_shm.h` (RE'd from Ghidra of
  `binaries/wrapper/libocxbpapi-w.so.3`)
- `OCXcip_Open`, `OCXcip_CreateTagDbHandle`, `OCXcip_BuildTagDb`,
  `OCXcip_GetSymbolInfo`: `docs/phase1-results.md`
- `OCXcip_AccessTagData` field semantics fix
  (`elem_byte_size`/`elem_count` correction): `docs/phase2-results.md`
- Error codes: `include/ocxbpapi.h` `OCX_ERR_*` constants
- Path-string format quirk: `tools/probe_path_format.py`

## Implementations

Each language SDK implements directly against this spec — no shared
code, no generated bindings. The dispatcher / opcode layout / field
offsets are duplicated faithfully; the three are kept honest via
the `runprobe.py` byte-identical diff harness.

| Language | Module / package | Dispatcher | Per-opcode encoders | Public API |
|---|---|---|---|---|
| C        | `libbpclient` (`c/`) | [`c/src/client.c`](../c/src/client.c) (`bp_client_call`) | `c/src/*.c` (one file per opcode area) | [`c/include/bpclient.h`](../c/include/bpclient.h) |
| Go       | `.../go/ocxbp` | [`go/ocxbp/shm/call.go`](../go/ocxbp/shm/call.go) (`Client.Call`) | [`go/ocxbp/cip/`](../go/ocxbp/cip/) (one file per opcode area) | [`go/ocxbp/`](../go/ocxbp/) (`Client`, `TagDB`, …) |
| Python   | `bpclient` (`python/`) | [`python/src/bpclient/_ipc.py`](../python/src/bpclient/_ipc.py) (`Client.call`) | [`python/src/bpclient/client.py`](../python/src/bpclient/client.py) + per-area dataclasses | [`python/src/bpclient/__init__.py`](../python/src/bpclient/__init__.py) (`Client`, `TagDB`, …) |

If you change a field offset, opcode size, or payload layout in
this doc, the same change must land in each implementation's
dispatcher / encoders. `py runprobe.py --image
bpclient-{c,go,python}-tagtest:dev tagtest` + the `msgprobe`
slot-sweep are the canonical regression check.

## Transport: POSIX shared memory + named semaphores

### `/dev/shm/bpShmem` — the slot array

```
Total size:    0x4B0000 bytes  (16 slots × 0x4B000 each)
Slot stride:   0x4B000 bytes   (~300 KiB per slot)
Slot count:    16              (numbered 0..15)
```

Each slot is one in-flight request/response. A client claims a slot,
fills its header + payload, signals the server via a named semaphore,
and waits on the matching response semaphore.

Open with `shm_open("/bpShmem", O_RDWR, 0)` then `mmap(... PROT_READ |
PROT_WRITE, MAP_SHARED ...)`. The kernel exposes this as
`/dev/shm/bpShmem` so you can also `open()` that path directly — both
forms are equivalent.

### Named semaphores

Standard POSIX `sem_open("/<name>", 0)`. On Linux these appear as
`/dev/shm/sem.<name>` files.

| Name | Count | Direction | Purpose |
|---|---|---|---|
| `/bpShm` | 1 | mutex | Serializes slot-table scans. Hold while picking a free slot. |
| `/bpReq00`..`bpReq15` | 16 | client → server | Caller `sem_post`s the matching slot's req sem after writing the request. |
| `/bpResp00`..`bpResp15` | 16 | server → client | Server `sem_post`s after writing the response. Caller `sem_timedwait`s for it. |
| `/bpCbConn00`..`bpCbConn15` | 16 | server → client | Async callback notify. Used by `OCXcip_RegisterAssemblyObj`-style callbacks; **NOT used by this SDK** (outbound-only). |
| `/bpCbServ00`..`bpCbServ15` | 16 | unused | Defined but not used on cm1756. |
| `/bpCbMsg00`..`bpCbMsg15` | 16 | unused | Defined but not used on cm1756. |
| `/bpCbTxRx00`..`bpCbTxRx15` | 16 | unused | Defined but not used on cm1756. |

For outbound tag I/O you only need `bpShm` + the 16 each of `bpReq` /
`bpResp`. That's **33 semaphores** open per client process.

## Slot header (the first 0x78 bytes of every slot)

```
+0x00  uint16  opcode             0x00CA for every OCXcip_* call
+0x02  uint16  (padding)
+0x04  uint32  payload_size       per-opcode constant; see "Per-opcode payloads" below
+0x08  char[63] fn_name           NUL-padded ASCII function name (no leading "OCXcip_" stripped)
+0x47  uint8   fn_name_terminator always 0
+0x48  uint32  client_pid         the calling process's PID (getpid())
+0x4C  uint16  is_docker          0 or 1 — set 1 if calling from a container
+0x4E  uint16  (padding)
+0x50  int32   errorcode          server writes; -301 (= 0xFFFFFED3 unsigned) = "pending"
+0x54  uint32  (padding)
+0x58  uint64  slot_owner         (gettid() << 32) | getpid(); 0 means slot is free
+0x60  uint32  slot_number        index of this slot in the array (0..15)
+0x64  uint32  (padding)
+0x68  16 bytes (reserved, write zeros)
+0x78           PAYLOAD STARTS    per-opcode marshalling begins at this offset
```

`fn_name` is the literal `OCXcip_<Whatever>` string, NUL-padded
exactly to 63 bytes + a NUL at +0x47 (so positions 0x08..0x47
inclusive are the C-string region).

## Slot ownership state machine

```
+----------+      sem_wait(/bpShm)         +-----------+
|   FREE   | ----------------------------> |  CLAIMING |
| owner=0  |   scan; first slot.owner==0   | (under    |
|          |                                |   /bpShm) |
+----------+                                +-----------+
    ^                                              |
    |                                              | write owner = (tid<<32)|pid
    |                                              | drain sem_reqN / sem_respN
    |                                              | sem_post(/bpShm)
    |                                              v
+----------+                                +-----------+
| RELEASED | <--- sem_wait(/bpShm)        | OWNED     |
| owner=0  |     write owner=0             | (caller   |
|          |     drain semaphores          |  builds   |
|          |     sem_post(/bpShm)          |  request) |
+----------+                                +-----------+
                                                   |
                                                   | write header + payload
                                                   | errorcode = -301
                                                   | sem_post(/bpReqN)
                                                   v
                                            +-----------+
                                            | PENDING   |
                                            | (server   |
                                            |  works)   |
                                            +-----------+
                                                   |
                                                   | server fills response,
                                                   | sets errorcode != -301,
                                                   | sem_post(/bpRespN)
                                                   v
                                            +-----------+
                                            | DONE      |
                                            | (caller   |
                                            |  reads    |
                                            |  reply)   |
                                            +-----------+
                                                   |
                                                   v
                                            (back to RELEASED)
```

The `/bpShm` mutex is held ONLY during the scan/claim and the
release. The request/response phase doesn't hold it — that lets 16
clients work in parallel.

**Local synchronization:** within one client process, also serialize
the slot scan with your own mutex. The `/bpShm` POSIX semaphore
protects across processes, but two threads in the same process can
race the scan if you only hold the cross-process sem.

## The call sequence (everything outbound does this)

1. `sem_wait(/bpShm)` — block on the scan mutex (cross-process)
2. (local mutex) scan slots 0..15 for `slot[i].owner == 0`
3. Set `slot[i].owner = (tid << 32) | pid` (claim it)
4. Drain stale posts: `while sem_trywait(/bpReqN) == 0`, same for `/bpRespN`
5. `sem_post(/bpShm)` — release the scan mutex
6. Fill slot header (opcode, fn_name, payload_size, pid, is_docker, slot_number)
7. Set `slot[i].errorcode = -301` (pending)
8. Fill payload (per-opcode; see below)
9. `sem_post(/bpReqN)` — kick the server
10. `sem_timedwait(/bpRespN, deadline)` — block for reply (typically 1–500 ms)
    - On `ETIMEDOUT`, fall back to polling `errorcode != -301` for up to 200ms
11. Read `errorcode`; if non-zero, that's the OCX_ERR_* return
12. Parse response from slot
13. `sem_wait(/bpShm)`; `slot[i].owner = 0`; drain `/bpReqN`,`/bpRespN`; `sem_post(/bpShm)`

## Per-opcode payloads

Every opcode uses `opcode = 0x00CA`. The fn_name string is what
distinguishes them.

### `OCXcip_Open` — open a session

```
fn_name        = "OCXcip_Open"
payload_size   = 0x80

REQUEST PAYLOAD: (none)

RESPONSE PAYLOAD:
  slot + 0x78  uint32  session_handle  (opaque; you don't pass it back to any other call)
```

In practice the session_handle is bookkeeping — the wrapper IPC
tracks state via slot ownership, not the handle. But the call must
happen and must succeed before any other OCXcip_* call works.

### `OCXcip_Close` — release the session (v0.9.0+)

```
fn_name        = "OCXcip_Close"
payload_size   = 0x78

REQUEST PAYLOAD:  (none — entire 0x78 bytes is the slot header)
RESPONSE PAYLOAD: errorcode only
```

Releases the engine-side session opened by `OCXcip_Open`.  Without
this, bpServer's session table accumulates a dead entry per process
exit; once full, the next process's `OCXcip_CreateTagDbHandle`
returns engine errorcode 8 ("session table full") and the only
recovery is restarting bpServer.

The SDK's `bp_client_close` / `Client.Close()` / `client.close()`
methods call this automatically.  Wire address in the OEM library:
`OCXcip_Close` at `0x0010ACF4` (RE'd from `libocxbpapi-w.so`).

### `OCXcip_Dummy` — no-op server roundtrip (v0.10.3+)

```
fn_name        = "OCXcip_Dummy"
payload_size   = 0x78

REQUEST PAYLOAD:  (none — entire 0x78 bytes is the slot header)
RESPONSE PAYLOAD: errorcode only
```

Cheap liveness probe — server reads nothing, writes nothing, just
returns success.  Useful for confirming the bpServer dispatcher is
alive without committing engine-side session state (unlike
`OCXcip_Open` which allocates a session table entry).

Wire format confirmed via Ghidra decompile of
`OCXcip_Dummy @ 0x0010A180` (RE'd from `libocxbpapi-w.so`).  The
wrapper writes only the standard slot header fields (`fn_name`
string at +0x08, `payload_size` at +0x04, `localPid` / `localDocker`
/ slot_index, and opcode `0xCA` at +0x00) — the rest of the slot is
untouched.

SDKs: `bp_client_dummy` / `Client.Dummy` / `client.dummy`.

### `OCXcip_GetDeviceIdStatus` — lightweight status-word probe (v0.10.0+)

```
fn_name        = "OCXcip_GetDeviceIdStatus"
payload_size   = 0x180

REQUEST PAYLOAD:
  slot + 0x078  char[255]  text_path        NUL-terminated, max 254 bytes
  slot + 0x17A  uint16     instance         normally 1
                                              NOTE: NOT +0x178 (that's the
                                              engine's output slot — wrapper
                                              clears it before dispatch).

RESPONSE PAYLOAD:
  slot + 0x178  uint16     status           bits 0..3 reserved, bits 4..7
                                              extended device status (Logix mode:
                                              0x3=RUN, 0x4=PROGRAM, ...)
```

Returns just the 16-bit Identity status word — cheaper than
`GetDeviceIdObject` (48-byte Identity struct) when callers only need
the heartbeat or run-program nibble.

Wire format confirmed via Ghidra decompile of
`OCXcip_GetDeviceIdStatus @ 0x108620` —
the slot pointer in the wrapper is `undefined2 *` (uint16 indices),
and the relevant operations are:
- `puStack_8[0xbc] = 0` clears slot+0x178 (output)
- `puStack_8[0xbd] = param_4` writes instance to slot+0x17A
- `*param_3 = puStack_8[0xbc]` reads back from slot+0x178 (status)

> **cm1756 divergence**: empirically the cm1756 `bpServer` returns
> `rc=0` for `OCXcip_GetDeviceIdStatus` but leaves slot+0x178 holding
> residual bytes (looks like fragments of the input path text), not
> the expected status word.  The OEM library's `libocxbpapi-w.so`
> expects the response at `+0x178` per the wrapper code, so this is
> a server-side quirk of this image rather than an SDK marshalling
> bug.  Callers needing reliable Identity status on cm1756 should
> fall back to `bp_client_get_device_id().status` (the full Identity
> object's status field at the same offset within the Identity
> struct) until the bpServer behavior is RE'd separately.

### `ReconnectClient` — IPC restart after bpServer restart (v0.10.0+, client-side)

Not a wire opcode at all — `ReconnectClient` at
`libocxbpapi-w.so:0x107e00` is a client-side helper that performs:

1. `comClient.Close()` — close all 33 semaphores + munmap shm.
2. `usleep(50000)` — 50 ms gap so a restarted bpServer can publish
   its `/bpReq*` / `/bpResp*` named semaphores.
3. `comClient.Open()` — reopen everything.

The SDK exposes `bp_client_reconnect` / `Client.Reconnect` /
`client.reconnect`.  These INVALIDATE EVERYTHING the caller held —
pools, tag databases, symbol caches, and class-3 TxRx state are all
wiped before the IPC restart.  After a successful reconnect callers
re-call `open_session` and re-open any pools / tag databases.

### Wall-clock get/set (v0.10.0+)

```
fn_name        = "OCXcip_GetWCTime" | "OCXcip_GetWCTimeUTC" |
                 "OCXcip_SetWCTime" | "OCXcip_SetWCTimeUTC"
payload_size   = 0x1B0

REQUEST PAYLOAD:
  slot + 0x078  char[255]  text_path        NUL-terminated, max 254 bytes
  slot + 0x178  uint16     instance         normally 1
  slot + 0x17A  uint8      have_buffer      1 if get/set input is present
  slot + 0x180  uint64     sec              (input for Set; output for Get)
  slot + 0x188  uint64     nsec
  slot + 0x190  uint64     aux0             TZ / DST / leap metadata
  slot + 0x198  uint64     aux1
  slot + 0x1A0  uint64     aux2
  slot + 0x1A8  uint64     aux3

RESPONSE PAYLOAD (Get only):
  Same 6 qwords at +0x180..+0x1B0 (engine overwrites).
```

Wire format from Ghidra: `OCXcip_GetWCTime @ 0x10e2e0`,
`OCXcip_SetWCTime @ 0x10e4c0` (and the UTC variants at `0x10e6a0`,
`0x10e894`).  Both Get and Set share the same payload layout — the
`have_buffer` flag at `+0x17A` toggles whether the qword region is
consumed (Set) or returned (Get).

The four SDK helpers expose this as a raw 6-qword struct
(`bp_wctime_t` / `ocxbp.WCTime` / `bpclient.WCTime`).  Field
semantics validated empirically against the cm1756 (v0.10.2,
2026-05-22):

- **`sec`** is microseconds since a per-PLC epoch (NOT Unix seconds
  as the OEM header annotation claimed).  Observed map:

  | PLC | `GetWCTime`               | `GetWCTimeUTC`                 |
  |-----|---------------------------|--------------------------------|
  | L73 | µs since 2000-01-01 UTC   | µs since 1998-01-01 UTC        |
  | L85 | µs since 1972-01-01 UTC (ODVA std) | µs since 1970-01-01 UTC (Unix) |

  Pattern: the UTC variant subtracts 2 years from the LOCAL
  variant's epoch (i.e. returns a larger µs value).  Reason
  unclear; consistent across the two devices.  v0.10.2+ ships an
  enum (`bp_wctime_epoch_t` / `ocxbp.WCTimeEpoch` /
  `bpclient.WCTIME_EPOCH_*`) + a `to_unix_us(epoch)` helper that
  does the conversion when the caller knows their device's epoch.

- **`nsec`** is always 0 in observed responses.

- **`aux0..aux3`** carry one of two payloads:
  - **TZ-name ASCII** (observed on the L85's `GetWCTimeUTC`):
    32 little-endian bytes spelling e.g.
    `"0 Pacific Time (US & Canada)"`.  Extracted with
    `bp_wctime_tz_name` / `WCTime.TZName` / `WCTime.tz_name`.
  - **Structured per-field metadata** (observed on `GetWCTime`
    LOCAL): broken-down date/time + sub-second timestamp.  v0.10.3
    ships a PROVISIONAL `aux2` decoder; `aux0`/`aux1` remain
    opaque pending more samples.

  Empirical `GetWCTime` LOCAL aux layout (`aux2` validated on two
  PLC families on 2026-05-22; `aux0` / `aux1` / `aux3` still
  partially understood — see notes per row):

  | Qword | L73 sample (0x… LE)          | L85 sample (0x… LE)          | Notes |
  |-------|------------------------------|------------------------------|-------|
  | aux0  | `000323e043556f9d`           | `00065249bd3dbe80`           | High-resolution timestamp.  Magnitude ≈ 1.78 × 10¹⁵ on L85 → Unix nanoseconds yields ≈ 2026-05-20, right order of magnitude.  Exact unit not yet confirmed; could also be µs since some longer epoch. |
  | aux1  | `0002000107ce0000`           | `0005000507ea0000`           | Bytes 2..3 = year-like (L85 = `0x07EA` = 2026 ✓; L73 = `0x07CE` = 1998 ✗ for 2026 date — likely a per-PLC offset).  Bytes 4..5 = month (L85 = `5` ✓, L73 = `1` ✓).  Byte 7 = day-of-week (L85 = `5` Fri ✓, L73 = `2` Tue ✓). |
  | aux2  | `0023003b00010006`           | `0012002600140016`           | **Confirmed**: four LE uint16s in (day, hour, minute, second) order.  L73 = `(6, 1, 59, 35)` ✓ matches 2026-01-06 01:59:35; L85 = `(22, 20, 38, 18)` ✓ matches 2026-05-22 20:38:18. |
  | aux3  | `0000000000000250`           | `000000000000019c`           | Looks like a small integer field; possibly TZ offset in minutes (L85 = `0x019c` = 412; the L85's PDT offset is 420 min — close but not exact).  Not yet characterized. |

  v0.10.3 exposes the `aux2` decoder as `bp_wctime_decode_local` /
  `WCTime.DecodeLocal` / `WCTime.decode_local`, returning a
  `bp_wctime_local_t` / `WCTimeLocal` struct.  Status: `aux2`
  confirmed across L73 + L85; `aux0`/`aux1`/`aux3` decoders not
  shipped because the field semantics aren't certain.  Re-run
  `wctime --raw` from any SDK to capture more samples and refine
  the remaining fields.

### Extended device info / IP config (v0.10.0+)

```
OCXcip_GetExDevObject — 226-byte extended device info:
  payload_size = 0x260
  request:  path at +0x78, instance at +0x25A (uint16)
  response: 28 qwords + uint16 trailer at +0x178..+0x25A

OCXcip_GetDeviceICPObject — 20-byte EtherNet/IP IP-config:
  payload_size = 0x190
  request:  path at +0x78, instance at +0x18C (uint16)
  response: 2 qwords + 1 uint32 at +0x178..+0x18C
```

Both opcodes are exposed as raw-bytes accessors in the SDK
(`bp_client_get_ex_dev_object` etc.).  Field-level decoders come
back if a real consumer materializes; the engine returns
structured data but its internal layout for the ExDev struct in
particular is vendor-specific and worth deferring until needed.

Wire format from Ghidra: `OCXcip_GetExDevObject @ 0x1087e4`,
`OCXcip_GetDeviceICPObject @ 0x108d00`.

### `OCXcip_ParsePath` — text path → encoded EPATH (v0.10.0+ public; earlier diagnostic)

```
fn_name        = "OCXcip_ParsePath"
payload_size   = 0x288

REQUEST PAYLOAD:
  slot + 0x078  char[255]  text_path        NUL-terminated, max 254 bytes
  slot + 0x280  uint16     encoded_capacity  caller's buffer cap (we pass 256)

RESPONSE PAYLOAD:
  slot + 0x178  uint16     class             parsed class word (0x20 if path
                                              contained a class segment)
  slot + 0x17A  uint8      seg_flags         0x01 if path contains a port seg
  slot + 0x17C  uint32     instance          parsed instance number
  slot + 0x180  uint8[]    encoded_path      binary EPATH bytes
  slot + 0x280  uint16     encoded_size      byte count written into encoded_path
  slot + 0x282  uint8      attr_flags        0x01 if path contains an attribute seg
```

OldI format only: `<letter>:<num>` segments joined by commas
("P:1,S:2", "P:1,S:2,C:1,I:1,A:1", etc.).  Rockwell-style "1,2"
notation is rejected with engine code -101 ("Bad path").

Wire address: `libocxbpapi-w.so:0x1094f0` (RE'd via the legacy
`pathprobe` diagnostic).  v0.10.0 promoted the dispatch from a
diagnostic-only path to the public
`bp_client_parse_path` / `Client.ParsePath` / `client.parse_path`
helpers, plus a typed `bp_parsed_path_t` / `ocxbp.ParsedPath` /
`bpclient.ParsedPath` result struct.

Wire format reconfirmed against Ghidra decompile of
`OCXcip_ParsePath @ 0x1094f0` — slot pointer is `undefined2 *`
(uint16-indexed), and the wrapper:
- writes path text at `slot+0x78` (`func_0x00104530(puStack_8 + 0x3c, param_2, 0xff)`)
- writes caller cap at `slot+0x280` (`puStack_8[0x140] = min(*param_7, 256)`)
- reads `puStack_8[0xbc]` = `+0x178` → `out_class`
- reads `puStack_8[0xbd]` = `+0x17A` → `out_segment_flags`
- reads `puStack_8[0xbe]` = `+0x17C` → `out_instance`
- copies `puStack_8 + 0xc0..` = `+0x180..` → `out_encoded_path`
- reads `puStack_8[0x140]` = `+0x280` → encoded byte count

> **Note** (v0.10.0): OldI port/slot paths like `P:1,S:2`
> return `rc=0` with `encoded_size=0` — the engine maps the routing
> semantics directly into `out_class` / `out_instance` (e.g.
> `class=1, instance=2` for "P:1,S:2") and does not produce a
> separate encoded EPATH byte sequence for those inputs.  The
> `seg_flags` / `attr_flags` bytes for such paths surface as
> apparently-residual ASCII characters from the input text;
> they are not meaningful flags.  This is engine behavior, not
> an SDK bug.

### `OCXcip_ErrorString` — engine-owned rc → ASCII description (v0.10.0+)

```
fn_name        = "OCXcip_ErrorString"
payload_size   = 0xD0

REQUEST PAYLOAD:
  slot + 0x78  int32    code              the rc to translate

RESPONSE PAYLOAD:
  slot + 0x7C  char[78] description       NUL-padded ASCII
```

Input + output overlap on adjacent bytes (0x78 + 4 = 0x7C); the
engine reads the 4-byte input first and overwrites the buffer with
the 78-byte string before returning.  Confirmed empirically — an
earlier attempt to put the input at `+0x50` returned "Successful"
for every code regardless.

The engine owns a static string table indexed by `code`.  Useful for
surfacing engine-internal codes the SDK's hardcoded
`bp_strerror` doesn't cover (positive engine codes, undocumented
negative codes, etc.).

Wire-format RE'd from `libocxbpapi-w.so.3:0x0010A600` — see sibling
`docs/libocxbpapi-w.md §4.2`.

SDK helpers: `bp_client_error_string` (C), `Client.ErrorString` (Go),
`client.error_string` (Python).  Each returns the empty string if
the engine has no entry for `code`.

### `OCXcip_CreateTagDbHandle` — get a per-PLC tag DB handle

```
fn_name        = "OCXcip_CreateTagDbHandle"
payload_size   = 0x180

REQUEST PAYLOAD:
  slot + 0x78  char[255]   path string, NUL-terminated         "P:1,S:2"
  slot + 0x177 uint8       (always 0; ensures terminator)
  slot + 0x178 uint16      flags                                0

RESPONSE PAYLOAD:
  slot + 0x17C uint32      db_handle                            opaque, pass to all later DB calls
```

**Path string format is critical:** OldI uses `<letter>:<num>`
segments joined by `,`. The classic Rockwell `1,2` form is **rejected**
with rc=1 (Bad parameter).

| What you want | Path string |
|---|---|
| ControlLogix at backplane slot 2 | `"P:1,S:2"` |
| Different backplane slot | `"P:1,S:N"` |
| Through another card (CIP routing) | `"P:1,S:2,P:2,N:0"` |

### `OCXcip_BuildTagDb` — walk PLC symbol table

```
fn_name        = "OCXcip_BuildTagDb"
payload_size   = 0x80

REQUEST PAYLOAD:
  slot + 0x78  uint32  db_handle

RESPONSE PAYLOAD:
  slot + 0x7C  uint16  symbol_count    (number of symbols enumerated)
```

Typically takes ~200 ms for a few thousand symbols on a L85E. On a
larger PLC may take seconds — set your timeout accordingly.

### `OCXcip_GetSymbolInfo` — one symbol descriptor

```
fn_name        = "OCXcip_GetSymbolInfo"
payload_size   = 0x100

REQUEST PAYLOAD:
  slot + 0x78  uint32  db_handle
  slot + 0x7C  uint16  index           (0 .. symbol_count - 1)

RESPONSE PAYLOAD:
  slot + 0x80  128 bytes of ocx_symbol_info_t (see below)
```

**`ocx_symbol_info_t` layout** (128 bytes total, offsets within the
struct itself, i.e. relative to `slot + 0x80`):

```
+0x00  char[100]  name             NUL-terminated, up to 100 bytes
+0x64  uint16     data_type        CIP type code (low 13 bits significant); see tag-types.md
+0x66  uint16     (padding)
+0x68  uint16     struct_type      0 for atomic scalars, non-zero for UDT
+0x6A  uint16     (padding)
+0x6C  uint32     elem_byte_size   bytes per element (or struct byte size for UDTs)
+0x70  uint32     dim0             outer dimension; 0 if scalar.  DINT[5,10,30] → 5
+0x74  uint32     dim1             middle dimension; 0 if rank < 2.  DINT[5,10,30] → 10
+0x78  uint32     dim2             inner dimension; 0 if rank < 3.  DINT[5,10,30] → 30
+0x7C  uint16     flags            (bit 0 set: "alias"; other bits undocumented)
+0x7E  uint16     (padding)
```

Note: earlier RE notes (pre-Phase 1 of the 2026-05-21 SDK push) called
+0x78 `instance_id`.  An empirical 3-D dump (`Test_DINT_3D : DINT[5,10,30]`)
showed +0x78 = 30 while remaining 0 for all scalars, 1-D, and 2-D
tags.  Conclusion: the slot has always been `dim2`; the original
"instance_id" label was a wrong guess from looking only at non-3-D
shapes.

### `OCXcip_TestTagDbVer` — has the PLC's tag DB changed?

Cheap (~5 ms) probe — compared to a ~200 ms BuildTagDb on a few-thousand-tag
PLC.  The engine maintains a 12-byte version vector per tagdb handle that
gets captured during BuildTagDb; this call recomputes the current PLC-side
version and memcmp's against the captured one.

```
fn_name        = "OCXcip_TestTagDbVer"
payload_size   = 0x80

REQUEST PAYLOAD:
  slot + 0x78  uint32  db_handle

RESPONSE:
  slot + 0x50  int32   errorcode — also encodes the result:
                        0x00 = versions match (caller's cache is current)
                        0x14 = versions differ (caller should rebuild)
                        0x15 = tagdb has no captured version yet
                               (BuildTagDb has not been called on this handle)
                        other negative values = transport / system errors
```

The C SDK's `bp_tagdb_test_version` collapses `0x14` and `0x15` into
`*out_changed = 1`; both signal the caller to call `bp_tagdb_build`.

### `OCXcip_MessageSend` — one unconnected (UCMM) CIP request

Sends a single UCMM CIP request to a chosen backplane slot and waits
for the raw CIP response.  Use this for arbitrary services (Identity
Get_Attributes_All, custom vendor classes, routed Unconnected_Send,
…) — anything where you have the full CIP request bytes already
encoded.

```
fn_name        = "OCXcip_MessageSend"
payload_size   = 0x32088

REQUEST PAYLOAD:
  slot + 0x00078  uint8[]  cip_request         full CIP request body (req_size bytes)
                                               [service, path_size_words, path..., body...]
  slot + 0x19078  uint16   req_size            byte count of cip_request (1..500)
  slot + 0x3207a  uint16   resp_capacity (in)  caller buffer size (must be > 0)
  slot + 0x32080  uint8    slot                BACKPLANE SLOT NUMBER (0..0x13)
  slot + 0x32082  uint16   timeout_ms          per-attempt timeout (engine clamps min to 26)

RESPONSE PAYLOAD:
  slot + 0x1907a  uint8[]  response data       raw CIP response, resp_len bytes
                                               [reply_svc (= req_svc|0x80), reserved,
                                                general_status, ext_status_size_words,
                                                ext_status[ext_size*2], body...]
  slot + 0x3207a  uint16   resp_len  (out)     bytes the engine wrote
  slot + 0x3207c  uint32   status    (out)     wrapper status field
  slot + 0x50     int32    errorcode           normal slot semantics
```

**Field name correction from earlier revisions of this doc.**  The
OEM wrapper signature is `OCXcip_MessageSend(handle, ep, ep_sz, resp,
&resp_len, &status, service, class_or_misc)`.  RE of the engine
(`OC_bpMessageSend` at libocxbpeng.so.2.3 0x19bf84 →
`um_ProcessClientRequest` at 0x18c518), corroborated by the
historianupdate apex2d daemon's `apex2_asic_send_ucmm`, shows:

| Wrapper name | Actual meaning | Where on the wire |
|---|---|---|
| `service` (u8)        | **backplane slot number** (0..0x13) | CB+0x1C (chip control block) |
| `class_or_misc` (u16) | **per-attempt timeout in ms** (>=26) | CB+0x10, multiplied by 1000 → microseconds |
| `encoded_path` (u8[]) | **full CIP request body**, not just an EPATH | Copied verbatim into UCMM TX buffer |

There is no separate "default target" state on the chip.  The slot
byte is the only routing input; the CIP request bytes are forwarded
as-is.  For off-chassis routing (DH+, EtherNet/IP, ControlNet) embed
an Unconnected_Send (service 0x52) in `cip_request` and put the
route_path inside the embedded message.

The C SDK exposes the corrected names via `bp_message_t`:

```c
uint8_t resp[256];
uint8_t identity_req[] = {
    0x01,                     /* CIP service: Get_Attributes_All */
    0x02,                     /* path_size in words */
    0x20, 0x01, 0x24, 0x01,   /* class 1 (Identity), instance 1 */
};
bp_message_t msg = {
    .slot          = 2,                  /* L85 in slot 2 */
    .cip_request   = identity_req,
    .req_size      = sizeof(identity_req),
    .timeout_ms    = 3000,
    .resp_data     = resp,
    .resp_capacity = sizeof(resp),
};
bp_client_message_send(client, &msg);
/* msg.resp_data now holds:
 *   resp[0] = 0x81  (= req_svc 0x01 | 0x80)
 *   resp[1] = 0
 *   resp[2] = general status (0 on success)
 *   resp[3] = ext status size in words
 *   resp[4..] = vendor_id, device_type, product_code, fw_major, fw_minor,
 *               status, serial, name_len, name[name_len]
 */
```

Path segment reference (for hand-building `cip_request` paths):

| Segment | Wire | Purpose |
|---|---|---|
| Logical 8-bit Class | `0x20, classID` | select class |
| Logical 16-bit Class | `0x21, 0x00, lo, hi` | select class (>= 256) |
| Logical 8-bit Instance | `0x24, instID` | select instance |
| Logical 16-bit Instance | `0x25, 0x00, lo, hi` | select instance (>= 256) |
| Logical 8-bit Attribute | `0x30, attrID` | select attribute |
| Extended Symbolic | `0x91, len, name[len], 0?` | tag name (NUL-pad to word) |

**Empirically verified service support on cm1756, sweeping slots 0..3:**

| `slot` | `cip_request` | Device | Reply |
|---|---|---|---|
| 0 | `01 02 20 01 24 01` | 1756-HIST2G/B | Get_Attributes_All ok |
| 1 | `01 02 20 01 24 01` | 1756-L73/A LOGIX5573 | Get_Attributes_All ok |
| 2 | `01 02 20 01 24 01` | 1756-L85E/B | Get_Attributes_All ok |
| 3 | `01 02 20 01 24 01` | 1756 Embedded Edge Compute (self) | Get_Attributes_All ok |
| 1 | `0e 03 20 01 24 01 30 01` | L73 vendor attr | Get_Attribute_Single ok (reply 0x8e, vendor=0x0001) |
| 4 | `01 02 20 01 24 01` | empty slot | rc=3 (engine refused, no response) |

Range and validity rules:

| Field | Validation | Failure |
|---|---|---|
| `slot` | `< 0x14` (20) | engine rc=1 |
| `req_size` | `1..500` | engine rc=1 (returned before wire) |
| `timeout_ms` | clamped to min 26 | silent; retry budget exhausts → engine rc=14 |
| `resp_capacity` | `> 0` | SDK returns `BP_ERR_NULL_ARG` |

### Connected messaging — wire format

Class-3 connected messaging on cm1756 is implemented by the SDK by
**bypassing the broken `OCXcip_TxRxOpenConn` family** and routing
all three lifecycle calls through `bp_client_message_send` (mbox
0x200, UCMM transport).  The PLC's connection state machine is
driven entirely by the CIP service bytes the SDK sends; the chip's
mailbox transport is irrelevant to whether the PLC considers a
connection open.

Why `OCXcip_TxRx*` is broken on cm1756: Ghidra RE of
`libocxbpapi.so.2.3 @ 0x106f44` → external thunks at `0x134048+`
shows the inner `func_0x0010664c` resolves to
`OCXCN_OpenClass3Connection`, a PLT entry pointing to an
`OCXCN_*` library that is not present anywhere on the cm1756 image.
The `OCXCN_*` thunk family covers `RegisterConnectionObj`,
`OpenClass3Connection`, `CloseClass3Connection`,
`SendClass3Request`, `UnregisterConnectionObj` — the entire
connection-management state machine.  The `OC_bp*` engine in
`libocxbpeng.so.2.3` and the APex2 chip firmware are NOT involved
at the OpenConn dispatch level, so we have no local workaround
through that path.  The SDK keeps the `bp_client_txrx_*` public
API for source-compatibility, but implements it directly against
`bp_client_message_send`.

#### Sequence numbers are NOT prepended

The sibling `historianupdate` apex2d daemon
([`apex2_cip_connection.c:1346-1362`](../../historianupdate/driver/apex2/daemon/apex2_cip_connection.c))
empirically established (2026-03-31, comment in source) that the
APex2 ASIC treats all MBOX_LOOPBACK (0x200) sends identically —
**connected vs UCMM is a CIP protocol distinction, not an ASIC
transport distinction**.  Forward Open establishes PLC-side
context; subsequent CIP reads ride the same UCMM transport.

This was independently confirmed on cm1756 against an L85 in
slot 2 (`c/examples/connexp` v0.7.0 Phase 1 experiment, 2026-05-22):
LFO + bare Identity Get_Attributes_All + Forward_Close all
round-tripped cleanly via three separate `bp_client_message_send`
calls.  Round-trip latency 0.5–1.3 ms per call.

`cip_connected_send` in the sibling explicitly does **not** prepend
sequence numbers to the request body — it increments
`conn->sequence` for diagnostic tracking only.  The standalone
[`apex2_lib.h::apex_send_connected`](../../historianupdate/driver/apex2/apex2_lib.h)
docstring claims it writes CB+0x1E and CB+0x36, but the
implementation never does — line 899 explicitly says "_not used by
ASIC for MBOX_LOOPBACK_".

The SDK matches this convention.  `bp_client_txrx_msg` passes the
caller's request bytes through `bp_client_message_send`
**unmodified**.

#### Wire format

**Large Forward Open** (CIP service `0x5B`).  Reference:
[`apex2_cip_connection.c::build_forward_open` lines 682-820](../../historianupdate/driver/apex2/daemon/apex2_cip_connection.c).
Field positions are little-endian throughout.

```
off len field
+00 1   service = 0x5B
+01 1   path_size_words = 0x02
+02 4   class 6 (CM) inst 1     20 06 24 01
+06 1   priority/tick           0x05
+07 1   timeout ticks           0xF7
+08 4   O→T conn ID hint        originator-chosen, target overrides; OCX uses 0x80010000
+0C 4   T→O conn ID hint        originator-chosen, PLC echoes unchanged; OCX uses 0x80000001 (bit 31 required by L73 fw v21)
+10 2   connection serial       originator-chosen; must be unique within active set
+12 2   vendor ID               0x0001 (Rockwell)
+14 4   originator serial       random — duplicate triggers PLC error
+18 4   timeout multiplier      0x00000003 → RPI × 4 = ~40 s
+1C 4   O→T RPI µs              10_000_000 (= 10 s, matches OCX)
+20 4   O→T conn params         0x42000FA0 = P2P, variable, 4000 B (FO_REQUEST_OT_SIZE in sibling)
+24 4   T→O RPI µs              10_000_000
+28 4   T→O conn params         0x42000FA0
+2C 1   transport trigger       0xA3 (Class 3, server)
+2D 1   conn path_size_words    0x02
+2E 4   conn path               20 02 24 01 (Msg Router class 2 inst 1)
total: 50 bytes
```

**LFO reply** — service `0xDB` (success) or `0xD4` (small FO; not
emitted by L85).  Header = 4 bytes + `resp[3] * 2` (extended status
words; 0 in the success case).

```
off len field                   notes
+00 1   reply service = 0xDB
+01 1   reserved = 0x00
+02 1   general status          0x00 = success
+03 1   ext_status size words   0x00
+04 4   O→T conn ID             PLC-chosen — use as routing tag for connected sends
+08 4   T→O conn ID             echo of our T→O hint
+0C 2   echo of our conn serial
+0E 2   echo of our vendor ID
+10 4   echo of our orig serial
+14 4   actual O→T RPI µs       PLC may negotiate down
+18 4   actual T→O RPI µs
+1C 1   reply size              # words of extra app reply data
+1D 1   reserved
```

Captured against L85 (slot 2, 2026-05-22 from connexp):

```
req (50 B):
  5b 02 20 06 24 01 05 f7 00 00 01 80 01 00 00 80
  a8 a5 01 00 56 80 1d 67 03 00 00 00 80 96 98 00
  a0 0f 00 42 80 96 98 00 a0 0f 00 42 a3 02 20 02
  24 01
                                                   conn_serial = 0xa5a8
                                                   orig_serial = 0x671d8056
                                                   ot_params   = 0x42000fa0
resp (30 B):
  db 00 00 00 35 04 02 80 01 00 00 80 a8 a5 01 00
  56 80 1d 67 80 96 98 00 80 96 98 00 00 00
  ^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^^^
  reply hdr   O→T conn ID T→O conn ID = 0x80000001 (echo)
              = 0x80020435
```

**Forward_Close** (CIP service `0x4E`).  Reference:
[`apex2_cip_connection.c::build_forward_close` lines 1244-1283](../../historianupdate/driver/apex2/daemon/apex2_cip_connection.c).
All four identifier fields must echo the LFO exactly — the PLC
matches against its connection table.

```
off len field                   notes
+00 1   service = 0x4E
+01 1   path_size_words = 0x02
+02 4   class 6 (CM) inst 1     20 06 24 01
+06 1   priority/tick           0x0A
+07 1   timeout ticks           0x0E
+08 2   conn serial             must match LFO
+0A 2   vendor ID               must match LFO
+0C 4   orig serial             must match LFO
+10 1   conn path_size_words    0x02
+11 1   reserved                0x00
+12 4   conn path               20 02 24 01
total: 22 bytes
```

**FC reply** — service `0xCE` (= 0x4E | 0x80).

```
+00 1   reply service = 0xCE
+01 1   reserved
+02 1   general status         0x00 = success; conn fully removed from PLC table
+03 1   ext_status size words
+04 2   echo conn serial
+06 2   echo vendor ID
+08 4   echo orig serial
+0C 1   reply size (words)
+0D 1   reserved
```

Captured FC against L85 (14 B):
`ce 00 00 00 a8 a5 01 00 56 80 1d 67 00 00`

#### Connected sends ride the LFO-established context

After a successful LFO, subsequent CIP requests sent via
`bp_client_message_send` to the same slot reach the PLC and are
processed in the context of the open connection.  The PLC does
**not** require the request body to carry a connection ID or
sequence number for this to work — those fields are part of the
on-wire CPF address item that the chip firmware emits, not part
of the CIP service bytes the host hands to `MessageSend`.

What this means for the SDK:

- `bp_client_txrx_open` builds + sends LFO; caches
  `(conn_serial, vendor_id, orig_serial, ot_conn_id, to_conn_id)`
  for the Forward_Close, plus a per-connection sequence counter
  for diagnostics only.
- `bp_client_txrx_msg` passes the caller's request through
  `bp_client_message_send` byte-for-byte.
- `bp_client_txrx_close` builds + sends Forward_Close using the
  cached identifiers.

#### Known limitation: small-buffer transport only

Routing through `bp_client_message_send` uses chip mailbox `0x200`
(UCMM path), which caps single-frame payloads at ~500 bytes (the
`BP_MSG_MAX_REQ = 500` limit documented above).  The real benefit
of CIP Class 3 connected messaging on this chip family — 4002-byte
packets via mailbox `0x204` with pre-registered transport CBs — is
not reachable through `OCXcip_MessageSend` and requires direct
userland SHRAM access through `/dev/ocx_shram` + `/dev/ocx_cbregs`
(the path the sibling apex2d daemon uses).

This is **v0.8 RE territory**, tracked separately.  For v0.7.0,
TxRx* is functional with the same envelope size as MessageSend.

### Multi-hop routes — Unconnected_Send (service 0x52)

The SDK's `bp_message_t.slot` / `ConnSpec.EncodedPath` field only
addresses **one hop**: chip → backplane slot.  To reach a device
beyond that slot (EtherNet/IP node off the L85, ControlNet node off
a CNB, DH+ node off a DHRIO, etc.) the caller wraps their CIP
request inside an **Unconnected_Send** service (`0x52`) targeting
the **Connection Manager** of the slot device, which then routes
through to the actual destination.  No SDK or wire change is
required — the chip just forwards the bytes the caller assembled.

The Unconnected_Send service is itself a CIP request.  Send it
exactly like any other CIP body — via `bp_client_message_send`
(UCMM transport, mailbox 0x200), via `bp_client_txrx_msg` over a
class-3 connection, or via `bp_client_pool_txrx` from a pool.  The
chip is transport-agnostic and the L85's Connection Manager
accepts Unconnected_Send on both paths.

#### Wire format

The Unconnected_Send body (the bytes the SDK puts into
`bp_message_t.cip_request` or the connected `req` buffer):

```
+0    uint8   service          = 0x52    (Unconnected_Send)
+1    uint8   path_size_words  = 0x02    (request path follows)
+2    uint8[] request_path     = 20 06 24 01  (class 0x06 ConnMgr, instance 1)
+6    uint8   priority_tick    = 0x05    (priority 0, tick = 32 ms)
+7    uint8   timeout_ticks    = ceil(timeout_ms / 32), clamped 1..255
+8    uint16  embedded_msg_sz  (little-endian; byte count of embedded CIP request)
+10   uint8[] embedded_msg     (the target's CIP request — service / path / body)
+L    uint8   pad              (0x00, present iff embedded_msg_sz is odd)
+L'   uint8   route_path_words (count of 16-bit words in route_path)
+L'+1 uint8   reserved         = 0x00
+L'+2 uint8[] route_path       (route_path_words * 2 bytes)
```

`L = 10 + embedded_msg_sz`, `L' = L + (embedded_msg_sz & 1)`.
`route_path` is always word-aligned by construction (`_words * 2`),
so no trailing pad is needed.

#### Route path encoding

Standard CIP port segments.  Each hop is one of:

| Bytes | Meaning |
|---|---|
| `01 NN`               | Port 1 (backplane), link address `NN` (slot number) |
| `02 NN`               | Port 2 (front-side EtherNet/IP, or DH+ on a DHRIO), link address `NN` |
| `0F 00 ext_lo ext_hi` | Extended link address (16-bit) — for nodes with addresses > 255 |
| `12 LL <ascii>`       | Extended link address as ASCII (IP literal, used by EIP routers) |

Concatenate one hop per intermediate device.  Two examples:

```
# Identity on the L85's first EtherNet/IP module (port 2 of the L85,
# device link address 1 on that segment):
embedded_msg = 01 02 20 01 24 01           ; Identity Get_Attributes_All
route_path   = 02 01                       ; one port segment, 1 word
priority/tick = 05, timeout_ticks = 0xFA   ; ~8 s

# Wrapped Unconnected_Send body (sent to slot 2 = L85 ConnMgr):
52 02 20 06 24 01     ; svc 0x52, path = ConnMgr
05 FA                 ; priority/tick + timeout_ticks
06 00                 ; embedded_msg_sz = 6 (LE)
01 02 20 01 24 01     ; embedded msg (6 bytes, even — no pad)
01                    ; route_path_words = 1
00                    ; reserved
02 01                 ; route_path (1 word = 2 bytes)
```

```
# Identity on a remote DH+ node (slot 2 = L85 in chassis,
# port 2 = DH+ side of a DHRIO sitting in slot 3 of a remote chassis,
# DH+ node 5):
embedded_msg = 01 02 20 01 24 01
route_path   = 01 03 02 05                 ; two hops: slot 3 → DH+ node 5
```

#### Helpers

`bp_build_unconnected_send()` (C), `cip.BuildUnconnectedSend()` (Go),
and `bpclient.build_unconnected_send()` (Python) assemble the body
above given `(embedded_msg, route_path, timeout_ms)`.  The reply is
the embedded service's reply (service byte `embedded_svc | 0x80`,
status, body) and parses normally with the existing decoders — no
unwrap step is needed; the L85's ConnMgr returns the routed reply
in the same envelope as a direct reply.

The helpers also expose `parse_unconnected_send_reply()` which is
the no-op identity transform when the call succeeded (`status = 0`)
or surfaces the routing error when the route failed (`status != 0`,
`ext_status` codes in [`docs/error-codes.md`](error-codes.md) —
common ones: `0x01 / 0x0204` "Unconnected_Send timeout", `0x01 /
0x0205` "Parameter error in Unconnected_Send", `0x01 / 0x0311`
"Port not available").

### `OCXcip_DeleteTagDbHandle` — release the tag DB

```
fn_name        = "OCXcip_DeleteTagDbHandle"
payload_size   = 0x80

REQUEST PAYLOAD:
  slot + 0x78  uint32  db_handle

RESPONSE: errorcode only
```

### `OCXcip_AccessTagData` — batched read/write

This is the workhorse. Up to N requests per call (limited by slot
size; comfortably batches 10s of tags).

```
fn_name        = "OCXcip_AccessTagData"
payload_size   = 0x2A0 + (request_count - 1) × 0x120 + total_data_bytes

REQUEST PAYLOAD:
  slot + 0x78   char[255]   path string (same format as CreateTagDbHandle)   "P:1,S:2"
  slot + 0x178  uint16      service code (unused by engine but written)        0
  slot + 0x17A  uint16      request_count                                     1..N

  Per request, stride 0x120, first descriptor at slot + 0x180:
    +0x000  char[256]  tag_name             NUL-terminated, up to 255 bytes
    +0x100  uint16     data_type            CIP type code (e.g. 0xC4 = DINT)
    +0x102  uint16     elem_byte_size       bytes per element (4 for DINT, 1 for BOOL, ...)
    +0x104  uint16     action               1 = read, 2 = write
    +0x106  uint16     elem_count           number of elements (1 for scalar)
    +0x108  uint8      has_extra            0 (we never use the extra-path mode)
    +0x109  uint8[7]   (padding to 8-byte align)
    +0x110  uint64     data_ptr             unused by server for inline data; set to 0
    +0x118  uint32     result               server-written: 0 on success, OCX_ERR_* on failure
    +0x11C  uint32     (padding)

  DATA AREA starts at: slot + 0x180 + request_count × 0x120
                       (i.e. immediately after the last descriptor)

    For each request in order:
      - For ActionWrite: caller writes elem_count × elem_byte_size bytes here
      - For ActionRead:  server writes elem_count × elem_byte_size bytes here

RESPONSE PAYLOAD:
  Same slot, server populates per-request result fields and read data areas.
```

### THE bug-fix that wasn't obvious

The original wrapper header (`include/ocxbpapi.h` in the vendor SDK)
named the fields at `+0x0A` and `+0x0E` as `count` and `elem_size`,
which was inverted. The actual semantics, verified by Phase 2's
write-roundtrip testing:

| Field offset (within descriptor) | Correct meaning | Vendor header (wrong) |
|---|---|---|
| `+0x102` (uint16) | `elem_byte_size` — bytes per element | `count` |
| `+0x104` (uint16) | `action` (1=read, 2=write) | `action` ✓ |
| `+0x106` (uint16) | `elem_count` — number of elements | `elem_size` |

Reads of BOOL tags coincidentally worked under the wrong naming
because `elem_byte_size == elem_count == 1` for a scalar BOOL. The
inversion only bit on writes (data wasn't sent to the PLC) or on
multi-element accesses (truncation or buffer overrun).

## Why `has_extra = 0`

The vendor wrapper has an "extra path" mode where the
`AccessTagData` request can override the per-call path via a
secondary string at the end of the descriptor. We never use it —
setting `has_extra = 1` with a non-NUL extra path crashes the engine
on cm1756 (PROBE: SIGSEGV in libocxbpeng during marshalling). Always
set it to 0 and use the slot-level path field.

## The slot header's `is_docker` byte

Set this to **1** when calling from inside a container. The vendor
engine uses it for logging/diagnostic purposes only — it doesn't
change protocol behavior — but setting it to 0 from a containerized
client has caused weird logging on the server side. Always set it
to 1 from containers; 0 from native userland.

## Quirks that surprise new implementers

1. **`fn_name` is the full `OCXcip_*` string**, not a numeric opcode
   alias. The opcode is always `0x00CA`; the function is dispatched
   by string match on `fn_name`. Misspell it and the server returns
   `OCX_ERR_GENERIC` with no useful message.
2. **`payload_size`** must match the per-opcode value listed above.
   Server doesn't check it but underreporting causes the server to
   skip parts of the payload silently.
3. **Slot-owner field uses TID, not PID**. On Linux, `gettid()`
   (`SYS_gettid`) is needed — `pthread_self()` is a different ID.
4. **Drain semaphores on slot claim AND slot release.** Stale posts
   from a prior request can wake your `sem_timedwait` instantly with
   garbage in the slot. Both Go and the vendor wrapper drain on
   claim; we drain on release too as defense in depth.
5. **Don't reorder the slot header fields.** The server reads them
   in a fixed order. If `errorcode` is set to -301 before the rest
   of the header is in place, the server may pick up the slot mid-
   write.

## Verifying your impl

The smoke test sequence (`tagtest`) every language ships:

1. Open IPC — should succeed in < 50 ms
2. `OCXcip_Open` — should return any nonzero handle in < 5 ms
3. `OCXcip_CreateTagDbHandle("P:1,S:2")` — should return a handle
4. `OCXcip_BuildTagDb` — typically 150–250 ms; returns symbol count
5. `OCXcip_GetSymbolInfo(0..9)` — first symbols on a known PLC
   should match expected names from `validation/expected.yaml`
6. `OCXcip_AccessTagData` ReadDINT("OCX_TEST") — current value V0
7. `OCXcip_AccessTagData` WriteDINT("OCX_TEST", 0xDEADBEEF)
8. ReadDINT again — expect 0xDEADBEEF
9. Restore V0
10. Final read — expect V0

If your impl produces identical output to the C tagtest, you've
implemented the protocol correctly.

## References

- POSIX shared memory: [`shm_overview(7)`](https://man7.org/linux/man-pages/man7/shm_overview.7.html)
- POSIX named semaphores: [`sem_overview(7)`](https://man7.org/linux/man-pages/man7/sem_overview.7.html)
- The RE source repo:
  [github.com/complacentsee/rockwell-bpgateway-re](https://github.com/complacentsee/rockwell-bpgateway-re/)
- Specifically the spec-equivalent header: that repo's
  [`include/ocxbpapi_shm.h`](https://github.com/complacentsee/rockwell-bpgateway-re/blob/main/include/ocxbpapi_shm.h)
