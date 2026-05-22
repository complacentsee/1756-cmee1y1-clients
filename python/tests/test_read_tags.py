"""Unit tests for db.read_tags (v0.9.0 Phase 2).

Bypasses the IPC layer with a fake AccessTagData implementation that
populates per-request `.data` and `.result` from a test-controlled
table.  Verifies type dispatch, whole-batch error semantics, chunking
at 16-request boundaries, and pre-IPC rejection of arrays/UDTs/unknowns.

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
    """Subset of the Client surface used by TagDB.  Exposes the cache
    helpers and a fake `raw.call` that lets the test inject behaviour."""

    def __init__(self):
        self._tag_cache_mu = threading.Lock()
        self._tag_caches = {}
        # response_provider is called by raw.call with (fn_name, slot)
        # before the read callback runs.  Tests use it to populate
        # AccessTagData responses.
        self.response_provider = None
        # Each AccessTagData call records its descriptor list here.
        self.last_access_descriptors = []

    @property
    def raw(self):
        return self

    def call(self, fn_name, payload_size, fill=None, read=None, timeout_ms=0):
        slot = bytearray(proto.SLOT_STRIDE)
        if fill:
            fill(slot)
        if fn_name == "OCXcip_AccessTagData":
            count = struct.unpack_from("<H", slot, proto.TAGDATA_COUNT_OFF)[0]
            descs = []
            for i in range(count):
                req_start = proto.TAGDATA_REQ0_START + i * proto.TAGDATA_REQ_STRIDE
                name_bytes = slot[req_start:req_start + 256]
                name_end = name_bytes.find(b"\x00")
                name = name_bytes[:name_end].decode("ascii")
                dt, ebs, action, n = struct.unpack_from(
                    "<HHHH", slot, req_start + proto.REQ_DATATYPE_OFF)
                descs.append((name, dt, ebs, action, n, req_start))
            self.last_access_descriptors = descs
            # Inject test response.
            if self.response_provider:
                self.response_provider(slot, descs)
        if read:
            read(slot)

    # cache helpers (copied from the real Client; bound via test setup)
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
    """A TagDB pre-populated with a synthetic symbol table.  Uses the
    real read_tags / lookup_symbol logic by inheriting from TagDB."""

    def __init__(self, client, path, symbols):
        super().__init__(client, handle=1, path=path)
        self._symbols = symbols
        # Pre-populate the cache so lookup_symbol doesn't need symbol_at.
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
        tagdb.Symbol(name="E_DINT_ARR", data_type=proto.TYPE_DINT,
                     elem_byte_size=4, dim0=10),
        tagdb.Symbol(name="F_STRING", data_type=0x0CE, struct_type=0xFCE,
                     elem_byte_size=88),
    ]


# ---- tests ----

def test_read_tags_decodes_scalars():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())

    # Populate response: A_BOOL=1, B_DINT=0xDEADBEEF, C_REAL=3.5, D_UDINT=42
    def provider(slot, descs):
        # data area starts at 0x2A0 + (count-1) * stride
        count = len(descs)
        data_off = 0x2A0 + (count - 1) * proto.TAGDATA_REQ_STRIDE
        for name, dt, ebs, action, n, req_start in descs:
            if name == "A_BOOL":
                slot[data_off] = 1
            elif name == "B_DINT":
                struct.pack_into("<i", slot, data_off, -559038737)  # 0xDEADBEEF
            elif name == "C_REAL":
                struct.pack_into("<f", slot, data_off, 3.5)
            elif name == "D_UDINT":
                struct.pack_into("<I", slot, data_off, 42)
            # result stays 0 (initialized)
            data_off += ebs * n
    c.response_provider = provider

    out = db.read_tags(["A_BOOL", "B_DINT", "C_REAL", "D_UDINT"])
    assert out["A_BOOL"] is True
    assert out["B_DINT"] == -559038737
    assert abs(out["C_REAL"] - 3.5) < 1e-6
    assert out["D_UDINT"] == 42


def test_read_tags_rejects_array_pre_ipc():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    try:
        db.read_tags(["B_DINT", "E_DINT_ARR"])
    except errors.BpParamRange:
        # Should reject without making any IPC call.
        assert c.last_access_descriptors == []
        return
    raise AssertionError("expected BpParamRange for array tag")


def test_read_tags_rejects_udt_pre_ipc():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())
    try:
        db.read_tags(["F_STRING"])
    except errors.BpParamRange:
        assert c.last_access_descriptors == []
        return
    raise AssertionError("expected BpParamRange for STRING UDT")


def test_read_tags_partial_failure_raises_with_results():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", _make_symbols())

    def provider(slot, descs):
        count = len(descs)
        data_off = 0x2A0 + (count - 1) * proto.TAGDATA_REQ_STRIDE
        for name, dt, ebs, action, n, req_start in descs:
            if name == "A_BOOL":
                slot[data_off] = 1
                # result stays 0
            elif name == "B_DINT":
                # Mark this tag as failed at the CIP layer.
                struct.pack_into("<I", slot, req_start + proto.REQ_RESULT_OFF, 0x04)
            data_off += ebs * n
    c.response_provider = provider

    try:
        db.read_tags(["A_BOOL", "B_DINT"])
    except errors.BpGeneric as e:
        assert "B_DINT" in str(e)
        assert e.results["A_BOOL"] is True
        assert e.statuses["B_DINT"] == 0x04
        return
    raise AssertionError("expected BpGeneric for partial failure")


def test_read_tags_chunks_at_16():
    """Verifies that 18 names result in 2 AccessTagData calls
    (16 + 2)."""
    c = FakeClient()
    syms = [
        tagdb.Symbol(name=f"T{i}", data_type=proto.TYPE_DINT, elem_byte_size=4)
        for i in range(20)
    ]
    db = FakeDB(c, "P:1,S:2", syms)

    call_counts = [0]

    def provider(slot, descs):
        call_counts[0] += 1
        count = len(descs)
        data_off = 0x2A0 + (count - 1) * proto.TAGDATA_REQ_STRIDE
        for name, dt, ebs, action, n, req_start in descs:
            struct.pack_into("<i", slot, data_off, int(name[1:]))
            data_off += ebs * n
    c.response_provider = provider

    names = [s.name for s in syms[:18]]
    out = db.read_tags(names)
    assert call_counts[0] == 2, f"expected 2 chunks, got {call_counts[0]}"
    for i in range(18):
        assert out[f"T{i}"] == i


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  OK  {name}")
    print("all read_tags tests passed")
