# UDT (User-Defined Type) support

This SDK fully supports reading and writing Logix UDTs ("structs"
in CIP terminology). Three independent access patterns work today:

| Approach | Suitable for | API |
|----------|--------------|-----|
| **Schema discovery + dotted member access** (recommended) | UDTs whose layout you don't know at compile-time | `bp_tagdb_get_struct_info` + `bp_tagdb_get_struct_member` then `bp_tagdb_read_*` with `parent.member` tag names |
| **Direct dotted member access** | UDTs whose layout you DO know | `bp_tagdb_read_dint(db, "MyTag.Field", ...)` etc. |
| **Whole-struct raw bytes** | Rare cases; see "Gotcha" below | `bp_tagdb_access` with `data_type = struct's wire-level data_type`, `elem_byte_size = struct_size`, `elem_count = 1` |

## The schema-discovery flow

Symbol enumeration tells you whether a tag is a UDT
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
            /* recurse with m.struct_id to enumerate nested UDT members */
        }
    }
}
```

Then to read a member's value, build the dotted tag name and call
the matching atomic-type helper:

```c
int32_t v;
bp_tagdb_read_dint(db, "MyUDT.SomeIntField", &v);

char buf[100]; size_t len;
bp_tagdb_read_string(db, "MyUDT.SomeStringField", buf, sizeof(buf), &len);

/* Element of an array of UDTs: */
bp_tagdb_read_dint(db, "MyUDT_Array[5].SomeIntField", &v);
```

Writing UDT members works identically — `bp_tagdb_write_dint`,
`bp_tagdb_write_string`, etc. with the dotted tag name. All atomic
helpers accept dotted paths transparently.

## Worked example: the AB STRING UDT

The Logix STRING type is itself a UDT (template id `0x21` on most
firmwares), so it's a universally-available example. Running
`udtinfo --struct-id 0x21` produces:

```
struct 'STRING'  id=0x21  data_type=0x0fce  size=88 bytes  members=2
  [0] DATA   @+4   type=SINT     size=1[82]            flags=0x49
  [1] LEN    @+0   type=DINT     size=4                flags=0x45
```

So a `STRING` is 88 bytes total: 4 bytes for `LEN` (DINT) + 82
bytes for `DATA` (SINT[82]) + 2 bytes of trailing padding. The
flag byte `0x49` on `DATA` indicates "atomic array".

The same flow works for custom UDTs you define in Studio 5000.
Pass the tag name to `udtinfo --tag MyTag` and it'll look up the
struct_id from the symbol enumeration and walk the template.

## Flag byte interpretation

The single flag byte at the end of each `bp_struct_member_info_t`
encodes:

| Value | Meaning | Examples |
|-------|---------|----------|
| `0x45` | atomic scalar | DINT, REAL, BOOL, INT members |
| `0x41` | struct member | nested UDT (look at `struct_id`) |
| `0x49` | atomic array | `SINT[82]` member of `STRING` |

Bit `0x08` is set when the member is an array. Use
`bp_member_is_array(&m)` as a convenience. Bit `0x40` is set in all
observed cases — likely the "valid member" indicator.

## Nested UDTs

When `m.struct_id != 0`, the member is itself a UDT. Recurse with
that struct_id:

```c
bp_struct_info_t nested;
bp_tagdb_get_struct_info(db, m.struct_id, &nested);
/* enumerate nested.n_members the same way */
```

Logix UDTs can nest arbitrarily deep. The `examples/udtinfo.c`
program recurses up to depth 2 for readability; raise the depth
limit if you have deeper hierarchies.

## Multi-dimensional arrays

Logix supports 1-D, 2-D, and 3-D array tags. Tag-level arrays
expose their shape via `bp_symbol_info_t`:

```c
bp_symbol_info_t info;
bp_tagdb_symbol_at(db, idx, &info);
/* For DINT[5,10,30]:    info.dim0 = 5, info.dim1 = 10, info.dim2 = 30, rank = 3 */
/* For DINT[5,3]:        info.dim0 = 5, info.dim1 = 3,  info.dim2 = 0,  rank = 2 */
/* For DINT[10]:         info.dim0 = 10, info.dim1 = 0, info.dim2 = 0,  rank = 1 */
/* For scalar DINT:      info.dim0 = 0,  info.dim1 = 0, info.dim2 = 0,  rank = 0 */

int  r = bp_symbol_rank(&info);              /* 0/1/2/3 */
uint32_t n = bp_symbol_total_elements(&info); /* dim0 * dim1 * dim2 (zero dims treated as 1) */
```

**Element addressing uses comma-separated indices** in the tag
name string — Logix native convention:

```c
int32_t v;
bp_tagdb_read_dint (db, "MyDintArr2D[2,1]", &v);              /* 2-D single element */
bp_tagdb_write_dint(db, "MyDintArr2D[4,2]", 42);

/* Read a row slice (1-D access starting at [2,0]): */
int32_t row[3];
bp_tagdb_read_dint_array(db, "MyDintArr2D[2,0]", row, 3);
```

The batched array helpers (`bp_tagdb_read_dint_array` etc.) work
with multi-dim tags because the PLC returns elements in **row-major
linear order** — `[i,j]` is at linear index `i * dim1 + j`. So
reading the entire `DINT[5,3]` is one call:

```c
int32_t all[15];
bp_tagdb_read_dint_array(db, "MyDintArr2D[0,0]", all, 15);
/* all[i*3 + j] is element [i,j] */
```

**3-D arrays at the tag level are fully introspected** — `dim2` at
slot offset `+0x78` of the symbol-info struct.  Element addressing
uses three-comma-separated indices (e.g. `"MyArr[2,3,1]"`); the
PLC returns elements in **row-major linear order**, so reading the
whole array is one batched call starting from `[0,0,0]` with
`elem_count = dim0 * dim1 * dim2`.

**Multi-dim arrays as UDT members** likewise aren't fully
introspected — `bp_struct_member_info_t.array_count` holds the
first array dim, and the bytes at member-info `+0x3C` are zero in
all currently-observed cases. If you have a UDT with a `DINT[N,M]`
member, again — sample appreciated.

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

In practice this **returns a CIP General Status error**
(`r.result = 0x09`) on the firmwares we tested against. The engine
sends the request to the PLC; the PLC requires a per-type
template-handle / CRC match that our request doesn't supply
correctly.

**Recommendation: stick with the schema-discovery + dotted access
flow.** It's portable across firmware variants and gives you free
type-awareness. Per-member round-trips do cost more for heavily-
nested UDTs, so batch when possible using `bp_tagdb_access` with
multiple `bp_tag_request_t` entries.

## Worked example program

`c/examples/udtinfo.c` is the canonical demonstration. Run it
against any UDT-typed tag to dump its schema (and nested UDTs up
to two levels deep):

```sh
udtinfo --path P:1,S:2 --tag MyCustomUDTTag
udtinfo --path P:1,S:2 --struct-id 0x21    # the AB STRING UDT
```

## What's not (yet) supported

- **Multi-dim arrays as UDT members** — `array_count` is the first
  dim only
- **Whole-struct one-shot read** — see "Gotcha" above
- **UDT struct-template enumeration without a tag** — you need a
  symbol's `struct_type` to start; we don't expose a way to walk
  ALL struct templates the PLC defines. Workaround: enumerate all
  tags and collect unique `struct_type` values.
