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
            # Wait for a free entry; if pool is closing the queue will
            # be drained and we re-check initialized.
            while True:
                try:
                    idx = pool.free.get(timeout=0.5)
                    break
                except Exception:
                    if not pool.initialized:
                        raise BpNotOpen(f"pool for slot {slot} is closing")
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
                    print(f"[pool keepalive] slot={pool.slot} entry {i} "
                          f"ping failed: {e} — entry marked dead",
                          file=sys.stderr)
                finally:
                    entry.last_used = time.monotonic()
                    if pool.initialized:
                        pool.free.put(i)


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


# Re-import to avoid a circular ref at module top.
from .tagdb import TagDB  # noqa: E402,F401
