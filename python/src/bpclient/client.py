"""High-level Client — opens IPC, manages session, dispatches every
OCXcip_* opcode the SDK exposes.

Per-opcode wire layouts live next to the dataclasses they produce
(message.py, identity.py, conn.py, tagdb.py, access.py).  This file
concentrates the dispatch glue.

SPDX-License-Identifier: MIT
"""
import struct
from dataclasses import dataclass

from . import _proto as P
from ._ipc import Client as _RawClient
from .conn import ConnSpec
from .errors import BpNullArg, BpParamRange, BpSlotTooLarge, raise_for_rc
from .identity import Identity
from .message import Message


class Client:
    """Public client handle.  Wraps the raw IPC layer.

    Usage::

        with bpclient.Client() as c:
            c.open_session()
            db = c.open_tagdb("P:1,S:2")
            try:
                n = db.build()
                ...
            finally:
                db.close()
    """

    def __init__(self) -> None:
        self._raw = _RawClient()

    @property
    def raw(self) -> _RawClient:
        """Low-level IPC handle.  Exposed for diagnostic tools that
        need to dispatch opcodes not in the public API."""
        return self._raw

    def open(self) -> None:
        self._raw.open()

    def close(self) -> None:
        self._raw.close()

    def __enter__(self) -> "Client":
        self.open()
        return self

    def __exit__(self, *args) -> None:
        self.close()

    # ============================================================
    # Session
    # ============================================================
    def open_session(self) -> int:
        """OCXcip_Open: return the (opaque) session handle."""
        handle = 0

        def read(slot):
            nonlocal handle
            handle = struct.unpack_from("<I", slot, P.HDR_PAYLOAD_START)[0]

        self._raw.call("OCXcip_Open", 0x80, read=read, timeout_ms=5000)
        return handle

    # ============================================================
    # Tag-DB lifecycle
    # ============================================================
    def open_tagdb(self, path: str) -> "TagDB":
        """OCXcip_CreateTagDbHandle for an OldI CIP path."""
        from .tagdb import TagDB
        if not path:
            raise BpNullArg("path is required")
        if len(path) > 254:
            raise BpParamRange("path too long (max 254 bytes)")

        handle = 0
        path_bytes = path.encode("ascii", "strict")

        def fill(slot):
            slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + 256] = b"\x00" * 256
            slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + len(path_bytes)] = path_bytes
            struct.pack_into("<H", slot, 0x178, 0)  # flags

        def read(slot):
            nonlocal handle
            handle = struct.unpack_from("<I", slot, 0x17C)[0]

        self._raw.call("OCXcip_CreateTagDbHandle", 0x180,
                       fill=fill, read=read, timeout_ms=5000)
        return TagDB(self, handle, path)

    # ============================================================
    # Unconnected (UCMM) CIP messaging
    # ============================================================
    def message_send(self, msg: Message) -> None:
        """OCXcip_MessageSend: dispatch one UCMM CIP request.

        Returns nothing — the response is populated back into
        msg.resp_data / msg.resp_len / msg.status.  Caller must
        inspect the first ~4 bytes of resp_data for CIP-level
        success.  Engine codes raise BpEngine.
        """
        if msg is None:
            raise BpNullArg("msg is required")
        if not msg.cip_request:
            raise BpParamRange("cip_request is empty")
        req_size = msg.req_size if msg.req_size > 0 else len(msg.cip_request)
        if req_size > P.MSG_MAX_REQ:
            raise BpParamRange(f"req_size {req_size} > {P.MSG_MAX_REQ}")
        if msg.resp_capacity <= 0:
            raise BpNullArg("resp_capacity must be > 0")
        if msg.slot > P.MSG_MAX_SLOT:
            raise BpSlotTooLarge(f"slot {msg.slot} > {P.MSG_MAX_SLOT}")
        if req_size > len(msg.cip_request):
            raise BpParamRange("req_size exceeds cip_request length")

        msg.req_size = req_size
        msg.resp_len = 0
        msg.status = 0

        def fill(slot):
            slot[P.MSGSEND_REQ_OFF:P.MSGSEND_REQ_OFF + req_size] = msg.cip_request[:req_size]
            struct.pack_into("<H", slot, P.MSGSEND_REQ_SIZE_OFF, req_size)
            struct.pack_into("<H", slot, P.MSGSEND_RESPLEN_OFF, msg.resp_capacity)
            slot[P.MSGSEND_SLOT_OFF] = msg.slot
            struct.pack_into("<H", slot, P.MSGSEND_TIMEOUT_OFF, msg.timeout_ms)

        def read(slot):
            got = struct.unpack_from("<H", slot, P.MSGSEND_RESPLEN_OFF)[0]
            msg.status = struct.unpack_from("<I", slot, P.MSGSEND_STATUS_OFF)[0]
            if got > msg.resp_capacity:
                got = msg.resp_capacity
            msg.resp_len = got
            if got > 0:
                msg.resp_data = bytes(slot[P.MSGSEND_RESPDATA_OFF:P.MSGSEND_RESPDATA_OFF + got])

        self._raw.call("OCXcip_MessageSend", P.MSGSEND_PAYLOAD_SIZE,
                       fill=fill, read=read, timeout_ms=5000)

    # ============================================================
    # Identity / device queries
    # ============================================================
    def _decode_id(self, raw: memoryview, offset: int) -> Identity:
        v = struct.unpack_from("<HHHBBHI", raw, offset)
        product_name = bytes(raw[offset + 0x0E:offset + 0x0E + 32])
        return Identity(
            vendor_id=v[0],
            device_type=v[1],
            product_code=v[2],
            major_rev=v[3],
            minor_rev=v[4],
            status=v[5],
            serial_number=v[6],
            product_name=product_name,
        )

    def get_id_local(self) -> Identity:
        """OCXcip_GetIdObject: Identity of the local cm1756."""
        out: Identity | None = None

        def read(slot):
            nonlocal out
            out = self._decode_id(slot, P.HDR_PAYLOAD_START)

        self._raw.call("OCXcip_GetIdObject", 0xA8, read=read, timeout_ms=5000)
        assert out is not None
        return out

    def get_device_id(self, path: str, instance: int = 1) -> Identity:
        """OCXcip_GetDeviceIdObject: Identity of a device named by
        OldI text path."""
        if not path:
            raise BpNullArg("path is required")
        if len(path) > 254:
            raise BpParamRange("path too long")
        path_bytes = path.encode("ascii", "strict")
        out: Identity | None = None

        def fill(slot):
            slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + len(path_bytes)] = path_bytes
            slot[P.HDR_PAYLOAD_START + len(path_bytes)] = 0
            struct.pack_into("<H", slot, 0x178, instance)

        def read(slot):
            nonlocal out
            out = self._decode_id(slot, 0x178)

        self._raw.call("OCXcip_GetDeviceIdObject", 0x1B0,
                       fill=fill, read=read, timeout_ms=5000)
        assert out is not None
        return out

    def get_active_nodes(self) -> tuple[int, int]:
        """OCXcip_GetActiveNodeTable: returns (mask_lo, mask_hi)
        32-bit halves of the 64-bit responsive-node bitmap."""
        lo = hi = 0

        def read(slot):
            nonlocal lo, hi
            lo, hi = struct.unpack_from("<II", slot, P.HDR_PAYLOAD_START)

        self._raw.call("OCXcip_GetActiveNodeTable", 0x80,
                       read=read, timeout_ms=5000)
        return lo, hi

    # ============================================================
    # Local cm1756 module utilities — LED / Display / Switch
    # ============================================================
    def get_switch_position(self) -> int:
        v = 0

        def read(slot):
            nonlocal v
            v = struct.unpack_from("<I", slot, P.HDR_PAYLOAD_START)[0]

        self._raw.call("OCXcip_GetSwitchPosition", 0x80, read=read, timeout_ms=5000)
        return v

    def get_led(self, led_id: int) -> int:
        state = 0

        def fill(slot):
            struct.pack_into("<I", slot, P.HDR_PAYLOAD_START, led_id)

        def read(slot):
            nonlocal state
            state = struct.unpack_from("<I", slot, P.HDR_PAYLOAD_START + 4)[0]

        self._raw.call("OCXcip_GetLED", 0x80, fill=fill, read=read, timeout_ms=5000)
        return state

    def set_led(self, led_id: int, state: int) -> None:
        def fill(slot):
            struct.pack_into("<II", slot, P.HDR_PAYLOAD_START, led_id, state)

        self._raw.call("OCXcip_SetLED", 0x80, fill=fill, timeout_ms=5000)

    def get_display(self) -> bytes:
        """Return the 4-char display value (with trailing NUL byte
        appended → 5 bytes)."""
        out = b""

        def read(slot):
            nonlocal out
            out = bytes(slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + 4]) + b"\x00"

        self._raw.call("OCXcip_GetDisplay", 0x80, read=read, timeout_ms=5000)
        return out

    def set_display(self, four_chars: bytes) -> None:
        if len(four_chars) < 4:
            raise BpParamRange("four_chars must be exactly 4 bytes")

        def fill(slot):
            slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + 4] = four_chars[:4]
            slot[P.HDR_PAYLOAD_START + 4] = 0

        self._raw.call("OCXcip_SetDisplay", 0x80, fill=fill, timeout_ms=5000)

    # ============================================================
    # TxRx — class-3 connected messaging (NOT FUNCTIONAL on cm1756)
    # ============================================================
    # OCXcip_TxRxOpenConn / TxRxMsg / TxRxCloseConn return engine code
    # 0x1001 on cm1756 because the OCXCN_OpenClass3Connection library
    # is missing.  Workaround: send a Large Forward Open (svc 0x5B)
    # via message_send; see c/examples/connidentity.c for the recipe.
    # These methods are kept for parity with the C SDK so failure
    # paths can be diffed across languages.

    def txrx_open(self, spec: ConnSpec) -> tuple[int, int]:
        """OCXcip_TxRxOpenConn: returns (conn_id, conn_serial).
        STATUS: NOT FUNCTIONAL on cm1756."""
        if spec is None or not spec.encoded_path:
            raise BpNullArg("spec.encoded_path is required")
        if spec.path_size == 0 or spec.path_size > P.TXRX_MAX_PATH:
            raise BpParamRange("path_size out of range")
        conn_id = conn_serial = 0

        def fill(slot):
            struct.pack_into("<HI", slot, P.TXRX_APP_HANDLE_OFF,
                             spec.app_handle, spec.options)
            slot[P.TXRX_PATH_OFF:P.TXRX_PATH_OFF + spec.path_size] = spec.encoded_path[:spec.path_size]
            struct.pack_into("<HH", slot, P.TXRX_PATH_SIZE_OFF,
                             spec.path_size, spec.conn_params)

        def read(slot):
            nonlocal conn_id, conn_serial
            conn_id, conn_serial = struct.unpack_from("<HH", slot, P.TXRX_CONN_ID_OFF)

        self._raw.call("OCXcip_TxRxOpenConn", P.TXRX_OPENCLOSE_PAYLOAD,
                       fill=fill, read=read, timeout_ms=30000)
        return conn_id, conn_serial

    def txrx_msg(self, spec: ConnSpec, req: bytes, resp_capacity: int) -> bytes:
        """OCXcip_TxRxMsg: returns the response bytes.
        STATUS: NOT FUNCTIONAL on cm1756."""
        if spec is None or not spec.encoded_path:
            raise BpNullArg("spec.encoded_path is required")
        if spec.path_size == 0 or spec.path_size > P.TXRX_MAX_PATH:
            raise BpParamRange("path_size out of range")
        if resp_capacity <= 0:
            raise BpNullArg("resp_capacity must be > 0")
        req_size = len(req)
        out = b""

        def fill(slot):
            struct.pack_into("<HI", slot, P.TXRX_APP_HANDLE_OFF,
                             spec.app_handle, spec.options)
            slot[P.TXRX_PATH_OFF:P.TXRX_PATH_OFF + spec.path_size] = spec.encoded_path[:spec.path_size]
            struct.pack_into("<HH", slot, P.TXRX_PATH_SIZE_OFF,
                             spec.path_size, spec.conn_params)
            if req_size > 0:
                slot[P.TXRXMSG_REQ_BUF_OFF:P.TXRXMSG_REQ_BUF_OFF + req_size] = req[:req_size]
            struct.pack_into("<H", slot, P.TXRXMSG_REQ_SIZE_OFF, req_size)
            struct.pack_into("<H", slot, P.TXRXMSG_RESP_LEN_OFF, resp_capacity)

        def read(slot):
            nonlocal out
            got = struct.unpack_from("<H", slot, P.TXRXMSG_RESP_LEN_OFF)[0]
            if got > resp_capacity:
                got = resp_capacity
            if got > 0:
                out = bytes(slot[P.TXRXMSG_RESP_BUF_OFF:P.TXRXMSG_RESP_BUF_OFF + got])

        self._raw.call("OCXcip_TxRxMsg", P.TXRXMSG_PAYLOAD,
                       fill=fill, read=read, timeout_ms=30000)
        return out

    def txrx_close(self, spec: ConnSpec) -> None:
        """OCXcip_TxRxCloseConn.
        STATUS: NOT FUNCTIONAL on cm1756."""
        if spec is None or not spec.encoded_path:
            raise BpNullArg("spec.encoded_path is required")
        if spec.path_size == 0 or spec.path_size > P.TXRX_MAX_PATH:
            raise BpParamRange("path_size out of range")

        def fill(slot):
            struct.pack_into("<HI", slot, P.TXRX_APP_HANDLE_OFF,
                             spec.app_handle, spec.options)
            slot[P.TXRX_PATH_OFF:P.TXRX_PATH_OFF + spec.path_size] = spec.encoded_path[:spec.path_size]
            struct.pack_into("<H", slot, P.TXRX_PATH_SIZE_OFF, spec.path_size)

        self._raw.call("OCXcip_TxRxCloseConn", P.TXRX_OPENCLOSE_PAYLOAD,
                       fill=fill, timeout_ms=5000)


# Re-import to avoid a circular ref at module top.
from .tagdb import TagDB  # noqa: E402,F401
