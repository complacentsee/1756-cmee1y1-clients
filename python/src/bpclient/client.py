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
    BpCipError,
    BpEngine,
    BpGeneric,
    BpNotOpen,
    BpNoFreeSlot,
    BpNullArg,
    BpParamRange,
    BpSlotTooLarge,
    cip_status_message,
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
        # v0.8.0+ per-slot connection pools.  See pool_open / pool_txrx.
        self._pools_mu = threading.Lock()
        self._pools: dict[int, _Pool] = {}
        # v0.9.0+ per-PLC symbol cache, keyed on the OldI CIP path.
        # Shared across all TagDB handles to the same path.
        self._tag_cache_mu = threading.Lock()
        self._tag_caches: dict[str, _TagCache] = {}

    @property
    def raw(self) -> _RawClient:
        """Low-level IPC handle.  Exposed for diagnostic tools that
        need to dispatch opcodes not in the public API."""
        return self._raw

    def open(self) -> None:
        self._raw.open()

    def close(self) -> None:
        # Close pools first so keepalive threads stop before we tear
        # down the underlying IPC; PoolClose sends FC to the PLC.
        with self._pools_mu:
            slots = list(self._pools.keys())
        for s in slots:
            try:
                self.pool_close(s)
            except Exception:
                pass
        # Release the engine session so bpServer's session table
        # doesn't accumulate dead entries (best-effort).
        try:
            self.close_session()
        except Exception:
            pass
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

    def close_session(self) -> None:
        """OCXcip_Close: release the engine-side session opened by
        ``open_session``.  ``Client.close`` calls this automatically;
        explicit invocation is only needed if you want to keep the
        SDK's IPC handle alive across multiple sessions."""
        self._raw.call("OCXcip_Close", 0x78, timeout_ms=5000)

    def parse_path(self, text: str) -> "ParsedPath":
        """OCXcip_ParsePath: text path → encoded EPATH.

        Wire: payload 0x288, text at slot+0x78, encoded bytes at
        slot+0x180.. with the byte count at slot+0x280.

        Returns a ParsedPath with the encoded bytes + parsed class/
        instance/attribute.  Raises BpEngine(-101) on malformed text.
        """
        if not text:
            raise BpNullArg("text is required")
        if len(text) > 254:
            raise BpParamRange("text > 254 bytes")
        path_bytes = text.encode("ascii", "strict")
        state: dict = {}

        def fill(slot):
            slot[0x078:0x078 + len(path_bytes)] = path_bytes
            slot[0x078 + len(path_bytes)] = 0
            struct.pack_into("<H", slot, 0x280, 256)

        def read(slot):
            n = struct.unpack_from("<H", slot, 0x280)[0]
            if n > 256:
                n = 256
            state["encoded"] = bytes(slot[0x180:0x180 + n])
            state["cip_class"] = struct.unpack_from("<H", slot, 0x178)[0]
            state["segment_flags"] = slot[0x17A]
            state["instance"] = struct.unpack_from("<I", slot, 0x17C)[0]
            state["attr_flags"] = slot[0x282]

        self._raw.call("OCXcip_ParsePath", 0x288,
                       fill=fill, read=read, timeout_ms=5000)
        return ParsedPath(
            encoded=state["encoded"],
            cip_class=state["cip_class"],
            segment_flags=state["segment_flags"],
            instance=state["instance"],
            attr_flags=state["attr_flags"],
        )

    def error_string(self, code: int) -> str:
        """OCXcip_ErrorString: fetch the engine-owned ASCII description
        for an arbitrary error code (positive engine codes, negative
        OCX_ERR_*, or unknown values).  Complements ``strerror``
        (which only knows hardcoded BP_ERR_* values).

        Wire: payload 0xD0, code at slot+0x78, 78-byte ASCII at
        slot+0x7C..+0xC8.  Returns empty string if the engine has
        no entry for ``code``."""
        out = ""

        def fill(slot):
            struct.pack_into("<i", slot, P.HDR_PAYLOAD_START, code)

        def read(slot):
            nonlocal out
            buf = bytes(slot[0x7C:0x7C + 78])
            n = buf.find(b"\x00")
            if n < 0:
                n = len(buf)
            out = buf[:n].decode("ascii", "replace")

        self._raw.call("OCXcip_ErrorString", 0xD0,
                       fill=fill, read=read, timeout_ms=5000)
        return out

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

    def get_device_id_status(self, path: str, instance: int = 1) -> int:
        """OCXcip_GetDeviceIdStatus: 16-bit Identity status word for
        the device named by OldI text path.  Faster than
        ``get_device_id`` when callers only need the heartbeat /
        Logix-mode nibble (bits 4..7).
        """
        if not path:
            raise BpNullArg("path is required")
        if len(path) > 254:
            raise BpParamRange("path too long")
        path_bytes = path.encode("ascii", "strict")
        out_status = 0

        def fill(slot):
            slot[P.HDR_PAYLOAD_START:P.HDR_PAYLOAD_START + len(path_bytes)] = path_bytes
            slot[P.HDR_PAYLOAD_START + len(path_bytes)] = 0
            struct.pack_into("<H", slot, 0x178, instance)

        def read(slot):
            nonlocal out_status
            # +0x78 is the standard payload-start offset; the engine
            # overwrites the consumed input path text with the 16-bit
            # status response here.
            out_status = struct.unpack_from("<H", slot, P.HDR_PAYLOAD_START)[0]

        self._raw.call("OCXcip_GetDeviceIdStatus", 0x180,
                       fill=fill, read=read, timeout_ms=5000)
        return out_status

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
                  f"status=0x{status:02x} ext=0x{ext:04x} slot={slot} "
                  f"({cip_status_message(status, ext)})",
                  file=sys.stderr)
            raise BpCipError(service=svc, status=status, ext_status=ext, slot=slot)

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

    def _force_close_local(self, app_handle: int) -> None:
        """Wipe a single txrx_conn slot locally without sending FC.
        Used by pool auto-reopen (v0.9.0 Phase 4) when the PLC has
        already dropped the connection."""
        with self._txrx_mu:
            self._txrx_conns.pop(app_handle, None)

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
            ext = 0
            if len(msg.resp_data) >= 6 and msg.resp_data[3]:
                ext = msg.resp_data[4] | (msg.resp_data[5] << 8)
            print(f"[txrx_close] FC CIP failure: svc=0x{svc:02x} "
                  f"status=0x{status:02x} ext=0x{ext:04x} slot={state.slot} "
                  f"serial=0x{state.conn_serial:04x} "
                  f"({cip_status_message(status, ext)})", file=sys.stderr)
            raise BpCipError(service=svc, status=status,
                             ext_status=ext, slot=state.slot)

    # ============================================================
    # Connection pool (v0.8.0)
    # ============================================================
    def pool_open(self, spec: "PoolSpec") -> None:
        """Pre-open ``spec.size`` class-3 connections to ``spec.slot``
        and start a keepalive thread (if ``keepalive_ms > 0``).  Only
        one pool per slot per Client; reopening without pool_close
        first raises BpGeneric.

        On partial failure the partial state is rolled back and the
        underlying error (BpCipError / BpEngine / etc.) is reraised.
        """
        if spec is None:
            raise BpNullArg("spec is required")
        if spec.slot > P.MSG_MAX_SLOT:
            raise BpSlotTooLarge(f"slot {spec.slot} > {P.MSG_MAX_SLOT}")
        if spec.size < 1 or spec.size > P.POOL_MAX_SIZE:
            raise BpParamRange(
                f"size {spec.size} not in 1..{P.POOL_MAX_SIZE}")

        with self._pools_mu:
            if spec.slot in self._pools:
                raise BpGeneric(
                    f"pool already open for slot {spec.slot} "
                    "(call pool_close first)")
            pool = _Pool(slot=spec.slot, size=spec.size,
                         keepalive_ms=spec.keepalive_ms,
                         conn_params=spec.conn_params)
            self._pools[spec.slot] = pool

        opened = []
        try:
            now = time.monotonic()
            for i in range(spec.size):
                app_handle = (P.POOL_APP_HANDLE_BASE
                              | (spec.slot << 8) | i)
                cs = ConnSpec(
                    app_handle=app_handle,
                    encoded_path=bytes([0x01, spec.slot]),
                    path_size=2,
                    conn_params=spec.conn_params,
                )
                self.txrx_open(cs)
                pool.entries.append(_PoolEntry(app_handle=app_handle,
                                                last_used=now))
                opened.append(i)
        except Exception:
            for i in opened:
                cs = ConnSpec(
                    app_handle=pool.entries[i].app_handle,
                    encoded_path=bytes([0x01, spec.slot]),
                    path_size=2,
                    conn_params=spec.conn_params,
                )
                try:
                    self.txrx_close(cs)
                except Exception:
                    pass
            with self._pools_mu:
                self._pools.pop(spec.slot, None)
            raise

        # Seed the free queue with all entry indices.
        for i in range(spec.size):
            pool.free.put(i)
        pool.initialized = True

        if spec.keepalive_ms > 0:
            pool.ka_thread = threading.Thread(
                target=self._pool_keepalive_loop, args=(pool,),
                name=f"bpclient-pool-keepalive-{spec.slot}", daemon=True)
            pool.ka_thread.start()

    def pool_txrx(self, slot: int, req: bytes, resp_capacity: int) -> bytes:
        """Send one CIP request via a pool connection to ``slot``.

        Returns the raw CIP reply bytes.  Blocks if all pool entries
        are in flight.  Raises BpNotOpen if no pool is open for this
        slot (including during pool_close).
        """
        if not req:
            raise BpNullArg("req is required")
        if resp_capacity <= 0:
            raise BpNullArg("resp_capacity must be > 0")
        if slot > P.MSG_MAX_SLOT:
            raise BpSlotTooLarge(f"slot {slot} > {P.MSG_MAX_SLOT}")

        with self._pools_mu:
            pool = self._pools.get(slot)
        if pool is None or not pool.initialized:
            raise BpNotOpen(f"no pool open for slot {slot}")

        with pool.inflight_lock:
            pool.inflight += 1
        try:
            # Acquire a non-dead entry.  If we pull a dead entry from
            # the queue, put it back (keepalive auto-reopens) and try
            # again.  Cap at pool.size to avoid spinning when all are
            # dead.
            idx = None
            attempts = 0
            while True:
                try:
                    idx = pool.free.get(timeout=0.5)
                except Exception:
                    if not pool.initialized:
                        raise BpNotOpen(f"pool for slot {slot} is closing")
                    continue
                if not pool.entries[idx].dead:
                    break
                # Return dead entry; keepalive will reopen it.
                if pool.initialized:
                    pool.free.put(idx)
                attempts += 1
                if attempts >= pool.size:
                    raise BpGeneric(
                        f"pool_txrx slot={slot}: all {pool.size} pool "
                        "entries are dead (keepalive auto-reopen still "
                        "in backoff)")
            try:
                entry = pool.entries[idx]
                cs = ConnSpec(
                    app_handle=entry.app_handle,
                    encoded_path=bytes([0x01, slot]),
                    path_size=2,
                    conn_params=pool.conn_params,
                )
                resp = self.txrx_msg(cs, req, resp_capacity)
            finally:
                entry.last_used = time.monotonic()
                if pool.initialized:
                    pool.free.put(idx)
        finally:
            with pool.inflight_lock:
                pool.inflight -= 1
                if pool.inflight == 0:
                    pool.inflight_zero.notify_all()
        return resp

    def pool_batch(self, slot: int, reqs: "list[bytes]",
                    resp_capacity: int) -> "list[tuple[bytes | None, BaseException | None]]":
        """Concurrently dispatch a batch of requests through the pool.

        Spawns ``min(pool.size, len(reqs))`` worker threads, each pulls
        from a shared index counter and calls ``pool_txrx`` for one
        request.  Blocks until all complete.

        Returns a list of ``(resp_bytes, exception)`` tuples in the
        same order as ``reqs``; exactly one of each pair is None.

        Raises BpNotOpen if no pool is open for ``slot``.
        """
        if reqs is None:
            raise BpNullArg("reqs is required")
        if slot > P.MSG_MAX_SLOT:
            raise BpSlotTooLarge(f"slot {slot} > {P.MSG_MAX_SLOT}")
        if not reqs:
            return []

        with self._pools_mu:
            pool = self._pools.get(slot)
        if pool is None or not pool.initialized:
            raise BpNotOpen(f"no pool open for slot {slot}")

        worker_count = min(pool.size, len(reqs))
        results: list[tuple[bytes | None, BaseException | None]] = \
            [(None, None)] * len(reqs)
        idx_lock = threading.Lock()
        next_idx = [0]

        def worker() -> None:
            while True:
                with idx_lock:
                    i = next_idx[0]
                    next_idx[0] += 1
                if i >= len(reqs):
                    return
                try:
                    resp = self.pool_txrx(slot, reqs[i], resp_capacity)
                    results[i] = (resp, None)
                except BaseException as e:
                    results[i] = (None, e)

        threads = [threading.Thread(target=worker,
                                     name=f"bpclient-pool-batch-{w}")
                   for w in range(worker_count)]
        for t in threads: t.start()
        for t in threads: t.join()
        return results

    def pool_close(self, slot: int) -> None:
        """Stop the keepalive thread, send Forward_Close on every
        pool entry, free the state.  In-flight calls finish first.

        Idempotent: closing a non-existent pool is a no-op.
        """
        if slot >= P.POOL_MAX_SLOTS:
            raise BpSlotTooLarge(f"slot {slot} > {P.POOL_MAX_SLOTS - 1}")

        with self._pools_mu:
            pool = self._pools.pop(slot, None)
        if pool is None:
            return

        pool.initialized = False
        pool.ka_stop.set()
        if pool.ka_thread is not None:
            pool.ka_thread.join()

        # Wait for in-flight calls to drain.
        with pool.inflight_lock:
            while pool.inflight > 0:
                pool.inflight_zero.wait()

        for entry in pool.entries:
            cs = ConnSpec(
                app_handle=entry.app_handle,
                encoded_path=bytes([0x01, slot]),
                path_size=2,
                conn_params=pool.conn_params,
            )
            try:
                self.txrx_close(cs)
            except BpNotOpen:
                pass            # underlying state already gone
            except Exception:
                pass            # CIP-layer FC failure is logged inside txrx_close

    def _pool_keepalive_loop(self, pool: "_Pool") -> None:
        """Periodic Identity GetAttributesAll ping on idle pool entries.

        Mirrors c/src/pool.c::pool_keepalive_loop and the sibling
        apex2d daemon's slot_pool_keepalive_idle (apex2_cip_connection.c:3287).
        """
        interval_ms = max(500, min(5000, pool.keepalive_ms // 2))
        interval_s = interval_ms / 1000.0
        identity_req = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
        while pool.initialized:
            # ka_stop.wait returns True if set (pool_close called) —
            # exit promptly instead of waiting out the full interval.
            if pool.ka_stop.wait(interval_s):
                break
            if not pool.initialized:
                break
            now = time.monotonic()
            idle_threshold_s = pool.keepalive_ms / 1000.0
            for i, entry in enumerate(pool.entries):
                if entry.dead:
                    continue
                if (now - entry.last_used) < idle_threshold_s:
                    continue
                # Try to acquire this specific entry by draining one
                # slot from the free queue; if we get a different
                # index, put it back and skip (real traffic counts
                # as a ping for that one).
                try:
                    got = pool.free.get_nowait()
                except Exception:
                    continue
                if got != i:
                    if pool.initialized:
                        pool.free.put(got)
                    continue
                try:
                    cs = ConnSpec(
                        app_handle=entry.app_handle,
                        encoded_path=bytes([0x01, pool.slot]),
                        path_size=2,
                        conn_params=pool.conn_params,
                    )
                    self.txrx_msg(cs, identity_req, 64)
                except Exception as e:
                    entry.dead = True
                    entry.last_reopen = time.monotonic()
                    if entry.reopen_backoff_ms == 0:
                        entry.reopen_backoff_ms = 1000
                    print(f"[pool keepalive] slot={pool.slot} entry {i} "
                          f"ping failed: {e} — entry marked dead",
                          file=sys.stderr)
                finally:
                    entry.last_used = time.monotonic()
                    if pool.initialized:
                        pool.free.put(i)

            # v0.9.0 Phase 4 auto-reopen pass.  For each dead entry
            # whose backoff has elapsed, force-close the stale local
            # conn-state and re-issue Forward_Open with the same
            # app_handle.  On success: dead=False, backoff resets.
            for i, entry in enumerate(pool.entries):
                if not entry.dead:
                    continue
                now2 = time.monotonic()
                since_ms = (now2 - entry.last_reopen) * 1000
                if since_ms < entry.reopen_backoff_ms:
                    continue
                # Claim the entry slot from free (non-blocking).  Skip
                # this tick if we can't.
                try:
                    got = pool.free.get_nowait()
                except Exception:
                    continue
                if got != i:
                    if pool.initialized:
                        pool.free.put(got)
                    continue
                entry.last_reopen = now2
                self._force_close_local(entry.app_handle)
                try:
                    cs = ConnSpec(
                        app_handle=entry.app_handle,
                        encoded_path=bytes([0x01, pool.slot]),
                        path_size=2,
                        conn_params=pool.conn_params,
                    )
                    self.txrx_open(cs)
                    entry.dead = False
                    entry.last_used = time.monotonic()
                    entry.reopen_backoff_ms = 1000
                    print(f"[pool keepalive] slot={pool.slot} entry {i} "
                          f"auto-reopen OK", file=sys.stderr)
                except Exception as oe:
                    nxt = min(entry.reopen_backoff_ms * 2, 30000)
                    entry.reopen_backoff_ms = nxt
                    print(f"[pool keepalive] slot={pool.slot} entry {i} "
                          f"auto-reopen failed: {oe} — next attempt in "
                          f"{nxt} ms", file=sys.stderr)
                finally:
                    if pool.initialized:
                        pool.free.put(i)


@dataclass
class ParsedPath:
    """Result of Client.parse_path (v0.10.0+).  Mirrors C
    bp_parsed_path_t and Go ocxbp.ParsedPath."""
    encoded: bytes = b""
    cip_class: int = 0
    segment_flags: int = 0
    instance: int = 0
    attr_flags: int = 0


@dataclass
class PoolSpec:
    """Configuration for a per-slot connection pool.

    Recommended defaults: ``size=4``, ``keepalive_ms=10000``,
    ``conn_params=0`` (SDK default 4000 B).
    """
    slot: int = 0
    size: int = 4
    keepalive_ms: int = 0      # 0 disables the keepalive thread
    conn_params: int = 0


@dataclass
class _PoolEntry:
    app_handle: int
    last_used: float
    dead: bool = False
    # v0.9.0 Phase 4 — auto-reopen bookkeeping.  Keepalive retries
    # a dead entry with exponential backoff (1 s → 2 s → 4 s → ...
    # cap 30 s).
    last_reopen: float = 0.0
    reopen_backoff_ms: int = 0


class _Pool:
    """Internal per-slot pool state."""
    def __init__(self, slot: int, size: int,
                 keepalive_ms: int, conn_params: int) -> None:
        import queue
        self.slot = slot
        self.size = size
        self.keepalive_ms = keepalive_ms
        self.conn_params = conn_params
        self.entries: list[_PoolEntry] = []
        self.free: "queue.Queue[int]" = queue.Queue(maxsize=size)
        self.initialized = False
        self.ka_thread: threading.Thread | None = None
        # ka_stop is set in pool_close to interrupt the keepalive sleep
        # so we don't wait up to keepalive_ms/2 ms on Python's
        # time.sleep — without this the join() in pool_close stalls.
        self.ka_stop = threading.Event()
        self.inflight = 0
        self.inflight_lock = threading.Lock()
        self.inflight_zero = threading.Condition(self.inflight_lock)


class _TagCache:
    """Per-PLC symbol cache (v0.9.0).  Mirrors C's struct bp_tag_cache
    and Go's tagCache."""

    def __init__(self) -> None:
        self.mu = threading.Lock()
        self.symbols: "list[Symbol]" = []
        self.known_count = 0
        self.total_count = 0


def _bind_tag_cache_methods() -> None:
    """Attach the cache helpers to Client lazily, after _TagCache is
    defined.  Python class bodies are evaluated top-to-bottom; binding
    here avoids forward-reference gymnastics."""
    from .tagdb import Symbol  # noqa: E402

    def _find_or_alloc_tag_cache(self, path: str) -> _TagCache:
        with self._tag_cache_mu:
            tc = self._tag_caches.get(path)
            if tc is None:
                tc = _TagCache()
                self._tag_caches[path] = tc
            return tc

    def _reset_tag_cache_after_build(self, path: str, total_count: int) -> None:
        tc = self._find_or_alloc_tag_cache(path)
        with tc.mu:
            tc.symbols = [Symbol() for _ in range(total_count)] if total_count > 0 else []
            tc.known_count = 0
            tc.total_count = total_count

    def _lookup_cached_symbol(self, db, name: str) -> Symbol:
        tc = self._find_or_alloc_tag_cache(db.path)
        tc.mu.acquire()
        try:
            if tc.total_count == 0 and not tc.symbols:
                raise BpParamRange(
                    "lookup before build (or PLC has no tags)")
            for i in range(tc.known_count):
                if tc.symbols[i].name == name:
                    return tc.symbols[i]
            # Lazy fill: walk symbol_at until we find a match.  Release
            # the cache mutex around each IPC call so parallel lookups
            # for already-cached names don't block on us.
            while tc.known_count < tc.total_count:
                idx = tc.known_count
                tc.mu.release()
                try:
                    sym = db.symbol_at(idx)
                finally:
                    tc.mu.acquire()
                if idx == tc.known_count and tc.known_count < len(tc.symbols):
                    tc.symbols[tc.known_count] = sym
                    tc.known_count += 1
                for i in range(idx, tc.known_count):
                    if tc.symbols[i].name == name:
                        return tc.symbols[i]
            raise BpParamRange(
                f"symbol {name!r} not found in PLC tag table")
        finally:
            tc.mu.release()

    def _preload_cached_symbols(self, db) -> int:
        tc = self._find_or_alloc_tag_cache(db.path)
        tc.mu.acquire()
        try:
            if tc.total_count == 0 and not tc.symbols:
                raise BpParamRange(
                    "preload before build (or PLC has no tags)")
            while tc.known_count < tc.total_count:
                idx = tc.known_count
                tc.mu.release()
                try:
                    sym = db.symbol_at(idx)
                finally:
                    tc.mu.acquire()
                if idx == tc.known_count and tc.known_count < len(tc.symbols):
                    tc.symbols[tc.known_count] = sym
                    tc.known_count += 1
            return tc.known_count
        finally:
            tc.mu.release()

    Client._find_or_alloc_tag_cache = _find_or_alloc_tag_cache
    Client._reset_tag_cache_after_build = _reset_tag_cache_after_build
    Client._lookup_cached_symbol = _lookup_cached_symbol
    Client._preload_cached_symbols = _preload_cached_symbols


_bind_tag_cache_methods()


# Re-import to avoid a circular ref at module top.
from .tagdb import TagDB  # noqa: E402,F401
