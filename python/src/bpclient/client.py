"""High-level Client — opens IPC, manages session, dispatches every
OCXcip_* opcode the SDK exposes.

Per-opcode wire layouts live next to the dataclasses they produce
(message.py, identity.py, conn.py, tagdb.py, access.py).  This file
concentrates the dispatch glue.

SPDX-License-Identifier: MIT
"""
import os
import random
import struct
import sys
import threading
import time
from dataclasses import dataclass, field

from . import _conn_wire as cw
from . import _proto as P
from ._ipc import Client as _RawClient
from .conn import ConnSpec
from .errors import (
    BpEngine,
    BpGeneric,
    BpNotOpen,
    BpNoFreeSlot,
    BpNullArg,
    BpParamRange,
    BpSlotTooLarge,
    raise_for_rc,
)
from .identity import Identity
from .message import Message


@dataclass
class _TxRxState:
    """Per-connection state cached on the Client."""
    slot: int
    conn_serial: int
    vendor_id: int
    orig_serial: int
    ot_conn_id: int
    to_conn_id: int
    sequence: int = 0       # diagnostic only — NOT on the wire


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
        # v0.7.0+ class-3 connected-messaging cache.  See txrx_open.
        self._txrx_mu = threading.Lock()
        self._txrx_conns: dict[int, _TxRxState] = {}

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
    # TxRx — class-3 connected messaging (v0.7.0+ small-buffer impl)
    # ============================================================
    # v0.7.0+ implementation: routes the connection lifecycle through
    # Client.message_send (chip mailbox 0x200, UCMM transport).  The
    # OCXcip_TxRx* OEM entry points are not used — they dispatch to
    # OCXCN_OpenClass3Connection in a library missing from the
    # cm1756 image (Ghidra RE of libocxbpapi.so.2.3 @ 0x106f44).
    #
    # Wire format documented in docs/protocol.md "Connected messaging
    # — wire format".  Sequence numbers are NOT prepended to txrx_msg
    # requests — the chip's MBOX_LOOPBACK transport treats UCMM and
    # connected CIP identically (sibling apex2_cip_connection.c:1346-1362
    # has the empirical record).
    #
    # State lifecycle:
    #   txrx_open  → Forward_Open, caches state keyed by spec.app_handle.
    #   txrx_msg   → looks up state, sends request bytes UNMODIFIED.
    #   txrx_close → Forward_Close, evicts cached state.
    #
    # Concurrent open() with the same app_handle raises BpGeneric.
    # Up to P.TXRX_MAX_CONNS connections per Client.
    #
    # Known limitation: inherits the ~500 B envelope from
    # MessageSend.  4002-byte transport via chip mbox 0x204 is
    # v0.8 territory — see docs/v0.8-large-buffer-re.md.

    def _next_conn_serial(self) -> int:
        v = random.getrandbits(16)
        return v if v != 0 else 0xBEEF

    def _next_orig_serial(self) -> int:
        return (os.getpid() & 0xFFFFFFFF) ^ ((int(time.time()) << 16) & 0xFFFFFFFF)

    def _best_effort_close(self, slot: int, conn_serial: int,
                            vendor_id: int, orig_serial: int) -> None:
        """Send a Forward_Close ignoring the outcome — cleanup after
        a txrx_open failure path so the PLC's table doesn't leak."""
        try:
            fc_msg = Message(
                slot=slot,
                cip_request=cw.build_forward_close(conn_serial, vendor_id, orig_serial),
                resp_data=b"",
                resp_capacity=64,
                timeout_ms=5000,
            )
            self.message_send(fc_msg)
        except Exception:
            pass

    def txrx_open(self, spec: ConnSpec) -> tuple[int, int]:
        """Open a class-3 connection.

        Sends a Large Forward Open (CIP svc 0x5B) to the slot encoded
        in spec.encoded_path (must be the canonical {0x01, slot}
        backplane-direct shape).  On success, caches state keyed by
        spec.app_handle and returns ``(conn_id_lo16, conn_serial)``:

          conn_id_lo16  – low 16 of the PLC-assigned O→T conn ID
          conn_serial   – the random 16-bit serial we sent in the LFO
                          (echoed by the PLC's Forward_Close reply)

        spec.conn_params, if non-zero, sets O→T/T→O size in BYTES.
        0 → SDK default 4000.  Hardware max 4002; values above are
        capped with a warning.
        """
        if spec is None or not spec.encoded_path:
            raise BpNullArg("spec.encoded_path is required")
        if spec.path_size == 0 or spec.path_size > P.TXRX_MAX_PATH:
            raise BpParamRange("path_size out of range")

        slot = cw.extract_slot(spec.encoded_path, spec.path_size)
        if slot is None:
            raise BpParamRange(
                "encoded_path must be the canonical backplane-direct "
                "shape {0x01, slot} for v0.7.0")
        if slot > P.MSG_MAX_SLOT:
            raise BpSlotTooLarge(f"slot {slot} > {P.MSG_MAX_SLOT}")

        ot_size = spec.conn_params if spec.conn_params else P.LFO_DEFAULT_OT_SIZE
        if ot_size > P.LFO_MAX_OT_SIZE:
            print(f"[txrx_open] conn_params={ot_size} exceeds LFO max "
                  f"{P.LFO_MAX_OT_SIZE}; capping (caller probably passed "
                  f"a stale OEM 16-bit param)", file=sys.stderr)
            ot_size = P.LFO_MAX_OT_SIZE

        conn_serial = self._next_conn_serial()
        orig_serial = self._next_orig_serial()
        lfo = cw.build_forward_open(conn_serial, orig_serial, ot_size)

        msg = Message(
            slot=slot,
            cip_request=lfo,
            resp_data=b"",
            resp_capacity=64,
            timeout_ms=5000,
        )
        self.message_send(msg)

        ot_conn_id, to_conn_id, status, ok = cw.parse_forward_open(msg.resp_data)
        if not ok:
            svc = msg.resp_data[0] if msg.resp_data else 0
            ext = 0
            if len(msg.resp_data) >= 6 and msg.resp_data[3]:
                ext = msg.resp_data[4] | (msg.resp_data[5] << 8)
            print(f"[txrx_open] LFO CIP failure: svc=0x{svc:02x} "
                  f"status=0x{status:02x} ext=0x{ext:04x} slot={slot}",
                  file=sys.stderr)
            raise BpGeneric(
                f"Forward_Open rejected: svc=0x{svc:02x} status=0x{status:02x}")

        with self._txrx_mu:
            if spec.app_handle in self._txrx_conns:
                existing = self._txrx_conns[spec.app_handle]
                self._best_effort_close(slot, conn_serial,
                                        P.LFO_VENDOR_ID, orig_serial)
                raise BpGeneric(
                    f"app_handle={spec.app_handle} already open "
                    f"(slot={existing.slot}, serial=0x{existing.conn_serial:04x}) "
                    f"— call txrx_close first")
            if len(self._txrx_conns) >= P.TXRX_MAX_CONNS:
                self._best_effort_close(slot, conn_serial,
                                        P.LFO_VENDOR_ID, orig_serial)
                raise BpNoFreeSlot(
                    f"max {P.TXRX_MAX_CONNS} TxRx connections per Client")
            self._txrx_conns[spec.app_handle] = _TxRxState(
                slot=slot,
                conn_serial=conn_serial,
                vendor_id=P.LFO_VENDOR_ID,
                orig_serial=orig_serial,
                ot_conn_id=ot_conn_id,
                to_conn_id=to_conn_id,
            )

        return ot_conn_id & 0xFFFF, conn_serial

    def txrx_msg(self, spec: ConnSpec, req: bytes, resp_capacity: int) -> bytes:
        """Send one CIP request over the connection identified by
        ``spec.app_handle``.  Returns the raw CIP reply bytes.

        The caller's request is sent byte-for-byte (no sequence
        prepending — see docs/protocol.md).

        v0.7.0 cap: ``len(req) ≤ P.MSG_MAX_REQ`` (500 bytes).
        """
        if spec is None:
            raise BpNullArg("spec is required")
        if not req:
            raise BpNullArg("req is required")
        if resp_capacity <= 0:
            raise BpNullArg("resp_capacity must be > 0")

        with self._txrx_mu:
            state = self._txrx_conns.get(spec.app_handle)
            if state is None:
                raise BpNotOpen(
                    f"no open connection for app_handle={spec.app_handle}")
            slot = state.slot
            state.sequence += 1  # diagnostic only

        msg = Message(
            slot=slot,
            cip_request=bytes(req),
            resp_data=b"",
            resp_capacity=resp_capacity,
            timeout_ms=5000,
        )
        self.message_send(msg)
        return msg.resp_data

    def txrx_close(self, spec: ConnSpec) -> None:
        """Release the connection.  Sends Forward_Close using the
        cached identifiers, then evicts the state regardless of FC
        outcome.  Raises ``BpNotOpen`` if no entry matches
        ``spec.app_handle``."""
        if spec is None:
            raise BpNullArg("spec is required")

        with self._txrx_mu:
            state = self._txrx_conns.pop(spec.app_handle, None)
            if state is None:
                raise BpNotOpen(
                    f"no open connection for app_handle={spec.app_handle}")

        msg = Message(
            slot=state.slot,
            cip_request=cw.build_forward_close(
                state.conn_serial, state.vendor_id, state.orig_serial),
            resp_data=b"",
            resp_capacity=64,
            timeout_ms=5000,
        )
        self.message_send(msg)

        status, ok = cw.parse_forward_close(msg.resp_data)
        if not ok:
            svc = msg.resp_data[0] if msg.resp_data else 0
            print(f"[txrx_close] FC CIP failure: svc=0x{svc:02x} "
                  f"status=0x{status:02x} slot={state.slot} "
                  f"serial=0x{state.conn_serial:04x}", file=sys.stderr)
            raise BpGeneric(
                f"Forward_Close rejected: svc=0x{svc:02x} status=0x{status:02x}")


# Re-import to avoid a circular ref at module top.
from .tagdb import TagDB  # noqa: E402,F401
