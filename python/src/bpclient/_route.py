"""Unconnected_Send (CIP svc 0x52) body assembly + port-segment
helpers (v0.8.0 Phase 3 — multi-hop routes).

Wire format documented in docs/protocol.md "Multi-hop routes —
Unconnected_Send (service 0x52)".  Keep this file in sync with
c/src/route.c and go/ocxbp/cip/route.go.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import struct

# Standard CIP Unconnected_Send encoding: tick=5 → 32 ms units
# (priority 0).
_UCS_TICK_VAL = 5
_UCS_TICK_MS = 32


def build_unconnected_send(embedded_msg: bytes,
                            route_path: bytes,
                            timeout_ms: int) -> bytes:
    """Assemble an Unconnected_Send (svc 0x52) request body.

    Parameters
    ----------
    embedded_msg
        The target's CIP request bytes — must start with
        ``[service, path_size_words, ...]``.
    route_path
        CIP port segments concatenated.  Length must be even
        (route_path_size is encoded in 16-bit words).
    timeout_ms
        Total request timeout; converted to CIP priority/tick + ticks
        using tick=5 (32 ms units), clamped to ticks 1..255.

    Returns
    -------
    bytes
        The assembled Unconnected_Send body; pass to ``message_send``
        / ``txrx_msg`` / ``pool_txrx`` targeting the slot device's
        Connection Manager.

    Raises
    ------
    ValueError
        On odd ``route_path`` length or oversized inputs.
    """
    if not embedded_msg:
        raise ValueError("embedded_msg is required")
    if route_path is None or len(route_path) == 0:
        raise ValueError("route_path is required")
    if len(route_path) & 1:
        raise ValueError("route_path length must be even (encoded as 16-bit words)")
    if len(embedded_msg) > 0xFFFF:
        raise ValueError(f"embedded_msg too large ({len(embedded_msg)} > 0xFFFF)")
    if len(route_path) > 0x1FE:
        raise ValueError(f"route_path too large ({len(route_path)} > 510)")

    ticks = (timeout_ms + _UCS_TICK_MS - 1) // _UCS_TICK_MS
    ticks = max(1, min(255, ticks))

    pad = len(embedded_msg) & 1
    total = 10 + len(embedded_msg) + pad + 2 + len(route_path)
    out = bytearray(total)
    off = 0
    out[off] = 0x52; off += 1                  # service: Unconnected_Send
    out[off] = 0x02; off += 1                  # path_size in words
    out[off] = 0x20; off += 1
    out[off] = 0x06; off += 1                  # class 0x06 = ConnMgr
    out[off] = 0x24; off += 1
    out[off] = 0x01; off += 1                  # instance 1
    out[off] = _UCS_TICK_VAL & 0x0F; off += 1  # priority 0, tick = 5
    out[off] = ticks; off += 1                 # timeout_ticks
    struct.pack_into("<H", out, off, len(embedded_msg)); off += 2
    out[off:off + len(embedded_msg)] = embedded_msg
    off += len(embedded_msg)
    if pad:
        out[off] = 0x00; off += 1
    out[off] = len(route_path) // 2; off += 1  # route_path size in words
    out[off] = 0x00; off += 1                  # reserved
    out[off:off + len(route_path)] = route_path
    off += len(route_path)
    assert off == total
    return bytes(out)


def port_segment(port: int, link: int) -> bytes:
    """Encode one CIP port segment ``{port, link}`` (2 bytes).

    Standard encodings:
      port=1  link=N    backplane slot N
      port=2  link=N    front-side EtherNet/IP, link address N
    """
    if not (0 <= port <= 0xFF) or not (0 <= link <= 0xFF):
        raise ValueError("port/link must be 0..255")
    return bytes([port & 0xFF, link & 0xFF])
