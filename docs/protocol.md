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

### Connected messaging — open issues

The `OCXcip_TxRxOpenConn` / `OCXcip_TxRxMsg` / `OCXcip_TxRxCloseConn`
APIs are documented for completeness but **do not work on cm1756**.

Symptom: `OCXcip_TxRxOpenConn` always returns rc=`0x1001` (4097)
regardless of `(slot, encoded_path, conn_params, app_handle)`.

Cause (Ghidra RE of libocxbpapi.so.2.3 @ 0x106f44 →
external thunks at 0x134048+): the inner `func_0x0010664c` resolves
to `OCXCN_OpenClass3Connection`, a PLT entry pointing to an
`OCXCN_*` library that is not present anywhere on the cm1756 image
we have.  The `OCXCN_*` thunk family covers `RegisterConnectionObj`,
`OpenClass3Connection`, `CloseClass3Connection`, `SendClass3Request`,
`UnregisterConnectionObj` — the entire connection-management
state machine.  The `OC_bp*` engine in libocxbpeng.so.2.3 and the
APex2 chip firmware are NOT involved at the OpenConn dispatch
level, so we have no local workaround through that path.

Workaround that does work today (verified 2026-05-21 on L85, slot 2):

1. Build a Large Forward Open (CIP service `0x5B`) request body —
   borrow the encoding from
   [historianupdate `apex2_cip_connection.c::build_forward_open`](../../historianupdate/driver/apex2/daemon/apex2_cip_connection.c).
2. Send it via `bp_client_message_send` with the target slot.
3. Parse the response — reply service `0xDB`, general status `0x00`,
   body contains the PLC-chosen O→T connection ID and the echoed
   T→O connection ID.

Captured 2026-05-21 against L85 (slot 2):

```
slot=2 timeout_ms=5000
req=5b 02 20 06 24 01 05 f7 00 00 01 80 01 00 00 80 34 12 01 00
    ef be ad de 03 00 00 00 80 96 98 00 00 00 00 42 80 96 98 00
    00 00 00 42 a3 02 20 02 24 01   (50 bytes, Large Forward Open)

resp (30 bytes):
db 00 00 00 2f 04 02 80 01 00 00 80 34 12 01 00
ef be ad de 80 96 98 00 80 96 98 00 00 00
^^^^^^^^^^^ ^^^^^^^^^^^ ^^^^^^^^^^^
reply hdr   O→T conn ID T→O conn ID (= 0x80000001 echo'd)
                = 0x8002042f
```

Whether subsequent **connected** sends/receives work via the same
UCMM transport path remains to be tested.  The sibling apex2d
daemon (`apex2_cip_connection.c::cip_connected_send`) reports that
on the same chip family CB+0x1D=0x0D (UCMM round-trip) carries
connected payloads transparently — the PLC sees the connection
because it has already accepted the Forward Open and routes by
the embedded connection ID — but this requires sequence-number
prepending and is beyond the scope of the current SDK.

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
