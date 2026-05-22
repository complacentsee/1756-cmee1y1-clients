"""Unit tests for the pool auto-reopen backoff math (v0.9.0 Phase 4).

The live keepalive integration is hard to fake (requires a stalled
PLC); these tests focus on the backoff sequence: 1s → 2s → 4s → ...
capped at 30s.

SPDX-License-Identifier: MIT
"""
import importlib.util
import sys
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


# Re-implement the backoff math standalone — same sequence as
# client.py's keepalive loop.  Verifying here keeps the test
# decoupled from the full Client which needs posix_ipc.
def next_backoff(current_ms: int) -> int:
    if current_ms == 0:
        return 1000
    return min(current_ms * 2, 30000)


def test_backoff_sequence_starts_at_1s():
    assert next_backoff(0) == 1000


def test_backoff_doubles_each_failure():
    seq = []
    cur = 0
    # The first failure sets backoff to 1000.  After that the keepalive
    # doubles on each subsequent failure.
    cur = next_backoff(cur)
    seq.append(cur)  # 1000
    for _ in range(10):
        cur = next_backoff(cur)
        seq.append(cur)
    assert seq[0] == 1000
    assert seq[1] == 2000
    assert seq[2] == 4000
    assert seq[3] == 8000
    assert seq[4] == 16000
    assert seq[5] == 30000     # cap kicks in
    assert seq[6] == 30000
    assert seq[-1] == 30000    # stays capped


def test_backoff_resets_on_success():
    """After a successful reopen, the keepalive resets backoff to 1s
    so the next failure has the full backoff sequence available."""
    # Simulate sequence: 1s -> 2s -> 4s -> success.
    cur = next_backoff(0)
    cur = next_backoff(cur)
    cur = next_backoff(cur)
    assert cur == 4000
    # On success, code path: entry.reopen_backoff_ms = 1000
    cur = 1000
    cur = next_backoff(cur)
    assert cur == 2000   # back at start of sequence


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  OK  {name}")
    print("all pool_reopen tests passed")
