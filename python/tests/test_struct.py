"""Offline unit tests for the structured (whole-UDT) accessors.

Mirrors go/ocxbp/struct_test.go.  No bpServer / PLC, and (like
test_read_tags.py / test_cip_error.py) no posix_ipc: the bpclient
submodules are loaded directly via importlib so the test runs on any
host.  A FakeClient captures the CIP request bytes and returns a
canned reply, so we assert on the exact wire framing the accessor
builds, the decode of a known reply (type/handle prefix), and the
pre-IPC arg guards.

SPDX-License-Identifier: MIT
"""
import importlib.util
import struct
import sys
import types
from pathlib import Path

import pytest

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"


def _load_submodule(parent_pkg_name, sub_name, rel_path):
    full_name = f"{parent_pkg_name}.{sub_name}"
    spec = importlib.util.spec_from_file_location(
        full_name, SRC / parent_pkg_name / rel_path)
    m = importlib.util.module_from_spec(spec)
    sys.modules[full_name] = m
    spec.loader.exec_module(m)
    return m


pkg = types.ModuleType("bpclient")
pkg.__path__ = [str(SRC / "bpclient")]
sys.modules["bpclient"] = pkg

_proto = _load_submodule("bpclient", "_proto", "_proto.py")
errors = _load_submodule("bpclient", "errors", "errors.py")
message = _load_submodule("bpclient", "message", "message.py")
struct_mod = _load_submodule("bpclient", "struct", "struct.py")

Message = message.Message
read_struct = struct_mod.read_struct
write_struct = struct_mod.write_struct
_sym_path = struct_mod._sym_path
StructTypeAbbrev = struct_mod.StructTypeAbbrev
BpCipError = errors.BpCipError
BpNullArg = errors.BpNullArg
BpParamRange = errors.BpParamRange


class FakeClient:
    """Captures the Message passed to message_send and fills a canned reply."""

    def __init__(self, reply: bytes):
        self.reply = reply
        self.sent = None

    def message_send(self, m: "Message") -> None:
        self.sent = m
        m.resp_data = self.reply
        m.resp_len = len(self.reply)


# ---- symbolic-path encoding (mirrors TestSymbolicIOIPath) ----


def test_sym_path_even():
    # even-length name (4): no pad. 0x91,len + 4 = 6 bytes = 3 words.
    path, words = _sym_path("TSEQ")
    assert path == bytes([0x91, 0x04, ord("T"), ord("S"), ord("E"), ord("Q")])
    assert words == 3
    assert len(path) % 2 == 0
    assert words * 2 == len(path)


def test_sym_path_odd():
    # odd-length name (3): trailing NUL pad. 2+3+1 = 6 bytes = 3 words.
    path, words = _sym_path("Abc")
    assert path == bytes([0x91, 0x03, ord("A"), ord("b"), ord("c"), 0x00])
    assert words == 3
    assert len(path) % 2 == 0
    assert words * 2 == len(path)


def test_sym_path_real_suf_register():
    # the real SUF register (26 chars, even): 2+26 = 28 bytes = 14 words.
    name = "Tran_From_iSeries_Register"
    path, words = _sym_path(name)
    assert words == 14
    assert len(path) % 2 == 0
    assert words * 2 == len(path)
    assert path[0] == 0x91 and path[1] == len(name)


# ---- read_struct framing + decode ----


def test_read_struct_frames_request_and_decodes():
    # reply: svc, 0, status=0, ext=0, then [type 0x02A0][handle 0x0A2C][payload]
    payload = bytes(range(8))
    reply = bytes([0xCC, 0x00, 0x00, 0x00]) + struct.pack("<HH", StructTypeAbbrev, 0x0A2C) + payload
    c = FakeClient(reply)
    data, handle = read_struct(c, 2, "TSEQ", 600)
    assert handle == 0x0A2C
    assert data == payload
    # request framing: service 0x4C, words, path, elem_count=1
    req = c.sent.cip_request
    assert req[0] == 0x4C
    assert req[1] == 3  # "TSEQ" -> 3 words
    assert req[2:6] == bytes([0x91, 0x04, ord("T"), ord("S")])
    assert req[-2:] == bytes([0x01, 0x00])


def test_read_struct_cip_error_raises():
    # status byte non-zero -> BpCipError (carries the wire fields)
    reply = bytes([0xCC, 0x00, 0x05, 0x00])
    c = FakeClient(reply)
    with pytest.raises(BpCipError) as ei:
        read_struct(c, 2, "MissingTag", 600)
    assert ei.value.status == 0x05
    assert ei.value.slot == 2
    assert c.sent is not None


# ---- write_struct framing + handle echo ----


def test_write_struct_frames_request_with_handle_echo():
    # write reply: svc=0xCD, 0, status=0, ext=0 (no payload)
    reply = bytes([0xCD, 0x00, 0x00, 0x00])
    c = FakeClient(reply)
    data = bytes([0xDE, 0xAD, 0xBE, 0xEF])
    write_struct(c, 2, "TSEQ", 0x0A2C, data)
    req = c.sent.cip_request
    assert req[0] == 0x4D
    assert req[1] == 3  # "TSEQ" -> 3 words
    # tail: type(0x02A0) + handle(0x0A2C) + elem_count(1) + payload
    assert req[-(len(data) + 6):] == struct.pack("<HHH", StructTypeAbbrev, 0x0A2C, 1) + data


def test_write_struct_cip_error_raises():
    reply = bytes([0xCD, 0x00, 0x0F, 0x00])
    c = FakeClient(reply)
    with pytest.raises(BpCipError) as ei:
        write_struct(c, 2, "TSEQ", 0x0A2C, bytes([1]))
    assert ei.value.status == 0x0F


# ---- arg-validation guards (mirror TestReadWriteStructArgValidation) ----


def test_read_struct_nil_client():
    with pytest.raises(BpNullArg):
        read_struct(None, 2, "Tag", 600)


def test_write_struct_nil_client():
    with pytest.raises(BpNullArg):
        write_struct(None, 2, "Tag", 0x0A2C, bytes([1]))


def test_read_struct_param_range():
    with pytest.raises(BpParamRange):
        read_struct(FakeClient(b""), 2, "x" * 251, 600)
    with pytest.raises(BpParamRange):
        read_struct(FakeClient(b""), 2, "Tag", 8)  # resp_cap < 16


def test_write_struct_param_range_empty_data():
    with pytest.raises(BpParamRange):
        write_struct(FakeClient(b""), 2, "Tag", 0x0A2C, b"")


def test_write_struct_param_range_over_cap():
    # request must fit MSG_MAX_REQ (500); an oversized payload is
    # rejected pre-IPC with BpParamRange, not a truncated write.
    c = FakeClient(bytes([0xCD, 0x00, 0x00, 0x00]))
    with pytest.raises(BpParamRange):
        write_struct(c, 2, "Tag", 0x0A2C, bytes(600))
    # guard fires before any IPC
    assert c.sent is None


if __name__ == "__main__":
    # Lightweight self-test for hosts without pytest installed.
    failures = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  OK  {name}")
            except Exception as e:  # noqa: BLE001
                failures += 1
                print(f"FAIL  {name}: {e!r}")
    if failures:
        print(f"{failures} struct test(s) FAILED")
        sys.exit(1)
    print("all struct tests passed")
