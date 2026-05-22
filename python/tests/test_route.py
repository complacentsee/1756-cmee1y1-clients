"""Unit tests for Unconnected_Send body assembly + port-segment helpers.

Pure unit tests — no PLC, no /dev/shm, no posix_ipc runtime — runs
on any host.  Validates against the canonical example in
docs/protocol.md "Multi-hop routes — Unconnected_Send (service 0x52)".

SPDX-License-Identifier: MIT
"""
import importlib.util
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
SRC = HERE.parent / "src"

_spec = importlib.util.spec_from_file_location(
    "bpclient_route", SRC / "bpclient" / "_route.py")
route = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(route)


def test_port_segment_encoding():
    # Backplane to slot 2 = 0x01 0x02.
    assert route.port_segment(1, 2) == b"\x01\x02"
    # Port 2 (EIP) link 5 = 0x02 0x05.
    assert route.port_segment(2, 5) == b"\x02\x05"


def test_build_unconnected_send_canonical():
    """Reproduces the canonical example in docs/protocol.md:
    Identity GAA via Unconnected_Send through L85 ConnMgr, route =
    {0x02, 0x01} (port 2 of L85, link 1).
    """
    embedded = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
    route_path = route.port_segment(2, 1)
    out = route.build_unconnected_send(embedded, route_path, 5024)

    # Expected layout (with ticks ≈ 5024/32 = 157 = 0x9D):
    expected = bytes([
        0x52, 0x02,                # svc 0x52, path_size 2 words
        0x20, 0x06, 0x24, 0x01,    # class 6 (ConnMgr), instance 1
        0x05, 0x9D,                # priority/tick, ticks (5024 → 157)
        0x06, 0x00,                # embedded_msg_sz = 6 (LE)
        0x01, 0x02, 0x20, 0x01, 0x24, 0x01,  # embedded msg (Identity GAA)
        0x01, 0x00,                # route_path_words = 1, reserved
        0x02, 0x01,                # route_path: port 2 link 1
    ])
    assert out == expected, f"got {out.hex()}, want {expected.hex()}"


def test_build_unconnected_send_pads_odd_embedded():
    # 5-byte embedded (odd) gets a 1-byte pad before route_path.
    embedded = bytes([0xAA, 0xBB, 0xCC, 0xDD, 0xEE])
    route_path = bytes([0x01, 0x02])
    out = route.build_unconnected_send(embedded, route_path, 5000)
    # offset of pad: 10 + 5 = 15; out[15] must be 0x00.
    assert out[15] == 0x00
    # After pad, route_path_words is at offset 16.
    assert out[16] == 1
    assert out[17] == 0x00
    assert out[18:20] == route_path


def test_build_unconnected_send_clamps_ticks():
    embedded = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
    route_path = bytes([0x01, 0x02])
    # Very small timeout → ticks=1 (minimum).
    short = route.build_unconnected_send(embedded, route_path, 1)
    assert short[7] == 1
    # Very large timeout → ticks=255 (maximum, ~8160 ms).
    long = route.build_unconnected_send(embedded, route_path, 60000)
    assert long[7] == 255


def test_build_unconnected_send_rejects_odd_route_path():
    embedded = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
    odd = bytes([0x01, 0x02, 0x02])  # 3 bytes — invalid
    try:
        route.build_unconnected_send(embedded, odd, 5000)
    except ValueError:
        return
    raise AssertionError("expected ValueError for odd route_path length")


def test_build_unconnected_send_rejects_empty():
    try:
        route.build_unconnected_send(b"", bytes([0x01, 0x02]), 5000)
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError for empty embedded_msg")
    try:
        route.build_unconnected_send(bytes([0x01]), b"", 5000)
    except ValueError:
        pass
    else:
        raise AssertionError("expected ValueError for empty route_path")


def test_multi_hop_route():
    # Two-hop route: through slot 3 then DH+ node 5.
    embedded = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
    path = route.port_segment(1, 3) + route.port_segment(2, 5)
    out = route.build_unconnected_send(embedded, path, 5000)
    # route_path_words = 2 at offset (10 + 6 = 16; embedded is even, no pad)
    assert out[16] == 2
    assert out[17] == 0x00
    assert out[18:22] == path


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            fn()
            print(f"  OK  {name}")
    print("all route tests passed")
