# CIP atomic type codes

The codes used in the `data_type` field of `ocx_symbol_info_t` and
the `data_type` field of an `OCXcip_AccessTagData` request descriptor.

Each implementation should expose these as constants in its idiomatic
namespace (e.g. C: `BP_TYPE_DINT`, Go: `ocxbp.TypeDINT`, Python:
`bpclient.TYPE_DINT`).

## Atomic scalar types (low 13 bits significant)

| CIP code | Name | Size | Range / encoding |
|---|---|---|---|
| `0xC1` | BOOL  | 1 byte | 0 = false, 1 = true (top 7 bits ignored on read, written 0) |
| `0xC2` | SINT  | 1 byte | signed 8-bit, two's complement |
| `0xC3` | INT   | 2 bytes | signed 16-bit, little-endian |
| `0xC4` | DINT  | 4 bytes | signed 32-bit, little-endian |
| `0xC5` | LINT  | 8 bytes | signed 64-bit, little-endian |
| `0xC6` | USINT | 1 byte | unsigned 8-bit |
| `0xC7` | UINT  | 2 bytes | unsigned 16-bit, little-endian |
| `0xC8` | UDINT | 4 bytes | unsigned 32-bit, little-endian |
| `0xC9` | ULINT | 8 bytes | unsigned 64-bit, little-endian |
| `0xCA` | REAL  | 4 bytes | IEEE 754 single-precision, little-endian |
| `0xCB` | LREAL | 8 bytes | IEEE 754 double-precision, little-endian |

## Structured types

| CIP code | Name | Notes |
|---|---|---|
| `0xCC` | STRING | Counted Allen-Bradley-style string. Layout: `int32 length; char data[82];` (size = 86 bytes). |
| `0xDA` (high bit) | STRING family | Various length variants. Reading these via `OCXcip_AccessTagData` is supported but interpretation is up to the caller. |

## Symbol-info `data_type` field nuances

The 16-bit `data_type` field in `ocx_symbol_info_t` (slot offset
`+0x64` within the symbol info struct) encodes more than just the
type code:

```
Bits 0-12 (low 13 bits):   CIP type code (table above)
Bit 13 (0x2000):           array flag
Bits 14-15:                reserved (0)
```

So **always mask with `0x1FFF`** when extracting the type code from
a symbol descriptor. The classic example:

- `0x00C4` = scalar DINT
- `0x20C4` = DINT array
- `0x0F81` = struct type (the high bits encode the struct table
  index; see `struct_type` field at `+0x68` for the actual definition)

## Detecting arrays

**`field2 != 0` means it's an array** (empirical: tested against
1756-L85E firmware ~36.11 via the cm1756). The legacy
`data_type & 0x2000` array-flag-bit is NOT set on these enumerations.

```
field2 (slot offset +0x70)   = dim0  (dimension 0 size)
field3 (slot offset +0x74)   = dim1  (or 0 if rank < 2)
field4 (slot offset +0x78)   = dim2  (or 0 if rank < 3)
```

3-D Logix arrays carry `dim2` at slot offset `+0x78` of the
symbol-info struct.  Verified against `Test_DINT_3D : DINT[5,10,30]`
on L85E firmware ~36.11.  (Earlier docs misidentified this slot as
`instance_id` because only 1-D / 2-D test tags were available, and
those always show 0 there.)

For your `OCXcip_AccessTagData` call when reading an array element
range, use `elem_count = N` where N is the number of elements you
want to read. Set `elem_byte_size` to the per-element byte size
from the table above (DINT array: `elem_byte_size = 4`).

## Element counts for batched access

The total transfer per request is `elem_count × elem_byte_size`
bytes. For a single scalar:

| Type  | elem_byte_size | elem_count |
|---|---|---|
| BOOL  | 1 | 1 |
| DINT  | 4 | 1 |
| REAL  | 4 | 1 |
| LREAL | 8 | 1 |

For a 10-element DINT array slice:

| Type  | elem_byte_size | elem_count |
|---|---|---|
| DINT  | 4 | 10 |

The data area in the slot will be `40 bytes` for that read.

## STRING is structured but accessible

If you must read an AB-style STRING via `OCXcip_AccessTagData`, you
can address the underlying scalar `int32 length` + `char data[82]`
fields by tag-name suffix (e.g. `MyTag.LEN` for the length, `MyTag.DATA`
for the array). Each is read as the underlying scalar type (DINT for
LEN, SINT array for DATA). Direct read of the whole STRING via the
struct's symbol entry isn't supported by this SDK in v0.1 — use the
two-call workaround.
