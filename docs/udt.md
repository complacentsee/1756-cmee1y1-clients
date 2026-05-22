# UDT (User-Defined Type) support

This SDK fully supports reading and writing Logix UDTs ("structs"
in CIP terminology). Three independent access patterns work today:

| Approach | Suitable for | API |
|----------|--------------|-----|
| **Schema discovery + dotted member access** | UDTs whose layout you don't know at compile-time | `bp_tagdb_get_struct_info` + `bp_tagdb_get_struct_member` then `bp_tagdb_read_*` with `parent.member` tag names |
| **Direct dotted member access** | UDTs whose layout you DO know | `bp_tagdb_read_dint(db, "MyTag.Field", ...)` etc. |
| **Whole-struct raw bytes** | When you know the byte layout and want to skip per-member round-trips | `bp_tagdb_access` with `data_type = struct's wire-level data_type` (e.g. `0x4527`), `elem_byte_size = struct_size`, `elem_count = 1`. Note: this requires the engine + PLC to agree on the struct template handle — works for some PLC firmwares, returns CIP error on others. The schema-discovery path is the portable choice. |

## The schema-discovery flow

Symbol enumeration already tells you whether a tag is a UDT
(`bp_symbol_is_struct(info)` → 1) and which template it uses
(`info.struct_type`). To get the template's layout, call
`bp_tagdb_get_struct_info` followed by `bp_tagdb_get_struct_member`
for each member index:

```c
bp_symbol_info_t  sym;
bp_tagdb_symbol_at(db, idx, &sym);
if (bp_symbol_is_struct(&sym)) {
    bp_struct_info_t s;
    bp_tagdb_get_struct_info(db, sym.struct_type, &s);
    printf("UDT '%s' is %u bytes with %u members\n",
           s.name, s.byte_size, s.n_members);
    for (uint32_t i = 0; i < s.n_members; i++) {
        bp_struct_member_info_t m;
        bp_tagdb_get_struct_member(db, sym.struct_type, i, &m);
        printf("  %s @ +%u  size=%u  flags=0x%02x\n",
               m.name, m.offset, m.byte_size, m.flags);
        if (bp_member_is_struct(&m)) {
            /* recurse with m.struct_id */
        }
    }
}
```

Then to actually read a member's value, build the dotted tag name
and call the matching atomic-type helper:

```c
int32_t fcode;
bp_tagdb_read_dint(db, "MyUDT.FCODE", &fcode);

char label[100]; size_t label_len;
bp_tagdb_read_string(db, "MyUDT.FLABEL_ID", label, sizeof(label), &label_len);

/* Element of an array of UDTs: */
int32_t f;
bp_tagdb_read_dint(db, "MyUDT_Array[5].FCODE", &f);
```

Writing UDT members works identically — `bp_tagdb_write_dint`,
`bp_tagdb_write_string`, etc. with the dotted tag name. All atomic
helpers accept dotted paths transparently.

## Worked example: walking `Tran_To_iSeries_FIFO_Loader`

Captured from `examples/udtinfo` running against a real L85E
firmware ~36.11:

```
struct 'Tran_FROM_Oldi_to_iSeries'  id=0x23  data_type=0x4527  size=104 bytes  members=5
  [0] FCODE      @+0    type=DINT     size=4              flags=0x45
  [1] FLABEL_ID  @+16   type=STRUCT   size=88  struct_id=0x21  flags=0x41
    struct 'STRING'  id=0x21  data_type=0x0fce  size=88 bytes  members=2
      [0] DATA   @+4    type=SINT     size=1[82]              flags=0x49
      [1] LEN    @+0    type=DINT     size=4                  flags=0x45
  [2] FSEQ       @+12   type=DINT     size=4                  flags=0x45
  [3] FSTAT      @+4    type=DINT     size=4                  flags=0x45
  [4] FSTATION   @+8    type=DINT     size=4                  flags=0x45
```

(Member indices are PLC-defined ordering — not necessarily the
order members appear in Studio 5000's editor.)

So `Tran_To_iSeries_FIFO_Loader` has five reachable accessors:

```c
bp_tagdb_read_dint  (db, "Tran_To_iSeries_FIFO_Loader.FCODE",     &v);
bp_tagdb_read_dint  (db, "Tran_To_iSeries_FIFO_Loader.FSTAT",     &v);
bp_tagdb_read_dint  (db, "Tran_To_iSeries_FIFO_Loader.FSTATION",  &v);
bp_tagdb_read_dint  (db, "Tran_To_iSeries_FIFO_Loader.FSEQ",      &v);
bp_tagdb_read_string(db, "Tran_To_iSeries_FIFO_Loader.FLABEL_ID",
                      buf, sizeof(buf), &len);
```

Verified live against the lab L85E: all four DINT members
round-trip clean; the STRING member round-trips clean
('INSDRV-1779328111-44' → 'BPCLIENT_UDT_TEST' → restored).

## Flag byte interpretation

The single flag byte at the end of each `bp_struct_member_info_t`
encodes:

| Value | Meaning | Examples |
|-------|---------|----------|
| `0x45` | atomic scalar | DINT, REAL, BOOL, INT members |
| `0x41` | struct member | Nested UDT (look at `struct_id`) |
| `0x49` | atomic array | `SINT[82]` member of `STRING` |

Bit 3 (`0x08`) is set when the member is an array. Use
`bp_member_is_array(&m)` as a convenience.

The high nibble's `0x40` bit is set in all observed cases — possibly
the "this is a valid member" indicator.

## Nested UDTs

When `m.struct_id != 0`, the member is itself a UDT. Recurse with
that struct_id:

```c
bp_struct_info_t nested;
bp_tagdb_get_struct_info(db, m.struct_id, &nested);
/* enumerate nested.n_members the same way */
```

The recursion is what lets `udtinfo` print the `STRING` UDT
inline under the `FLABEL_ID` member above.

## Whole-struct reads (the gotcha)

There IS a wire-protocol path for reading a whole struct's bytes
in one round-trip:

```c
bp_tag_request_t r = {
    .tag_name = "MyUDT_Tag",
    .data_type = 0x4527,         /* the UDT's wire data_type */
    .elem_byte_size = 104,        /* the struct's byte_size */
    .action = BP_ACTION_READ,
    .elem_count = 1,
    .data = my_buffer,
};
bp_tagdb_access(db, &r, 1);
```

In practice, this **returns a CIP General Status error**
(`r.result = 0x09`) on the firmware we tested against. The engine
sends the request to the PLC; the PLC requires a per-type
template-handle / CRC match that our request doesn't supply
correctly.

**Recommendation: stick with the schema-discovery + dotted access
flow.** It's portable across firmware variants and gives you
free type-awareness. Per-member round-trips do cost more for
heavily-nested UDTs, so batch when possible using
`bp_tagdb_access` with multiple `bp_tag_request_t` entries.

## What we don't (yet) support

- **Multi-dimensional arrays within UDT members** — the SDK
  surfaces only the first array dim (`array_count`). The bytes at
  member-info offset `+0x3C` are zero in every tested case; they
  might encode a second dim or might be reserved.
- **UDT struct template enumeration without a tag** — you need a
  symbol's `struct_type` to start; we don't expose a way to walk
  ALL struct templates the PLC defines. (Workaround: enumerate
  all tags and collect unique `struct_type` values.)
- **Schema-aware whole-struct read** — see the gotcha above. The
  per-member path is the supported one.

## Worked example program

`c/examples/udtinfo.c` is the canonical demonstration. Run it
against any UDT-typed tag to dump its schema (and nested UDTs up
to two levels deep):

```sh
udtinfo --path P:1,S:2 --tag Tran_To_iSeries_FIFO_Loader
udtinfo --path P:1,S:2 --struct-id 0x23
```
