# `OCXcip_AccessTagDataDb` — RE notes (v0.10.4 Phase 1)

This is the pre-implementation RE writeup for the
`OCXcip_AccessTagDataDb` opcode. It supersedes the wire-format
sketch in the v0.10.4 plan where the two diverge — when the
implementation lands, `docs/protocol.md` becomes the canonical
spec and this file is preserved as the trace.

## Source

- `libocxbpapi-w.so` (the SHM wrapper our SDK replaces):
  - `OCXcip_AccessTagDataDb` @ `0x0010DEE0` — the dispatcher we
    decoded.
  - `_Z9GetWrMaskjPv` (`GetWrMask(uint, void*)`) @ `0x0010CF40` —
    resolved target of the PLT stub at `0x001041F0`, called per-
    request to populate descriptor `+0x118`.
- `libocxbpapi.so.2.3` (engine-side, dlopen'd by bpServer):
  - `OCXcip_AccessTagDataDb` @ `0x0010DCA4` — the engine-side
    implementation. Validates against the cached tag DB, supports
    fragmentation for large arrays, walks BIT-class types via
    `(uStack_e4 == 0xc1 && (... & 2) != 0)` branches that consume
    the descriptor's `+0x118` mask seed. Not consumed by our SDK
    directly but useful for understanding which fields the engine
    actually reads.

## Wire format (verified)

Opcode `0xCA`, identical to `OCXcip_AccessTagData`. Dispatch is
by `fn_name` string match in the slot header.

```text
fn_name      = "OCXcip_AccessTagDataDb"  (22 chars + NUL, NUL-pad to 63)
payload_size = 0x1B0 + (count - 1) * 0x128 + total_data_bytes
                                 ^             ^
                                 |             sum of elem_byte_size * elem_count
                                 per-request descriptor stride

slot + 0x078  uint32   db_handle             from CreateTagDbHandle
slot + 0x07C  uint8    has_extra             1 if caller passed an opt_inout pointer; 0 otherwise
slot + 0x07D  uint8    (pad)
slot + 0x07E  uint16   opt_value             when has_extra=1, this is *opt_inout on input;
                                              server writes back to this slot on reply
slot + 0x080  uint16   count                 number of per-request descriptors
slot + 0x082  uint16   (pad)
slot + 0x084  uint32   (pad)
slot + 0x088  per-request descriptors, stride 0x128 bytes:

    descriptor + 0x000  char[255] tag_name        NUL-padded
    descriptor + 0x0FF  uint8     zero_term       always 0 (wrapper writes 0)
    descriptor + 0x100  uint16    action          BP_ACTION_READ (1) / BP_ACTION_WRITE (2)
    descriptor + 0x102  uint16    (pad)
    descriptor + 0x104  uint32    data_type       CIP type code (e.g. 0xC4 = DINT)
    descriptor + 0x108  uint32    elem_byte_size  bytes per element
    descriptor + 0x10C  uint32    elem_count      number of elements
    descriptor + 0x110  uint8     has_data        1 if caller passed a data pointer (sets mask_seed)
    descriptor + 0x111  uint8[7]  (pad)
    descriptor + 0x118  uint64    mask_seed       see "+0x118 mask_seed" below
    descriptor + 0x120  uint32    result          OUT: per-request CIP General Status
    descriptor + 0x124  uint32    (pad)

slot + (0x88 + count * 0x128) ..  data area  concatenated per-request buffers
                                                  writes: caller → slot
                                                  reads:  slot   → caller

Engine validates payload_size <= 0x4B000; over → returns -311.
```

## What changed vs `OCXcip_AccessTagData`

- Slot header `+0x78`: 255-byte path string → 4-byte
  `db_handle`. The engine looks up the cached tag DB by handle
  instead of re-parsing the path. Plus 251 bytes saved per call.
- Slot header `+0x7C..0x7F`: previously unused → `has_extra` +
  `opt_value` (semantics TBD; see "Open question: opt_value").
- Slot header `+0x80`: previously the start of the descriptor
  array (which began at `+0x180` for AccessTagData) → `count`
  field, with descriptors starting at `+0x88` instead.
- Descriptor stride: **0x120 → 0x128 (+8 bytes)**. The extra 8
  bytes accommodate `mask_seed` at `+0x118`.
- Descriptor field layout is **reorganized**, not just shifted:

  | Offset | `AccessTagData` (stride 0x120) | `AccessTagDataDb` (stride 0x128) |
  |---|---|---|
  | +0x100 | `data_type` (u16)              | `action` (u16) |
  | +0x102 | `elem_byte_size` (u16)         | (pad) |
  | +0x104 | `action` (u16)                 | `data_type` (u32) |
  | +0x106 | `elem_count` (u16)             | (in u32 above) |
  | +0x108 | `has_extra` (u8) + pad         | `elem_byte_size` (u32) |
  | +0x10C | (pad)                          | `elem_count` (u32) |
  | +0x110 | `data_ptr` (u64)               | `has_data` (u8) + pad |
  | +0x118 | `result` (u32)                 | `mask_seed` (u64) |
  | +0x11C | (pad)                          | (still in mask_seed) |
  | +0x120 | (end / next desc starts here)  | `result` (u32) |
  | +0x124 | —                              | (pad) |
  | +0x128 | —                              | (end / next desc) |

  An `accessdb.c` encoder must **not** be a copy-paste of
  `access.c` — the field offsets and widths differ.

## The `+0x118` field is a write-mask seed, not a type cookie

The v0.10.4 plan called this field `type_cookie` and named
`func_0x001041F0` as a static type-descriptor producer. The RE
disagrees:

1. `func_0x001041F0` in `libocxbpapi-w.so` is a PLT stub. The
   GOT entry at `0x12FBC8` resolves to a local symbol in the
   same library: `_Z9GetWrMaskjPv` = `GetWrMask(unsigned int,
   void *)` at `0x0010CF40`. It is not an external import.

2. `GetWrMask` is a small switch on the data type that returns
   the first 1/2/4/8 bytes of `*data_ptr` zero-extended to
   `uint64`. The CIP type → element width mapping:

   ```c
   /* GetWrMask(uint type, void *data) — paraphrased */
   switch (type) {
   case 0xC2: case 0xC6: case 0xD1:   return *(uint8_t *)data;
   case 0xC3: case 0xC7: case 0xD2:   return *(uint16_t *)data;
   case 0xC4: case 0xC8: case 0xD3:   return *(uint32_t *)data;
   case 0xC5: case 0xC9: case 0xD4:   return *(uint64_t *)data;
   default:                           return 0;
   }
   ```

   It is a function of `(type, *data)`, not `(type)` alone.
   Ghidra's decompile of the OEM wrapper hides the second arg
   because `x1` is already live with `request->data_ptr` from a
   `cbnz x1, ...` check several instructions earlier; the real
   `bl 0x001041F0` at `0x10E078` passes both registers.

3. The OEM wrapper only writes `+0x118` when the caller's
   request struct has a non-NULL data pointer (offset `+0x20`
   in the user request struct):

   ```c
   if (request->data == NULL) {                    /* +0x20 */
       slot_desc[i].mask_seed = 0;                 /* +0x118 */
       slot_desc[i].has_data  = 0;                 /* +0x110 */
   } else {
       slot_desc[i].mask_seed = GetWrMask(type, request->data);
       slot_desc[i].has_data  = 1;
   }
   ```

   Note that even when `has_data == 1`, the wrapper STILL copies
   `request->data` bytewise into the slot data area via
   `memcpy(slot + data_off, request->data, nbytes)` later in the
   loop — exactly the same as `AccessTagData`. So `has_data=1`
   is not "use the data pointer instead of the data area"; it's
   "I happen to also have the first element prefetched in
   `mask_seed` for the engine's convenience."

4. Plausible purpose: the engine-side AccessTagDataDb in
   `libocxbpapi.so.2.3` has dedicated branches for BIT-class
   writes (CIP type `0xC1` and BIT_STRING variants `0xD1..0xD4`)
   that issue a write-with-mask CIP service. For those, the
   engine needs the new-bit-values mask, which fits naturally
   in a u64 for any BIT_STRING up to 64 bits wide. `mask_seed`
   gives the engine that mask without dereferencing into the
   variable-length data area. For non-BIT types, the engine
   should be free to ignore `+0x118` and read from the data
   area as it does for `AccessTagData`.

## SDK policy

Set:

```
descriptor + 0x110  has_data   = 0
descriptor + 0x118  mask_seed  = 0
```

…always. Rationale:

- Our SDK has no userspace data pointer model. Caller data is
  always marshalled inline into the slot data area before the
  call (just like `AccessTagData`).
- The OEM wrapper's `data_ptr == NULL` branch already sets both
  fields to 0, so we're mirroring a code path the engine has
  always handled correctly.
- For BIT-class writes specifically, the v0.10.4 implementation
  will rely on the data-area path the existing `AccessTagData`
  uses (bp_tagdb_{read,write}_bool / bool_array in `access.c`
  already pack BIT_ARRAY into u32 chunks and the engine reads
  them from the data area). No new mask-seed encoding required.

Phase 1's live probe confirms this works for the cases we care
about (DINT, REAL, BOOL, BOOL[]). If the engine rejects
`mask_seed=0` for some BIT-write subcase, we can fall back to
populating it from the data area's first 1/2/4/8 bytes —
trivial since `accessdb.c` already has that buffer staged.

## Open question: `opt_value`

The slot bytes at `+0x7C..+0x7F` (`has_extra`, `opt_value`)
correspond to the OEM wrapper's `param_5` pointer — the caller
passes `*param_5` IN and the engine writes back to `*param_5`
on reply. The OEM wrapper logic is:

```c
if (param_5 == NULL) {
    slot[0x7C] = 0;     /* has_extra */
    slot[0x7E] = 0;     /* opt_value */
} else {
    slot[0x7C] = 1;
    slot[0x7E] = *param_5;
}
/* ... call engine ... */
if (param_5 != NULL) {
    *param_5 = slot[0x7E];  /* read back */
}
```

What the engine USES `opt_value` for is not visible in
libocxbpapi-w.so (which just shuttles the value through). The
engine-side dispatcher in libocxbpapi.so.2.3 takes a 5th
parameter `param_5` that is later passed to `func_0x0010d328`
and stored at offset `+0x7D6` of the internal request struct;
its consumption is not on a hot path visible in the static
decompile.

**Phase 1 probe plan**: send identical DINT reads with
`opt_inout = NULL` vs `opt_inout = &uint16_t{0}` vs
`opt_inout = &uint16_t{0xFFFF}`. Log the readback in all three
cases. If readback is always 0 (or echoes the input), it's a
no-op; if it varies with the request, document the semantics.

## SDK policy on `opt_value` for v0.10.4

Set `has_extra=0`, `opt_value=0` in the public API. Don't
expose `opt_inout` until Phase 1 characterization gives us a
useful semantic to wrap. This matches the OEM wrapper's
`param_5 == NULL` path, which we know is well-behaved.

## Diagnostic CLI: `accessdb_probe`

`c/examples/accessdb_probe.c` (to be built in Phase 1) exercises:

1. **Default-zero probe** — single DINT read with
   `has_data=0`, `mask_seed=0`, `has_extra=0`. Pass criterion:
   returns the same value `bp_tagdb_read_dint` returns via
   `OCXcip_AccessTagData`. This is the must-pass gate before
   any Phase 2 work.

2. **Non-zero mask_seed probe** — single DINT read with
   `has_data=0` but `mask_seed=0xDEADBEEFCAFEBABE`. Pass
   criterion: value still matches; engine ignores mask_seed
   when `has_data=0`. Confirms hypothesis (a) — the OEM
   wrapper's `data_ptr=NULL` branch is the engine's "no extra
   info" path.

3. **`mask_seed` matters for BIT writes** — write to a single
   BOOL with `has_data=0` + `mask_seed=0` and verify it
   succeeds (or fails coherently). If this fails but the
   `AccessTagData` BOOL write works, then `mask_seed`/`has_data`
   ARE required for BIT writes and we need the fallback
   encoder.

4. **`opt_inout` probes** — three reads identical in every
   field except (NULL, &0, &0xFFFF) for `opt_inout`. Log the
   readback in all three cases.

If any of 1–3 fail in unexpected ways, the failure modes get
recorded back into this doc before Phase 2 begins.

## Implementation hints for Phase 2

When writing `c/src/accessdb.c`:

- Header `payload_size` at `slot+4`: write `0x1B0` initially, then
  patch to `0x1B0 + (count-1)*0x128 + total_data_bytes` after the
  per-request loop has summed sizes (matches OEM wrapper at
  `0x10E1C0: str w20, [x0, #0x4]`).
- Descriptor field stores must be **u32** for data_type,
  elem_byte_size, elem_count (not u16 as in `access.c`).
  Functionally equivalent on the wire — CIP type codes and
  sizes fit in u16 — but matches what the OEM wrapper writes.
- `action` stays u16 at `+0x100`.
- Always set descriptor `+0x111..+0x117` (pad after has_data) to
  zero. The OEM wrapper zeroes the descriptor with `memset`
  before populating fields.
- Per-request `result` is at `+0x120` in the descriptor (not
  `+0x118` as it was for AccessTagData).
- The data area copy direction and stride model is identical to
  `access.c`'s `access_fill` / `access_read` — just lifted to
  the new descriptor stride and result offset.

## Verification gate before Phase 2 lands

`accessdb_probe` against the L85 must show:
- Step 1 reads the same DINT value as `bp_tagdb_read_dint` does
  through the existing `AccessTagData` path.
- Step 2 reads the same DINT value (mask_seed ignored).
- Step 4's readbacks are logged with no engine crash.

If those three pass, the field semantics in this doc are
trustworthy enough to write `accessdb.c` against them. Anything
unexpected goes into a "Findings" section appended to this file
before Phase 2 begins.
