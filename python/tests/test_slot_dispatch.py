"""Unit tests for the slot fill/read encoders.  Bypasses the real
IPC by monkey-patching Client.call with a synthetic dispatcher;
verifies the public API produces the correct request bytes and
correctly parses synthetic response bytes.

Works on any host (no PLC, no /dev/shm/bpShmem, no posix_ipc
runtime) — runs in CI.

SPDX-License-Identifier: MIT
"""
import struct
import sys
from pathlib import Path

# Add src/ to path for in-repo testing without installation.
sys.path.insert(0, str(Path(__file__).resolve().parent.parent / "src"))

import bpclient  # noqa: E402
from bpclient import _proto as P  # noqa: E402


class FakeRaw:
    """Substitute for bpclient._ipc.Client that captures the fill
    callback's slot bytes and lets the test pre-load the response
    bytes before invoking the read callback."""

    def __init__(self):
        self.slot = bytearray(P.SLOT_STRIDE)
        self.last_fn = ""
        self.last_payload_size = 0
        self.response_loader = None  # set by tests

    def open(self):
        pass

    def close(self):
        pass

    def call(self, fn_name, payload_size, fill=None, read=None, timeout_ms=30000):
        # Mirror the dispatcher's header pre-write + clear so the
        # fill callback sees the same slot shape it would in
        # production.
        self.slot[:] = b"\x00" * P.SLOT_STRIDE
        struct.pack_into("<HHI", self.slot, P.HDR_OPCODE,
                         P.OPCODE_CIP, 0, payload_size)
        fn_bytes = fn_name.encode("ascii")[:63]
        self.slot[P.HDR_FN_NAME:P.HDR_FN_NAME + len(fn_bytes)] = fn_bytes

        self.last_fn = fn_name
        self.last_payload_size = payload_size

        if fill is not None:
            fill(self.slot)

        # The "engine" loads its synthetic response over the same slot bytes.
        if self.response_loader is not None:
            self.response_loader(self.slot)

        if read is not None:
            read(self.slot)


def _make_client_with_fake() -> tuple[bpclient.Client, FakeRaw]:
    c = bpclient.Client()
    fake = FakeRaw()
    c._raw = fake  # type: ignore[attr-defined]
    return c, fake


def test_open_session_dispatch():
    c, fake = _make_client_with_fake()

    def load(slot):
        # Engine writes the session handle at +0x78.
        struct.pack_into("<I", slot, P.HDR_PAYLOAD_START, 0xCAFEBABE)

    fake.response_loader = load
    handle = c.open_session()
    assert handle == 0xCAFEBABE
    assert fake.last_fn == "OCXcip_Open"
    assert fake.last_payload_size == 0x80


def test_message_send_writes_corrected_field_names():
    """Phase 4 wire correction: slot at +0x32080, timeout_ms at +0x32082,
    cip_request copied verbatim at +0x78 (not just an EPATH)."""
    c, fake = _make_client_with_fake()

    cip_req = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
    msg = bpclient.Message(
        slot=2,
        cip_request=cip_req,
        timeout_ms=1000,
        resp_capacity=256,
    )

    def load(slot):
        # Synthesize a Get_Attributes_All response (Identity reply).
        resp = bytes([0x81, 0x00, 0x00, 0x00,  # reply_svc, rsv, gen_stat, ext_sz
                      0x01, 0x00,              # vendor 0x0001
                      0x0E, 0x00,              # device_type
                      0x5E, 0x00,              # product code
                      0x15, 0x0B,              # major.minor
                      0x50, 0x35, 0xCB, 0x62,  # serial
                      0x6C, 0x00,              # status
                      0x14,                    # name_len
                      ])
        slot[P.MSGSEND_RESPDATA_OFF:P.MSGSEND_RESPDATA_OFF + len(resp)] = resp
        struct.pack_into("<H", slot, P.MSGSEND_RESPLEN_OFF, len(resp))
        struct.pack_into("<I", slot, P.MSGSEND_STATUS_OFF, 0)

    fake.response_loader = load
    c.message_send(msg)

    # Verify request bytes ended up at the right slot offsets.
    assert bytes(fake.slot[P.MSGSEND_REQ_OFF:P.MSGSEND_REQ_OFF + len(cip_req)]) == cip_req
    assert struct.unpack_from("<H", fake.slot, P.MSGSEND_REQ_SIZE_OFF)[0] == len(cip_req)
    assert fake.slot[P.MSGSEND_SLOT_OFF] == 2
    assert struct.unpack_from("<H", fake.slot, P.MSGSEND_TIMEOUT_OFF)[0] == 1000

    # Response was parsed back.
    assert msg.resp_len == 19
    assert msg.resp_data[:4] == bytes([0x81, 0x00, 0x00, 0x00])
    assert msg.status == 0


def test_accesstagdata_uses_corrected_field_offsets():
    """Phase 2 fix: elem_byte_size at +0x102, elem_count at +0x106
    (vendor header had them swapped)."""
    c, fake = _make_client_with_fake()

    from bpclient import TagDB

    db = TagDB(client=c, handle=0xAA, path="P:1,S:2")

    def load(slot):
        # Engine writes per-request result=0 and the data bytes.
        # Data area for a single 4-byte request: data_area_start
        # = 0x2A0 (since count=1, (1-1)*stride=0)
        data_off = 0x2A0
        struct.pack_into("<i", slot, data_off, 0xDEADBEEF - (1 << 32))
        struct.pack_into("<I", slot,
                         P.TAGDATA_REQ0_START + P.REQ_RESULT_OFF, 0)

    fake.response_loader = load
    v = db.read_dint("OCX_TEST")
    assert v == -559038737  # int32 of 0xDEADBEEF

    # Field-offset assertions on the captured slot.
    req_start = P.TAGDATA_REQ0_START
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_DATATYPE_OFF)[0] == P.TYPE_DINT
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ELEM_BYTE_SIZE_OFF)[0] == 4
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ACTION_OFF)[0] == P.ACTION_READ
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ELEM_COUNT_OFF)[0] == 1
    # has_extra MUST be 0 (extra-path mode crashes the engine).
    assert fake.slot[req_start + P.REQ_HAS_EXTRA_OFF] == 0


def test_strerror_known_codes():
    assert bpclient.strerror(0) == "ok"
    assert "parameter range" in bpclient.strerror(bpclient.errors.BP_ERR_PARAM_RANGE)
    assert bpclient.strerror(99999) == "unknown error"


def test_err_code_maps_exceptions_to_rc():
    assert bpclient.err_code(None) == 0
    assert bpclient.err_code(bpclient.BpParamRange("x")) == bpclient.errors.BP_ERR_PARAM_RANGE
    assert bpclient.err_code(bpclient.BpEngine(7)) == 7


def test_dint_array_roundtrip_through_fake():
    """ReadDINTArray must request count elements at the corrected
    offsets and unpack them in little-endian."""
    c, fake = _make_client_with_fake()
    from bpclient import TagDB

    db = TagDB(client=c, handle=0xAA, path="P:1,S:2")

    def load(slot):
        # 4 DINTs at the data area for a single 4-element request
        # data_area_start = 0x2A0 (count=1 so (1-1)*stride=0)
        data_off = 0x2A0
        vals = [0x11111111, 0x22222222, 0x33333333, 0x44444444]
        struct.pack_into("<4I", slot, data_off, *vals)
        struct.pack_into("<I", slot,
                         P.TAGDATA_REQ0_START + P.REQ_RESULT_OFF, 0)

    fake.response_loader = load
    out = db.read_dint_array("ArrayTag", 4)
    assert out == [0x11111111, 0x22222222, 0x33333333, 0x44444444]

    # Verify the request descriptor used the right type + count
    req_start = P.TAGDATA_REQ0_START
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_DATATYPE_OFF)[0] == P.TYPE_DINT
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ELEM_BYTE_SIZE_OFF)[0] == 4
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ELEM_COUNT_OFF)[0] == 4


def test_bool_array_packs_to_dwords():
    """BOOL[] writes must produce ceil(N/32) DWORDs with the bits
    in the documented order (bit i of DWORD i/32 = vals[i])."""
    c, fake = _make_client_with_fake()
    from bpclient import TagDB

    db = TagDB(client=c, handle=0xAA, path="P:1,S:2")

    def load(slot):
        struct.pack_into("<I", slot,
                         P.TAGDATA_REQ0_START + P.REQ_RESULT_OFF, 0)

    fake.response_loader = load

    # Pattern: bits 0, 3, 7, 31, 32, 33 set (spans two DWORDs).
    vals = [False] * 34
    for i in (0, 3, 7, 31, 32, 33):
        vals[i] = True
    db.write_bool_array("BitTag", vals)

    # Two DWORDs at the data area.
    data_off = 0x2A0
    dw0, dw1 = struct.unpack_from("<II", fake.slot, data_off)
    # DWORD 0: bits 0, 3, 7, 31
    assert dw0 == (1 << 0) | (1 << 3) | (1 << 7) | (1 << 31)
    # DWORD 1: bits 32, 33 -> within DWORD 1: bit 0, bit 1
    assert dw1 == (1 << 0) | (1 << 1)

    # Wire type + count
    req_start = P.TAGDATA_REQ0_START
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_DATATYPE_OFF)[0] == P.TYPE_BIT_ARRAY
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ELEM_BYTE_SIZE_OFF)[0] == 4
    assert struct.unpack_from("<H", fake.slot, req_start + P.REQ_ELEM_COUNT_OFF)[0] == 2  # ceil(34/32)


# ─────────────────────────────────────────────────────────────────
# v0.7.0 Phase 4: connected-messaging wire encoder tests.
# These hit the pure-Python LFO/FC encoders in _conn_wire — no
# fake IPC needed.
# ─────────────────────────────────────────────────────────────────


def test_build_forward_open_matches_protocol_md_layout():
    """Large Forward Open body bytes match docs/protocol.md exactly."""
    from bpclient import _conn_wire as cw
    from bpclient import _proto as P

    body = cw.build_forward_open(
        conn_serial=0xA5A8,
        orig_serial=0x671D8056,
        ot_size_bytes=4000,
    )
    assert len(body) == 50

    # service + path
    assert body[0] == 0x5B
    assert body[1:6] == bytes([0x02, 0x20, 0x06, 0x24, 0x01])

    # priority/tick
    assert body[6:8] == bytes([0x05, 0xF7])

    # O→T conn ID hint = 0x80010000, T→O hint = 0x80000001 (LE)
    assert struct.unpack_from("<I", body, 0x08)[0] == 0x80010000
    assert struct.unpack_from("<I", body, 0x0C)[0] == 0x80000001

    # conn serial + vendor + orig serial
    assert struct.unpack_from("<H", body, 0x10)[0] == 0xA5A8
    assert struct.unpack_from("<H", body, 0x12)[0] == P.LFO_VENDOR_ID
    assert struct.unpack_from("<I", body, 0x14)[0] == 0x671D8056

    # timeout multiplier (3) → RPI × 4
    assert struct.unpack_from("<I", body, 0x18)[0] == 0x00000003

    # OT/TO RPI + conn params dwords — both pairs identical
    assert struct.unpack_from("<I", body, 0x1C)[0] == P.LFO_RPI_US
    assert struct.unpack_from("<I", body, 0x20)[0] == (0x42000000 | 4000)
    assert struct.unpack_from("<I", body, 0x24)[0] == P.LFO_RPI_US
    assert struct.unpack_from("<I", body, 0x28)[0] == (0x42000000 | 4000)

    # transport trigger + conn path
    assert body[0x2C] == 0xA3
    assert body[0x2D:0x32] == bytes([0x02, 0x20, 0x02, 0x24, 0x01])


def test_parse_forward_open_extracts_ids_from_real_l85_reply():
    """The 30-byte capture from the connexp run (docs/protocol.md):

        db 00 00 00 35 04 02 80 01 00 00 80 ...
    """
    from bpclient import _conn_wire as cw

    resp = bytes.fromhex(
        "db00000035040280010000 80a8a5010056801d6780969800809698000000")
    ot, to, status, ok = cw.parse_forward_open(resp)
    assert ok
    assert status == 0
    assert ot == 0x80020435
    assert to == 0x80000001


def test_parse_forward_open_rejects_error_replies():
    """LFO general status != 0 → ok=False, ids zeroed."""
    from bpclient import _conn_wire as cw

    # 0xDB + status=0x02 (Resource unavailable, as seen with oversize OT)
    resp = bytes([0xDB, 0x00, 0x02, 0x00, 0, 0, 0, 0, 0, 0, 0, 0])
    ot, to, status, ok = cw.parse_forward_open(resp)
    assert not ok
    assert status == 0x02
    assert ot == 0 and to == 0


def test_build_forward_close_matches_protocol_md_layout():
    from bpclient import _conn_wire as cw
    from bpclient import _proto as P

    body = cw.build_forward_close(
        conn_serial=0xA5A8,
        vendor_id=P.LFO_VENDOR_ID,
        orig_serial=0x671D8056,
    )
    assert len(body) == 22
    assert body[0] == 0x4E
    assert body[1:6] == bytes([0x02, 0x20, 0x06, 0x24, 0x01])
    assert body[6:8] == bytes([0x0A, 0x0E])  # priority/tick
    assert struct.unpack_from("<H", body, 0x08)[0] == 0xA5A8
    assert struct.unpack_from("<H", body, 0x0A)[0] == P.LFO_VENDOR_ID
    assert struct.unpack_from("<I", body, 0x0C)[0] == 0x671D8056
    assert body[0x10:0x16] == bytes([0x02, 0x00, 0x20, 0x02, 0x24, 0x01])


def test_parse_forward_close_accepts_ce_status_zero():
    from bpclient import _conn_wire as cw

    # The 14-byte capture: ce 00 00 00 a8 a5 01 00 56 80 1d 67 00 00
    resp = bytes.fromhex("ce0000 00a8a5010056801d670000".replace(" ", ""))
    status, ok = cw.parse_forward_close(resp)
    assert ok
    assert status == 0


def test_extract_slot_only_accepts_backplane_direct():
    from bpclient import _conn_wire as cw

    assert cw.extract_slot(b"\x01\x02", 2) == 2
    assert cw.extract_slot(b"\x01\x00", 2) == 0
    # Wrong port byte
    assert cw.extract_slot(b"\x02\x02", 2) is None
    # Wrong path_size
    assert cw.extract_slot(b"\x01\x02\x03\x04", 4) is None
    # Too-short buffer
    assert cw.extract_slot(b"\x01", 2) is None


def test_txrx_msg_passes_request_unmodified_no_seq_prepended():
    """v0.7.0 contract: txrx_msg sends caller bytes byte-for-byte.
    No connection ID or sequence number is prepended (sibling RE
    established that the chip's MBOX_LOOPBACK transport handles
    framing transparently — see docs/protocol.md "Sequence numbers
    are NOT prepended")."""
    c, fake = _make_client_with_fake()

    # Stage cached connection state directly so we skip the LFO round-trip.
    from bpclient.client import _TxRxState
    c._txrx_conns[42] = _TxRxState(
        slot=2, conn_serial=0xBEEF, vendor_id=1, orig_serial=0xDEADBEEF,
        ot_conn_id=0x80020435, to_conn_id=0x80000001,
    )

    spec = bpclient.ConnSpec(
        app_handle=42, encoded_path=b"\x01\x02", path_size=2)
    cip_req = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])

    def load(slot):
        resp = bytes([0x81, 0x00, 0x00, 0x00, 0x01, 0x00])
        slot[P.MSGSEND_RESPDATA_OFF:P.MSGSEND_RESPDATA_OFF + len(resp)] = resp
        struct.pack_into("<H", slot, P.MSGSEND_RESPLEN_OFF, len(resp))
        struct.pack_into("<I", slot, P.MSGSEND_STATUS_OFF, 0)

    fake.response_loader = load
    resp = c.txrx_msg(spec, cip_req, 256)

    # The fake MessageSend dispatcher should have copied cip_req
    # verbatim — same offset as plain MessageSend, NO conn ID /
    # seq prefix bytes.
    assert bytes(fake.slot[P.MSGSEND_REQ_OFF:P.MSGSEND_REQ_OFF + len(cip_req)]) == cip_req
    assert struct.unpack_from("<H", fake.slot, P.MSGSEND_REQ_SIZE_OFF)[0] == len(cip_req)
    assert fake.slot[P.MSGSEND_SLOT_OFF] == 2
    # Sequence counter advanced (diagnostic only).
    assert c._txrx_conns[42].sequence == 1
    # Reply propagated.
    assert resp[:4] == bytes([0x81, 0x00, 0x00, 0x00])
