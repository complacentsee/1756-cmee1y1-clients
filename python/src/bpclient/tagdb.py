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
        """OCXcip_BuildTagDb: returns the enumerated symbol count."""
        h = self._handle
        n = 0

        def fill(slot):
            struct.pack_into("<I", slot, P.HDR_PAYLOAD_START, h)

        def read(slot):
            nonlocal n
            n = struct.unpack_from("<H", slot, P.HDR_PAYLOAD_START + 4)[0]

        self._client.raw.call("OCXcip_BuildTagDb", 0x80,
                              fill=fill, read=read, timeout_ms=30000)
        return n

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
