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
+0x6C  uint32     field1           often elem_byte_size for arrays
+0x70  uint32     field2           array dim
+0x74  uint32     field3           array dim
+0x78  uint32     instance_id      symbol instance id on the PLC
+0x7C  uint16     flags            (bit 0 set: "alias"; other bits undocumented)
+0x7E  uint16     (padding)
```

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
