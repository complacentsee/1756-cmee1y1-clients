"""TagDB — per-PLC tag database handle + per-symbol/UDT inspection +
batched AccessTagData (with scalar/array helpers).

SPDX-License-Identifier: MIT
"""
import struct
from dataclasses import dataclass
from typing import TYPE_CHECKING

from . import _proto as P
from .access import TagRequest
from .errors import (
    BpEngine,
    BpGeneric,
    BpNullArg,
    BpParamRange,
    BpSlotTooLarge,
)

if TYPE_CHECKING:
    from .client import Client


@dataclass
class Symbol:
    """One PLC symbol — returned by TagDB.symbol_at(index).
    Mirrors bp_symbol_info_t."""
    name: str = ""
    data_type: int = 0
    struct_type: int = 0
    elem_byte_size: int = 0
    dim0: int = 0
    dim1: int = 0
    dim2: int = 0
    flags: int = 0

    def is_array(self) -> bool:
        return self.dim0 != 0

    def is_struct(self) -> bool:
        return self.struct_type != 0

    def type_code(self) -> int:
        return self.data_type & 0x1FFF

    def rank(self) -> int:
        if self.dim2:
            return 3
        if self.dim1:
            return 2
        if self.dim0:
            return 1
        return 0

    def total_elements(self) -> int:
        return (self.dim0 or 1) * (self.dim1 or 1) * (self.dim2 or 1)


@dataclass
class StructInfo:
    """UDT template descriptor — returned by TagDB.get_struct_info."""
    name: str = ""
    data_type: int = 0
    byte_size: int = 0
    n_members: int = 0


@dataclass
class StructMember:
    """One member of a UDT template — returned by TagDB.get_struct_member."""
    name: str = ""
    data_type: int = 0
    struct_id: int = 0      # non-zero if member is itself a UDT
    byte_size: int = 0
    offset: int = 0
    array_count: int = 0    # N for SINT[N]/DINT[N]/...; 0 scalar
    flags: int = 0

    def is_array(self) -> bool:
        return (self.flags & 0x08) != 0

    def is_struct_member(self) -> bool:
        return self.struct_id != 0


def _read_cstring(buf: memoryview | bytes, max_len: int) -> str:
    """Read NUL-terminated ASCII, capped at max_len bytes."""
    n = 0
    while n < max_len and n < len(buf) and buf[n] != 0:
        n += 1
    return bytes(buf[:n]).decode("ascii", "replace")


class TagDB:
    """Per-PLC tag-DB handle.  Construct via Client.open_tagdb(path)."""

    def __init__(self, client: "Client", handle: int, path: str) -> None:
        self._client = client
        self._handle = handle
        self._path = path

    @property
    def handle(self) -> int:
        return self._handle

    @property
    def path(self) -> str:
        return self._path

    def close(self) -> None:
        """OCXcip_DeleteTagDbHandle.  Idempotent."""
        if self._handle == 0:
            return
        h = self._handle

        def fill(slot):
            struct.pack_into("<I", slot, P.HDR_PAYLOAD_START, h)

        try:
            self._client.raw.call("OCXcip_DeleteTagDbHandle", 0x80,
                                  fill=fill, timeout_ms=5000)
        except Exception:
            pass
        self._handle = 0

    # ============================================================
    # Build / TestVersion / SymbolAt / StructInfo / StructMember
    # ============================================================
    def build(self) -> int:
        """OCXcip_BuildTagDb: returns the enumerated symbol count.

        On success, invalidates the per-PLC symbol cache for this
        path and resizes it for the fresh table.  Lazy fill kicks
        in on the next ``lookup_symbol``; call ``preload_symbols``
        for eager warm-up.
        """
        h = self._handle
        n = 0

        def fill(slot):
            struct.pack_into("<I", slot, P.HDR_PAYLOAD_START, h)

        def read(slot):
            nonlocal n
            n = struct.unpack_from("<H", slot, P.HDR_PAYLOAD_START + 4)[0]

        self._client.raw.call("OCXcip_BuildTagDb", 0x80,
                              fill=fill, read=read, timeout_ms=30000)
        self._client._reset_tag_cache_after_build(self._path, n)
        return n

    def lookup_symbol(self, name: str) -> Symbol:
        """Look up a symbol by name via the per-client cache.

        First call after ``build`` for a never-before-seen name walks
        ``symbol_at`` incrementally until a match is found (each
        examined symbol is appended to the cache so subsequent
        lookups are array scans, not IPC round-trips).

        Raises BpParamRange if the name isn't in the PLC's tag table
        (or build hasn't populated the cache yet).
        """
        if not name:
            raise BpNullArg("name is required")
        return self._client._lookup_cached_symbol(self, name)

    def preload_symbols(self) -> int:
        """Eagerly fetch every symbol descriptor.

        Returns the count of cached entries.  Useful when callers
        want to pay the IPC cost up front (one round-trip per
        symbol) instead of amortized across the first scan-loop
        iteration.
        """
        return self._client._preload_cached_symbols(self)

    def test_version(self) -> bool:
        """OCXcip_TestTagDbVer.  Returns True if caller should rebuild
        (covers both engine rc 0x14 versions-differ and 0x15
        no-captured-version)."""
        h = self._handle

        def fill(slot):
            struct.pack_into("<I", slot, P.HDR_PAYLOAD_START, h)

        try:
            self._client.raw.call("OCXcip_TestTagDbVer", 0x80,
                                  fill=fill, timeout_ms=5000)
            return False
        except BpEngine as e:
            if e.code in (0x14, 0x15):
                return True
            raise

    def symbol_at(self, index: int) -> Symbol:
        """OCXcip_GetSymbolInfo by zero-based index."""
        h = self._handle
        raw_bytes = b""

        def fill(slot):
            struct.pack_into("<IH", slot, P.HDR_PAYLOAD_START, h, index)

        def read(slot):
            nonlocal raw_bytes
            raw_bytes = bytes(slot[P.HDR_PAYLOAD_START + 8:P.HDR_PAYLOAD_START + 8 + 128])

        self._client.raw.call("OCXcip_GetSymbolInfo", 0x100,
                              fill=fill, read=read, timeout_ms=5000)

        # Layout per docs/protocol.md "ocx_symbol_info_t":
        #  +0x00 char[100] name
        #  +0x64 uint16    data_type
        #  +0x68 uint16    struct_type
        #  +0x6C uint32    elem_byte_size
        #  +0x70 uint32    dim0
        #  +0x74 uint32    dim1
        #  +0x78 uint32    dim2
        #  +0x7C uint16    flags
        return Symbol(
            name=_read_cstring(raw_bytes[:100], 99),
            data_type=struct.unpack_from("<H", raw_bytes, 0x64)[0],
            struct_type=struct.unpack_from("<H", raw_bytes, 0x68)[0],
            elem_byte_size=struct.unpack_from("<I", raw_bytes, 0x6C)[0],
            dim0=struct.unpack_from("<I", raw_bytes, 0x70)[0],
            dim1=struct.unpack_from("<I", raw_bytes, 0x74)[0],
            dim2=struct.unpack_from("<I", raw_bytes, 0x78)[0],
            flags=struct.unpack_from("<H", raw_bytes, 0x7C)[0],
        )

    def get_struct_info(self, struct_id: int) -> StructInfo:
        """OCXcip_GetStructInfo by UDT template id."""
        h = self._handle
        raw_bytes = b""

        def fill(slot):
            struct.pack_into("<IH", slot, P.HDR_PAYLOAD_START, h, struct_id)

        def read(slot):
            nonlocal raw_bytes
            raw_bytes = bytes(slot[P.HDR_PAYLOAD_START + 8:P.HDR_PAYLOAD_START + 8 + 56])

        self._client.raw.call("OCXcip_GetStructInfo", 0xB8,
                              fill=fill, read=read, timeout_ms=5000)
        return StructInfo(
            name=_read_cstring(raw_bytes[:40], 39),
            data_type=struct.unpack_from("<I", raw_bytes, 0x2C)[0],
            byte_size=struct.unpack_from("<I", raw_bytes, 0x30)[0],
            n_members=struct.unpack_from("<H", raw_bytes, 0x36)[0],
        )

    def get_struct_member(self, struct_id: int, member_index: int) -> StructMember:
        """OCXcip_GetStructMbrInfo by (struct_id, member_index)."""
        h = self._handle
        raw_bytes = b""

        def fill(slot):
            struct.pack_into("<IHH", slot, P.HDR_PAYLOAD_START, h,
                             struct_id, member_index)

        def read(slot):
            nonlocal raw_bytes
            raw_bytes = bytes(slot[P.HDR_PAYLOAD_START + 8:P.HDR_PAYLOAD_START + 8 + 76])

        self._client.raw.call("OCXcip_GetStructMbrInfo", 0xD0,
                              fill=fill, read=read, timeout_ms=5000)
        return StructMember(
            name=_read_cstring(raw_bytes[:44], 43),
            data_type=struct.unpack_from("<H", raw_bytes, 0x2C)[0],
            struct_id=struct.unpack_from("<H", raw_bytes, 0x30)[0],
            byte_size=struct.unpack_from("<I", raw_bytes, 0x34)[0],
            offset=struct.unpack_from("<I", raw_bytes, 0x38)[0],
            array_count=struct.unpack_from("<I", raw_bytes, 0x40)[0],
            flags=raw_bytes[0x44],
        )

    # ============================================================
    # AccessTagData
    # ============================================================
    def access(self, reqs: list[TagRequest]) -> None:
        """OCXcip_AccessTagData.  Each request's `.result` is set on
        return; per-request CIP General Status of 0 = ok.  Slot-level
        errorcode raises BpEngine."""
        if not reqs:
            raise BpNullArg("reqs is empty")
        if len(reqs) > 16:
            raise BpParamRange("max 16 requests per call")

        data_area_start = 0x2A0 + (len(reqs) - 1) * P.TAGDATA_REQ_STRIDE
        total_data_bytes = sum(r.elem_byte_size * r.elem_count for r in reqs)
        payload_size = data_area_start + total_data_bytes
        if payload_size > P.SLOT_STRIDE - 0x80:
            raise BpSlotTooLarge(f"payload {payload_size} exceeds slot capacity")

        path_bytes = self._path.encode("ascii", "strict")

        def fill(slot):
            # Path at +0x78 (256-byte region, already zero'd by dispatcher).
            slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + len(path_bytes)] = path_bytes
            struct.pack_into("<H", slot, P.TAGDATA_SERVICE_OFF, 0)
            struct.pack_into("<H", slot, P.TAGDATA_COUNT_OFF, len(reqs))

            data_off = data_area_start
            for i, r in enumerate(reqs):
                req_start = P.TAGDATA_REQ0_START + i * P.TAGDATA_REQ_STRIDE
                # descriptor is already zero'd
                tn = r.tag_name.encode("ascii", "strict")[:254]
                slot[req_start:req_start + len(tn)] = tn
                struct.pack_into("<HHHH", slot, req_start + P.REQ_DATATYPE_OFF,
                                 r.data_type, r.elem_byte_size,
                                 r.action, r.elem_count)
                slot[req_start + P.REQ_HAS_EXTRA_OFF] = 0
                struct.pack_into("<Q", slot, req_start + P.REQ_DATA_PTR_OFF, 0)

                nbytes = r.elem_byte_size * r.elem_count
                if r.action == P.ACTION_WRITE and nbytes > 0 and len(r.data) > 0:
                    n = min(nbytes, len(r.data))
                    slot[data_off:data_off + n] = r.data[:n]
                data_off += nbytes

        def read(slot):
            data_off = data_area_start
            for i, r in enumerate(reqs):
                req_start = P.TAGDATA_REQ0_START + i * P.TAGDATA_REQ_STRIDE
                r.result = struct.unpack_from("<I", slot, req_start + P.REQ_RESULT_OFF)[0]
                nbytes = r.elem_byte_size * r.elem_count
                if r.action == P.ACTION_READ and nbytes > 0:
                    r.data = bytes(slot[data_off:data_off + nbytes])
                data_off += nbytes

        self._client.raw.call("OCXcip_AccessTagData", payload_size,
                              fill=fill, read=read, timeout_ms=10000)

    # ============================================================
    # Scalar helpers
    # ============================================================
    def _scalar_rw(self, tag: str, data_type: int, byte_size: int,
                   action: int, data: bytes) -> bytes:
        r = TagRequest(
            tag_name=tag, data_type=data_type,
            elem_byte_size=byte_size, action=action,
            elem_count=1, data=data,
        )
        self.access([r])
        if r.result != 0:
            raise BpGeneric(f"CIP general status {r.result:#x} on tag {tag!r}")
        return r.data if action == P.ACTION_READ else b""

    def read_sint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_SINT, 1, P.ACTION_READ, b"\x00")
        return struct.unpack("<b", b[:1])[0]

    def write_sint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_SINT, 1, P.ACTION_WRITE, struct.pack("<b", v))

    def read_int(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_INT, 2, P.ACTION_READ, b"\x00" * 2)
        return struct.unpack("<h", b[:2])[0]

    def write_int(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_INT, 2, P.ACTION_WRITE, struct.pack("<h", v))

    def read_dint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_DINT, 4, P.ACTION_READ, b"\x00" * 4)
        return struct.unpack("<i", b[:4])[0]

    def write_dint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_DINT, 4, P.ACTION_WRITE, struct.pack("<i", v))

    def read_lint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_LINT, 8, P.ACTION_READ, b"\x00" * 8)
        return struct.unpack("<q", b[:8])[0]

    def write_lint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_LINT, 8, P.ACTION_WRITE, struct.pack("<q", v))

    def read_usint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_USINT, 1, P.ACTION_READ, b"\x00")
        return b[0]

    def write_usint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_USINT, 1, P.ACTION_WRITE, struct.pack("<B", v))

    def read_uint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_UINT, 2, P.ACTION_READ, b"\x00" * 2)
        return struct.unpack("<H", b[:2])[0]

    def write_uint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_UINT, 2, P.ACTION_WRITE, struct.pack("<H", v))

    def read_udint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_UDINT, 4, P.ACTION_READ, b"\x00" * 4)
        return struct.unpack("<I", b[:4])[0]

    def write_udint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_UDINT, 4, P.ACTION_WRITE, struct.pack("<I", v))

    def read_ulint(self, tag: str) -> int:
        b = self._scalar_rw(tag, P.TYPE_ULINT, 8, P.ACTION_READ, b"\x00" * 8)
        return struct.unpack("<Q", b[:8])[0]

    def write_ulint(self, tag: str, v: int) -> None:
        self._scalar_rw(tag, P.TYPE_ULINT, 8, P.ACTION_WRITE, struct.pack("<Q", v))

    def read_real(self, tag: str) -> float:
        b = self._scalar_rw(tag, P.TYPE_REAL, 4, P.ACTION_READ, b"\x00" * 4)
        return struct.unpack("<f", b[:4])[0]

    def write_real(self, tag: str, v: float) -> None:
        self._scalar_rw(tag, P.TYPE_REAL, 4, P.ACTION_WRITE, struct.pack("<f", v))

    def read_lreal(self, tag: str) -> float:
        b = self._scalar_rw(tag, P.TYPE_LREAL, 8, P.ACTION_READ, b"\x00" * 8)
        return struct.unpack("<d", b[:8])[0]

    def write_lreal(self, tag: str, v: float) -> None:
        self._scalar_rw(tag, P.TYPE_LREAL, 8, P.ACTION_WRITE, struct.pack("<d", v))

    def read_bool(self, tag: str) -> bool:
        b = self._scalar_rw(tag, P.TYPE_BOOL, 1, P.ACTION_READ, b"\x00")
        return b[0] != 0

    def write_bool(self, tag: str, v: bool) -> None:
        self._scalar_rw(tag, P.TYPE_BOOL, 1, P.ACTION_WRITE, b"\x01" if v else b"\x00")

    # ============================================================
    # Array helpers
    #
    # Each Read/Write pair dispatches one batched-of-one Access call
    # with the appropriate type code + elem_count.  Read returns a
    # Python list; write takes a list/sequence.
    # ============================================================
    def _array_rw(self, tag: str, data_type: int, elem_byte_size: int,
                  action: int, count: int, data: bytes) -> bytes:
        if count <= 0 or count > 0xFFFF:
            raise BpParamRange("array count must be 1..65535")
        r = TagRequest(
            tag_name=tag, data_type=data_type,
            elem_byte_size=elem_byte_size, action=action,
            elem_count=count, data=data,
        )
        self.access([r])
        if r.result != 0:
            raise BpGeneric(f"CIP general status {r.result:#x} on tag {tag!r}")
        return r.data if action == P.ACTION_READ else b""

    def _unpack_array(self, raw: bytes, count: int, fmt: str, elem_size: int) -> list:
        return list(struct.unpack(f"<{count}{fmt}", raw[:count * elem_size]))

    def _pack_array(self, vals, fmt: str) -> bytes:
        return struct.pack(f"<{len(vals)}{fmt}", *vals)

    # Signed integer arrays
    def read_sint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_SINT, 1, P.ACTION_READ,
                             count, b"\x00" * count)
        return self._unpack_array(raw, count, "b", 1)

    def write_sint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_SINT, 1, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "b"))

    def read_int_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_INT, 2, P.ACTION_READ,
                             count, b"\x00" * (count * 2))
        return self._unpack_array(raw, count, "h", 2)

    def write_int_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_INT, 2, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "h"))

    def read_dint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_DINT, 4, P.ACTION_READ,
                             count, b"\x00" * (count * 4))
        return self._unpack_array(raw, count, "i", 4)

    def write_dint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_DINT, 4, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "i"))

    def read_lint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_LINT, 8, P.ACTION_READ,
                             count, b"\x00" * (count * 8))
        return self._unpack_array(raw, count, "q", 8)

    def write_lint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_LINT, 8, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "q"))

    # Unsigned integer arrays
    def read_usint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_USINT, 1, P.ACTION_READ,
                             count, b"\x00" * count)
        return list(raw[:count])

    def write_usint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_USINT, 1, P.ACTION_WRITE,
                       len(vals), bytes(vals))

    def read_uint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_UINT, 2, P.ACTION_READ,
                             count, b"\x00" * (count * 2))
        return self._unpack_array(raw, count, "H", 2)

    def write_uint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_UINT, 2, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "H"))

    def read_udint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_UDINT, 4, P.ACTION_READ,
                             count, b"\x00" * (count * 4))
        return self._unpack_array(raw, count, "I", 4)

    def write_udint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_UDINT, 4, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "I"))

    def read_ulint_array(self, tag: str, count: int) -> list[int]:
        raw = self._array_rw(tag, P.TYPE_ULINT, 8, P.ACTION_READ,
                             count, b"\x00" * (count * 8))
        return self._unpack_array(raw, count, "Q", 8)

    def write_ulint_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_ULINT, 8, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "Q"))

    # Float arrays
    def read_real_array(self, tag: str, count: int) -> list[float]:
        raw = self._array_rw(tag, P.TYPE_REAL, 4, P.ACTION_READ,
                             count, b"\x00" * (count * 4))
        return self._unpack_array(raw, count, "f", 4)

    def write_real_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_REAL, 4, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "f"))

    def read_lreal_array(self, tag: str, count: int) -> list[float]:
        raw = self._array_rw(tag, P.TYPE_LREAL, 8, P.ACTION_READ,
                             count, b"\x00" * (count * 8))
        return self._unpack_array(raw, count, "d", 8)

    def write_lreal_array(self, tag: str, vals) -> None:
        self._array_rw(tag, P.TYPE_LREAL, 8, P.ACTION_WRITE,
                       len(vals), self._pack_array(vals, "d"))

    # ============================================================
    # BOOL[] (Logix BIT_ARRAY 0xD3)
    #
    # Logix packs BOOL[N] as ceil(N/32) DWORDs on the wire.  These
    # helpers convert between that wire form and a Python list[bool].
    #
    # Write note: if count isn't a multiple of 32, trailing bits in
    # the last DWORD are written as zeros.  Read-modify-write if you
    # need to preserve unrelated bits.
    # ============================================================
    @staticmethod
    def _dwords_for_bits(count: int) -> int:
        return (count + 31) // 32

    def read_bool_array(self, tag: str, count: int) -> list[bool]:
        """Read `count` BOOL[] elements as a list[bool]."""
        if count <= 0 or count > 0xFFFF:
            raise BpParamRange("array count must be 1..65535")
        n_dwords = self._dwords_for_bits(count)
        raw = self._array_rw(tag, P.TYPE_BIT_ARRAY, 4, P.ACTION_READ,
                             n_dwords, b"\x00" * (n_dwords * 4))
        dwords = struct.unpack(f"<{n_dwords}I", raw[:n_dwords * 4])
        return [bool((dwords[i // 32] >> (i & 31)) & 1) for i in range(count)]

    def write_bool_array(self, tag: str, vals) -> None:
        """Write a list[bool] / sequence as Logix BIT_ARRAY DWORDs."""
        count = len(vals)
        if count == 0 or count > 0xFFFF:
            raise BpParamRange("array count must be 1..65535")
        n_dwords = self._dwords_for_bits(count)
        dwords = [0] * n_dwords
        for i, v in enumerate(vals):
            if v:
                dwords[i // 32] |= 1 << (i & 31)
        packed = struct.pack(f"<{n_dwords}I", *dwords)
        self._array_rw(tag, P.TYPE_BIT_ARRAY, 4, P.ACTION_WRITE,
                       n_dwords, packed)

    # ============================================================
    # STRING (AB Logix STRING family)
    #
    # Works with the default STRING (LEN:DINT + DATA:SINT[82]),
    # STRING_32, STRING_512, and any LEN+DATA-shaped UDT.  Two
    # IPC round-trips internally (.LEN then .DATA).
    # ============================================================
    def read_string(self, tag: str) -> str:
        """Read tag.LEN (DINT) then tag.DATA (SINT[LEN])."""
        if not tag or len(tag) > 250:
            raise BpParamRange("tag too long")
        len_field = self.read_dint(tag + ".LEN")
        if len_field <= 0:
            return ""
        n = min(len_field, 0xFFFF)
        data = self.read_sint_array(tag + ".DATA", n)
        return bytes(b & 0xFF for b in data).decode("latin-1")

    def write_string(self, tag: str, value: str) -> None:
        """Write value's bytes to tag.DATA then update tag.LEN.

        If the destination's DATA[] capacity is smaller than
        len(value), the engine returns a CIP General Status error
        (typically 0x13 "Not enough data" or 0x15 "Too much data"),
        surfaced as BpGeneric.
        """
        if not tag or len(tag) > 250:
            raise BpParamRange("tag too long")
        encoded = value.encode("latin-1")
        if len(encoded) > 0xFFFF:
            raise BpParamRange("value too long")
        if encoded:
            data = [b - 256 if b >= 128 else b for b in encoded]
            self.write_sint_array(tag + ".DATA", data)
        self.write_dint(tag + ".LEN", len(encoded))
