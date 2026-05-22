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
