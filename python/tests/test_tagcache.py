"""Unit tests for the v0.9.0 per-client symbol cache.

Bypasses the IPC layer by injecting a fake `symbol_at` on a fake
TagDB; verifies that lookup_symbol's lazy fill, post-build invalidation,
and preload semantics work as documented.

SPDX-License-Identifier: MIT
"""
import importlib.util
import sys
import threading
import types
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"


def _load_submodule(parent_pkg_name, sub_name, rel_path):
    """Load `parent_pkg_name.sub_name` from rel_path under SRC."""
    full_name = f"{parent_pkg_name}.{sub_name}"
    spec = importlib.util.spec_from_file_location(
        full_name, SRC / parent_pkg_name / rel_path)
    m = importlib.util.module_from_spec(spec)
    sys.modules[full_name] = m
    spec.loader.exec_module(m)
    return m


# Install a fake `bpclient` parent package so `from .` imports inside
# the real source files resolve.  We then load each sub-module by file
# path with the right full dotted name.
pkg = types.ModuleType("bpclient")
pkg.__path__ = [str(SRC / "bpclient")]
sys.modules["bpclient"] = pkg

proto = _load_submodule("bpclient", "_proto", "_proto.py")
errors = _load_submodule("bpclient", "errors", "errors.py")

# Stub `bpclient.access` (TagRequest) so tagdb.py can import from it
# without dragging in the IPC layer.
access_stub = types.ModuleType("bpclient.access")
class TagRequest:
    pass
access_stub.TagRequest = TagRequest
sys.modules["bpclient.access"] = access_stub

tagdb = _load_submodule("bpclient", "tagdb", "tagdb.py")


# Minimal stand-in for Client that just hosts the cache helpers.
class FakeClient:
    """Replays the cache-helper portion of Client without the IPC dep."""

    def __init__(self):
        self._tag_cache_mu = threading.Lock()
        self._tag_caches = {}

    def _find_or_alloc_tag_cache(self, path):
        with self._tag_cache_mu:
            tc = self._tag_caches.get(path)
            if tc is None:
                tc = _TagCache()
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
            if tc.total_count == 0 and not tc.symbols:
                raise errors.BpParamRange("lookup before build")
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

    def _preload_cached_symbols(self, db):
        tc = self._find_or_alloc_tag_cache(db.path)
        tc.mu.acquire()
        try:
            if tc.total_count == 0 and not tc.symbols:
                raise errors.BpParamRange("preload before build")
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


class _TagCache:
    def __init__(self):
        self.mu = threading.Lock()
        self.symbols = []
        self.known_count = 0
        self.total_count = 0


class FakeDB:
    """Minimal TagDB substitute — exposes path + a synthetic symbol_at
    that pulls from a list provided by the test."""

    def __init__(self, client, path, symbols):
        self._client = client
        self.path = path
        self._symbols = symbols
        self.symbol_at_calls = 0

    def symbol_at(self, index):
        self.symbol_at_calls += 1
        return self._symbols[index]


def _make_symbols():
    return [
        tagdb.Symbol(name="A", data_type=0xC4, elem_byte_size=4),
        tagdb.Symbol(name="B", data_type=0xCA, elem_byte_size=4),
        tagdb.Symbol(name="OCX_TEST", data_type=0xC4, elem_byte_size=4),
        tagdb.Symbol(name="Z", data_type=0xC3, elem_byte_size=2),
    ]


# ---- tests ----

def test_lookup_before_build_raises():
    c = FakeClient()
    db = FakeDB(c, "P:1,S:2", [])
    try:
        c._lookup_cached_symbol(db, "anything")
    except errors.BpParamRange:
        return
    raise AssertionError("expected BpParamRange for lookup before build")


def test_lookup_lazy_fill_then_cache_hit():
    c = FakeClient()
    syms = _make_symbols()
    db = FakeDB(c, "P:1,S:2", syms)
    c._reset_tag_cache_after_build("P:1,S:2", len(syms))

    # First lookup of "OCX_TEST" walks indices 0..2 (A, B, OCX_TEST).
    info = c._lookup_cached_symbol(db, "OCX_TEST")
    assert info.name == "OCX_TEST"
    assert db.symbol_at_calls == 3, f"cold lookup should fetch 3 syms, got {db.symbol_at_calls}"

    # Second lookup is a pure cache hit — no new IPC.
    before = db.symbol_at_calls
    info2 = c._lookup_cached_symbol(db, "OCX_TEST")
    assert info2.name == "OCX_TEST"
    assert db.symbol_at_calls == before, "warm lookup must not do IPC"


def test_lookup_unknown_walks_to_exhaustion():
    c = FakeClient()
    syms = _make_symbols()
    db = FakeDB(c, "P:1,S:2", syms)
    c._reset_tag_cache_after_build("P:1,S:2", len(syms))
    try:
        c._lookup_cached_symbol(db, "DOES_NOT_EXIST")
    except errors.BpParamRange:
        # Walked the whole table.
        assert db.symbol_at_calls == len(syms)
        return
    raise AssertionError("expected BpParamRange for unknown symbol")


def test_preload_populates_everything():
    c = FakeClient()
    syms = _make_symbols()
    db = FakeDB(c, "P:1,S:2", syms)
    c._reset_tag_cache_after_build("P:1,S:2", len(syms))
    n = c._preload_cached_symbols(db)
    assert n == len(syms)
    # All subsequent lookups are cache hits.
    before = db.symbol_at_calls
    for s in syms:
        info = c._lookup_cached_symbol(db, s.name)
        assert info.name == s.name
    assert db.symbol_at_calls == before


def test_build_invalidates_cache():
    c = FakeClient()
    syms = _make_symbols()
    db = FakeDB(c, "P:1,S:2", syms)
    c._reset_tag_cache_after_build("P:1,S:2", len(syms))
    c._preload_cached_symbols(db)

    # New build → cache reset.  Lookup walks again from scratch.
    db.symbol_at_calls = 0
    c._reset_tag_cache_after_build("P:1,S:2", len(syms))
    c._lookup_cached_symbol(db, syms[0].name)
    assert db.symbol_at_calls == 1


def test_caches_keyed_by_path():
    """Different PLC paths have independent caches."""
    c = FakeClient()
    syms = _make_symbols()
    db1 = FakeDB(c, "P:1,S:1", syms)
    db2 = FakeDB(c, "P:1,S:2", syms)
    c._reset_tag_cache_after_build("P:1,S:1", len(syms))
    c._reset_tag_cache_after_build("P:1,S:2", len(syms))

    c._lookup_cached_symbol(db1, "A")
    c._lookup_cached_symbol(db2, "A")
    # Each path fetched once.
    assert db1.symbol_at_calls == 1
    assert db2.symbol_at_calls == 1


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  OK  {name}")
    print("all tagcache tests passed")
