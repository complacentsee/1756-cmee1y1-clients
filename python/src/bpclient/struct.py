"""Structured (whole-UDT) tag access via CIP Read Tag (0x4C) / Write Tag
(0x4D) over the raw MessageSend (UCMM) path.

Mirrors go/ocxbp/struct.go.  Unlike the typed AccessTagData family in
tagdb.py, this reads/writes a UDT instance as ONE CIP transaction —
atomic on the controller.  A structured Write Tag must carry the 2-byte
structure-template handle the controller assigned; the controller hands
that handle back in the reply to a structured Read Tag, so read_struct
returns it and write_struct (or the caller, via a cached handle)
supplies it back.

Wire shapes (CIP request body — service, path_size_words, path, body):

    Read Tag (0x4C):  [0x4C][words][0x91 len name pad][elem_count u16]
      reply data:     [type u16 = 0x02A0][handle u16][payload...]

    Write Tag (0x4D): [0x4D][words][0x91 len name pad]
                      [0xA0 0x02][handle u16][elem_count u16][payload...]

Limits: MessageSend is UCMM, so request+reply must fit MSG_MAX_REQ
(500) / the response buffer.  The SUF registers (104 B) fit easily;
larger UDTs would need a connected/fragmented path (future work).

These are module-level functions taking the Client as the first
argument (so they can be unit-tested with a fake client, like the
tagdb helpers).  client.py binds them onto Client as
``read_struct`` / ``write_struct`` methods, matching the Go
``(c *Client)`` receivers.

SPDX-License-Identifier: MIT
"""
import struct as _struct

from . import _proto as P
from .errors import BpCipError, BpGeneric, BpNullArg, BpParamRange
from .message import Message

# StructTypeAbbrev is the CIP "abbreviated structure" type code that
# prefixes structured-tag data on the wire (0x02A0 = 0xA0 with the
# structured bit).  The 2-byte template handle follows it.
StructTypeAbbrev = 0x02A0


def _sym_path(tag: str) -> tuple[bytes, int]:
    """Build an ANSI Extended Symbol Segment (0x91) request path for
    ``tag``.  Returns ``(path_bytes, size_in_words)`` — the value the
    CIP request header carries.  Mirrors struct.go::symbolicIOIPath:
    odd-length names get a trailing NUL pad so the path is an even
    number of bytes (whole 16-bit words).
    """
    name = tag.encode("ascii", "strict")
    b = bytearray()
    b.append(0x91)
    b.append(len(name))
    b += name
    if len(b) % 2 != 0:
        b.append(0x00)
    return bytes(b), len(b) // 2


def read_struct(client, slot: int, tag: str, resp_cap: int = 600) -> tuple[bytes, int]:
    """Read a whole structured (UDT) tag in one CIP Read Tag transaction.

    Returns ``(payload, handle)``: the raw struct payload bytes plus the
    controller-assigned 2-byte structure handle.  Pass the handle to
    ``write_struct`` to write the same UDT back.

    ``slot`` is the controller's backplane slot (the N in "P:1,S:N").
    ``resp_cap`` bounds the reply buffer; pass the struct's byte size + a
    little headroom (the reply adds a ~4-byte CIP header + the 4-byte
    type/handle prefix).

    Raises BpNullArg on a None client, BpParamRange on a bad tag or
    resp_cap, BpCipError on a non-zero CIP general status, BpGeneric on
    a short or malformed reply.
    """
    if client is None:
        raise BpNullArg("client is None")
    if not tag or len(tag) > 250:
        raise BpParamRange("tag empty or too long")
    if resp_cap < 16 or resp_cap > 0xFFFF:
        raise BpParamRange("resp_cap out of range 16..65535")

    ioi, words = _sym_path(tag)
    req = bytearray()
    req.append(0x4C)
    req.append(words)
    req += ioi
    req += _struct.pack("<H", 1)  # elem_count = 1

    m = Message(slot=slot, cip_request=bytes(req), resp_capacity=resp_cap)
    client.message_send(m)

    r = m.resp_data[: m.resp_len]
    if len(r) < 4:
        raise BpGeneric("short CIP reply")
    # CIP reply header: [service][reserved][general_status][ext_size words]
    svc, status, ext_words = r[0], r[2], r[3]
    if status != 0:
        ext = 0
        if ext_words >= 1 and len(r) >= 4 + ext_words * 2:
            ext = _struct.unpack_from("<H", r, 4)[0]
        raise BpCipError(service=svc, status=status, ext_status=ext, slot=slot)

    body = r[4 + ext_words * 2:]
    if len(body) < 4:
        raise BpGeneric("struct reply too short for type+handle prefix")
    # body = [type u16][handle u16][payload...]
    handle = _struct.unpack_from("<H", body, 2)[0]
    payload = bytes(body[4:])
    return payload, handle


def write_struct(client, slot: int, tag: str, handle: int, data: bytes) -> None:
    """Write a whole structured (UDT) tag in one CIP Write Tag
    transaction — atomic on the controller.

    ``handle`` is the 2-byte template handle from ``read_struct`` for
    this tag; ``data`` is the full struct payload (exactly the struct's
    byte size, controller-authoritative).

    Raises BpNullArg on a None client, BpParamRange on a bad tag, empty
    data, or an over-cap request (larger structs need a connected/
    fragmented path), BpCipError on a non-zero CIP general status,
    BpGeneric on a short or malformed reply.
    """
    if client is None:
        raise BpNullArg("client is None")
    if not tag or len(tag) > 250:
        raise BpParamRange("tag empty or too long")
    if not data:
        raise BpParamRange("data is empty")

    ioi, words = _sym_path(tag)
    req = bytearray()
    req.append(0x4D)
    req.append(words)
    req += ioi
    # abbreviated-structure type (0xA0 0x02) + handle
    req += _struct.pack("<H", StructTypeAbbrev)
    req += _struct.pack("<H", handle & 0xFFFF)
    req += _struct.pack("<H", 1)  # elem_count = 1
    req += data
    if len(req) > P.MSG_MAX_REQ:
        # UCMM request cap — larger structs need a connected/fragmented
        # path; surface a clear range error rather than a truncated write.
        raise BpParamRange(f"request {len(req)} bytes exceeds MSG_MAX_REQ {P.MSG_MAX_REQ}")

    m = Message(slot=slot, cip_request=bytes(req), resp_capacity=64)
    client.message_send(m)

    r = m.resp_data[: m.resp_len]
    if len(r) < 3:
        raise BpGeneric("short CIP reply")
    svc, status = r[0], r[2]
    ext_words = r[3] if len(r) >= 4 else 0
    if status != 0:
        ext = 0
        if ext_words >= 1 and len(r) >= 4 + ext_words * 2:
            ext = _struct.unpack_from("<H", r, 4)[0]
        raise BpCipError(service=svc, status=status, ext_status=ext, slot=slot)
