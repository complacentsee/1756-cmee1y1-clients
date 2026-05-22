"""Large Forward Open / Forward_Close CIP body encoders + decoders.

v0.7.0 implements class-3 connected messaging by building these CIP
service bodies in pure Python and sending them through
Client.message_send (chip mailbox 0x200, UCMM transport).  The
OCXcip_TxRx* OEM entry points are not used — they dispatch to
OCXCN_OpenClass3Connection in a library missing from the cm1756
image.

Wire format documented in docs/protocol.md "Connected messaging —
wire format".  Sibling reference:
historianupdate/driver/apex2/daemon/apex2_cip_connection.c.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import struct

from . import _proto as P


def extract_slot(encoded_path: bytes, path_size: int) -> int | None:
    """Pull the backplane slot from a canonical {0x01, slot} EncodedPath.

    Returns the slot, or None if the path isn't the v0.7.0-supported
    backplane-direct shape.  Multi-hop routes are out of scope —
    callers needing off-chassis targeting embed Unconnected_Send
    (svc 0x52) inside the txrx_msg request body.
    """
    if path_size != 2 or len(encoded_path) < 2 or encoded_path[0] != 0x01:
        return None
    return encoded_path[1]


def build_forward_open(conn_serial: int, orig_serial: int, ot_size_bytes: int) -> bytes:
    """Encode a Large Forward Open (CIP svc 0x5B) request body.

    Mirrors c/src/conn.c::build_lfo + go cip.BuildForwardOpen +
    historianupdate apex2_cip_connection.c::build_forward_open
    (lines 682-820).  Returns the 50-byte body.
    """
    out = bytearray(50)
    off = 0
    out[off] = 0x5B; off += 1
    out[off] = 0x02; off += 1                  # path size words
    out[off] = 0x20; off += 1                  # class 6 (CM)
    out[off] = 0x06; off += 1
    out[off] = 0x24; off += 1                  # instance 1
    out[off] = 0x01; off += 1
    out[off] = 0x05; off += 1                  # priority/tick
    out[off] = 0xF7; off += 1                  # timeout ticks
    struct.pack_into("<I", out, off, P._LFO_OT_HINT); off += 4
    struct.pack_into("<I", out, off, P._LFO_TO_HINT); off += 4
    struct.pack_into("<H", out, off, conn_serial); off += 2
    struct.pack_into("<H", out, off, P.LFO_VENDOR_ID); off += 2
    struct.pack_into("<I", out, off, orig_serial); off += 4
    struct.pack_into("<I", out, off, 0x00000003); off += 4   # timeout multiplier
    struct.pack_into("<I", out, off, P.LFO_RPI_US); off += 4
    struct.pack_into("<I", out, off, P._LFO_PARAMS_HI | (ot_size_bytes & 0xFFFF)); off += 4
    struct.pack_into("<I", out, off, P.LFO_RPI_US); off += 4
    struct.pack_into("<I", out, off, P._LFO_PARAMS_HI | (ot_size_bytes & 0xFFFF)); off += 4
    out[off] = 0xA3; off += 1                  # transport trigger: Class 3, server
    out[off] = 0x02; off += 1                  # conn path size words
    out[off] = 0x20; off += 1                  # class 2 (Msg Router)
    out[off] = 0x02; off += 1
    out[off] = 0x24; off += 1                  # instance 1
    out[off] = 0x01; off += 1
    assert off == 50
    return bytes(out)


def parse_forward_open(resp: bytes) -> tuple[int, int, int, bool]:
    """Parse the LFO reply.

    Returns (ot_conn_id, to_conn_id, general_status, ok).  ok=True
    means service byte is 0xDB or 0xD4 AND general_status == 0.
    """
    if len(resp) < 12:
        return 0, 0, 0xFF, False
    if resp[0] not in (0xDB, 0xD4):
        return 0, 0, resp[2] if len(resp) >= 3 else 0xFF, False
    status = resp[2]
    if status != 0x00:
        return 0, 0, status, False
    ot_conn_id = struct.unpack_from("<I", resp, 4)[0]
    to_conn_id = struct.unpack_from("<I", resp, 8)[0]
    return ot_conn_id, to_conn_id, status, True


def build_forward_close(conn_serial: int, vendor_id: int, orig_serial: int) -> bytes:
    """Encode a Forward_Close (CIP svc 0x4E) request body.

    Mirrors c/src/conn.c::build_fc + go cip.BuildForwardClose +
    historianupdate apex2_cip_connection.c::build_forward_close
    (lines 1244-1283).  Returns the 22-byte body.
    """
    out = bytearray(22)
    off = 0
    out[off] = 0x4E; off += 1
    out[off] = 0x02; off += 1
    out[off] = 0x20; off += 1
    out[off] = 0x06; off += 1
    out[off] = 0x24; off += 1
    out[off] = 0x01; off += 1
    out[off] = 0x0A; off += 1                  # priority/tick
    out[off] = 0x0E; off += 1                  # timeout ticks
    struct.pack_into("<H", out, off, conn_serial); off += 2
    struct.pack_into("<H", out, off, vendor_id); off += 2
    struct.pack_into("<I", out, off, orig_serial); off += 4
    out[off] = 0x02; off += 1                  # conn path size words
    out[off] = 0x00; off += 1                  # reserved
    out[off] = 0x20; off += 1
    out[off] = 0x02; off += 1
    out[off] = 0x24; off += 1
    out[off] = 0x01; off += 1
    assert off == 22
    return bytes(out)


def parse_forward_close(resp: bytes) -> tuple[int, bool]:
    """Parse the FC reply.  Returns (general_status, ok).

    ok=True means service byte is 0xCE and general_status == 0.
    """
    if len(resp) < 4:
        return 0xFF, False
    if resp[0] != 0xCE:
        return resp[2], False
    return resp[2], resp[2] == 0x00
