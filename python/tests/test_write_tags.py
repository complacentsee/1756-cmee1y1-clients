"""Unit tests for db.write_tags (v0.9.0 Phase 3).

Uses the same FakeClient / FakeDB harness as test_read_tags.py but
captures the AccessTagData WRITE payload bytes so we can verify the
encoder produces correct wire bytes and rejects type mismatches
before any IPC.

SPDX-License-Identifier: MIT
"""
import importlib.util
import struct
import sys
import threading
import types
from pathlib import Path

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

proto = _load_submodule("bpclient", "_proto", "_proto.py")
errors = _load_submodule("bpclient", "errors", "errors.py")
access = _load_submodule("bpclient", "access", "access.py")
tagdb = _load_submodule("bpclient", "tagdb", "tagdb.py")


class FakeClient:
    def __init__(self):
        self._tag_cache_mu = threading.Lock()
        self._tag_caches = {}
        self.last_write_payload: dict[str, bytes] = {}
        self.access_call_count = 0

    @property
    def raw(self):
        return self

    def call(self, fn_name, payload_size, fill=None, read=None, timeout_ms=0):
        slot = bytearray(proto.SLOT_STRIDE)
        if fill:
            fill(slot)
        if fn_name == "OCXcip_AccessTagData":
            self.access_call_count += 1
            count = struct.unpack_from("<H", slot, proto.TAGDATA_COUNT_OFF)[0]
            data_off = 0x2A0 + (count - 1) * proto.TAGDATA_REQ_STRIDE
            for i in range(count):
                req_start = proto.TAGDATA_REQ0_START + i * proto.TAGDATA_REQ_STRIDE
                name_bytes = slot[req_start:req_start + 256]
                name_end = name_bytes.find(b"\x00")
                name = name_bytes[:name_end].decode("ascii")
                dt, ebs, action, n = struct.unpack_from(
                    "<HHHH", slot, req_start + proto.REQ_DATATYPE_OFF)
                nbytes = ebs * n
                self.last_write_payload[name] = bytes(slot[data_off:data_off + nbytes])
                data_off += nbytes
        if read:
            read(slot)

    def _find_or_alloc_tag_cache(self, path):
        with self._tag_cache_mu:
            tc = self._tag_caches.get(path)
            if tc is None:
                tc = _Cache()
                self._tag_caches[path] = tc
            return tc

    def _reset_tag_cache_after_build(self, path, total_count):
        tc = self._find_or_alloc_tag_cache(path)
        with tc.mu:
            tc.symbols = [tagdb.Symbol() for _ in range(total_count)] if total_count > 0 else []
            tc.known_count = 0
            tc.total_count = total_count

    def _lookup_cached_symbol(self, db, name):
        tc = self._find_or_alloc_tag_cache(db.path)
        tc.mu.acquire()
        try:
            for i in range(tc.known_count):
                if tc.symbols[i].name == name:
                    return tc.symbols[i]
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
            raise errors.BpParamRange(f"symbol {name!r} not found")
        finally:
            tc.mu.release()


class _Cache:
    def __init__(self):
        self.mu = threading.Lock()
        self.symbols = []
        self.known_count = 0
        self.total_count = 0


class FakeDB(tagdb.TagDB):
    def __init__(self, client, path, symbols):
        super().__init__(client, handle=1, path=path)
        self._symbols = symbols
        client._reset_tag_cache_after_build(path, len(symbols))
        tc = client._find_or_alloc_tag_cache(path)
        with tc.mu:
            for i, s in enumerate(symbols):
                tc.symbols[i] = s
            tc.known_count = len(symbols)

    def symbol_at(self, index):
        return self._symbols[index]


def _make_symbols():
    return [
        tagdb.Symbol(name="A_BOOL",  data_type=proto.TYPE_BOOL,  elem_byte_size=1),
        tagdb.Symbol(name="B_DINT",  data_type=proto.TYPE_DINT,  elem_byte_size=4),
        tagdb.Symbol(name="C_REAL",  data_type=proto.TYPE_REAL,  elem_byte_size=4),
        tagdb.Symbol(name="D_UDINT", data_type=proto.TYPE_UDINT, elem_byte_size=4),
    ]


# ---- tests ----

def test_write_tags_encodes_scalars():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    db.write_tags({
        "A_BOOL": True,
        "B_DINT": -559038737,           # 0xDEADBEEF
        "C_REAL": 3.5,
        "D_UDINT": 42,
    })
    assert c.access_call_count == 1
    assert c.last_write_payload["A_BOOL"] == b"\x01"
    assert c.last_write_payload["B_DINT"] == struct.pack("<i", -559038737)
    assert c.last_write_payload["C_REAL"] == struct.pack("<f", 3.5)
    assert c.last_write_payload["D_UDINT"] == struct.pack("<I", 42)


def test_write_tags_rejects_bool_for_int_tag():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    try:
        db.write_tags({"B_DINT": True})  # bool not allowed for DINT
    except errors.BpParamRange as e:
        assert c.access_call_count == 0  # rejected pre-IPC
        assert "B_DINT" in str(e)
        return
    raise AssertionError("expected BpParamRange for bool-into-DINT")


def test_write_tags_rejects_float_for_int_tag():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    try:
        db.write_tags({"B_DINT": 3.5})
    except errors.BpParamRange:
        assert c.access_call_count == 0
        return
    raise AssertionError("expected BpParamRange for float-into-DINT")


def test_write_tags_rejects_int_for_bool_tag():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    try:
        db.write_tags({"A_BOOL": 1})  # int not allowed for BOOL
    except errors.BpParamRange:
        assert c.access_call_count == 0
        return
    raise AssertionError("expected BpParamRange for int-into-BOOL")


def test_write_tags_rejects_out_of_range_int():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    try:
        db.write_tags({"D_UDINT": -1})  # negative into UDINT
    except errors.BpParamRange:
        return
    raise AssertionError("expected BpParamRange for -1 into UDINT")

    try:
        db.write_tags({"B_DINT": 1 << 31})  # overflows int32
    except errors.BpParamRange:
        return
    raise AssertionError("expected BpParamRange for 2^31 into DINT")


def test_write_tags_accepts_int_for_real():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    db.write_tags({"C_REAL": 5})  # int → float promotion ok
    assert c.last_write_payload["C_REAL"] == struct.pack("<f", 5.0)


def test_write_tags_partial_failure_carries_statuses():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())

    # Inject a CIP failure on the second tag.
    real_call = c.call

    def call_wrapper(fn_name, payload_size, fill=None, read=None, timeout_ms=0):
        slot = bytearray(proto.SLOT_STRIDE)
        if fill: fill(slot)
        if fn_name == "OCXcip_AccessTagData":
            count = struct.unpack_from("<H", slot, proto.TAGDATA_COUNT_OFF)[0]
            for i in range(count):
                req_start = proto.TAGDATA_REQ0_START + i * proto.TAGDATA_REQ_STRIDE
                if i == 1:  # second request fails
                    struct.pack_into("<I", slot, req_start + proto.REQ_RESULT_OFF, 0x05)
        if read: read(slot)
    c.call = call_wrapper

    try:
        db.write_tags({"A_BOOL": True, "B_DINT": 7})
    except errors.BpGeneric as e:
        assert "B_DINT" in str(e)
        assert e.statuses["B_DINT"] == 0x05
        assert e.statuses["A_BOOL"] == 0
        return
    raise AssertionError("expected BpGeneric for partial failure")


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  OK  {name}")
    print("all write_tags tests passed")
