"""Error class hierarchy + strerror helper.

Mirrors c/src/errors.c::bp_strerror and c/include/bpclient.h
BP_ERR_* numbering.

SPDX-License-Identifier: MIT
"""

# BP_ERR_* codes — match the vendor OCX_ERR_* numbering.
BP_OK = 0
BP_ERR_GENERIC = -1
BP_ERR_SEND_REQUEST = -200
BP_ERR_RECV_ANSWER = -201
BP_ERR_NULL_ARG = -300
BP_ERR_PENDING = -301
BP_ERR_NOT_OPEN = -303
BP_ERR_PARAM_RANGE = -305
BP_ERR_SLOT_TOO_LARGE = -311
BP_ERR_CIP_STATUS = -400
BP_ERR_CLIENT_OPEN = -101802
BP_ERR_NO_FREE_SLOT = -103001


class BpError(Exception):
    """Base for all bpclient errors."""

    code: int = BP_ERR_GENERIC


class BpGeneric(BpError):
    code = BP_ERR_GENERIC


class BpSendRequest(BpError):
    code = BP_ERR_SEND_REQUEST


class BpRecvAnswer(BpError):
    code = BP_ERR_RECV_ANSWER


class BpNullArg(BpError):
    code = BP_ERR_NULL_ARG


class BpPending(BpError):
    code = BP_ERR_PENDING


class BpNotOpen(BpError):
    code = BP_ERR_NOT_OPEN


class BpParamRange(BpError):
    code = BP_ERR_PARAM_RANGE


class BpSlotTooLarge(BpError):
    code = BP_ERR_SLOT_TOO_LARGE


class BpClientOpen(BpError):
    code = BP_ERR_CLIENT_OPEN


class BpNoFreeSlot(BpError):
    code = BP_ERR_NO_FREE_SLOT


class BpEngine(BpError):
    """Carries a non-zero slot errorcode written by the bpServer.

    Positive values are engine codes (1 = bad param, 3 = empty
    slot target, 14 = retry budget exhausted, 0x14/0x15 from
    TestTagDbVer, etc.).  Negative values are OCX_ERR_* codes
    surfaced verbatim.
    """

    def __init__(self, code: int):
        super().__init__(f"engine errorcode {code} (0x{code & 0xFFFFFFFF:x}): {strerror(code)}")
        self.code = code


class BpCipError(BpError):
    """A CIP-layer rejection.

    Raised when the transport round-trip succeeds but the PLC returns
    a non-zero general_status (e.g. the SDK's TxRx* family sees the
    PLC reject an LFO or FC).  The structured fields:

      service     – reply service byte (0xDB for LFO reply, 0xCE for
                    FC reply, request_service|0x80 for other services)
      status      – CIP General Status byte (table in
                    docs/error-codes.md)
      ext_status  – CIP extended_status first 16-bit word (0 if absent)
      slot        – backplane slot that received the request

    ``err_code(exc)`` returns ``BP_ERR_CIP_STATUS`` (-400).
    """

    code = BP_ERR_CIP_STATUS

    def __init__(self, service: int, status: int, ext_status: int, slot: int):
        self.service = service
        self.status = status
        self.ext_status = ext_status
        self.slot = slot
        super().__init__(
            f"CIP rejection svc=0x{service:02x} status=0x{status:02x} "
            f"ext=0x{ext_status:04x} slot={slot} "
            f"({cip_status_message(status, ext_status)})"
        )


def cip_status_message(status: int, ext_status: int) -> str:
    """Human-readable string for a (status, ext_status) pair.  Mirrors
    c/src/errors.c::bp_cip_status_string and ocxbp.CIPStatusString —
    keep in sync."""
    if status == 0x00:
        return "success"
    if status == 0x01:
        return {
            0x0100: "connection in use (stale conn from prior session — let PLC idle-time-out ~40s or restart bpServer)",
            0x0103: "transport class unsupported (controller firmware rejected class 0xA3)",
            0x0107: "connection ID not found in Forward_Close (PLC already cleaned up; safe to ignore on close)",
            0x0113: "no more connections available on target",
            0x0114: "vendor id or product code mismatch in Forward_Close",
            0x0115: "device type mismatch in Forward_Close",
            0x0116: "revision mismatch in Forward_Close",
            0x0117: "non-listen-only connection not opened",
            0x0119: "Forward_Close conn ID mismatch",
            0x011A: "target application out of connections",
            0x0203: "connection timeout",
            0x0204: "Unconnected_Send timeout",
            0x0205: "parameter error in Unconnected_Send",
            0x0206: "message too large for Unconnected_Send",
            0x0311: "port not available",
            0x0312: "link address not available",
            0x0315: "invalid segment type or value in path",
            0x0316: "invalid attribute (connection path malformed)",
            0x0317: "key segment not preceded by port segment",
            0x0318: "link address to self invalid",
        }.get(ext_status, "connection failure")
    return {
        0x02: "resource unavailable (most often: conn_params requesting oversized buffer — try conn_params=0)",
        0x03: "invalid parameter value",
        0x04: "path segment error (bad tag name or EPATH)",
        0x05: "path destination unknown (slot empty, or object doesn't accept this service)",
        0x06: "partial transfer",
        0x07: "connection lost",
        0x08: "service not supported by target object",
        0x09: "invalid attribute value",
        0x0A: "attribute list error",
        0x0B: "already in requested state",
        0x0C: "object state conflict",
        0x0D: "object already exists",
        0x0E: "attribute not settable (write to read-only)",
        0x0F: "privilege violation",
        0x10: "device state conflict",
        0x11: "reply data too large",
        0x12: "fragmentation of primitive value",
        0x13: "not enough data",
        0x14: "attribute not supported",
        0x15: "too much data",
        0x16: "object does not exist",
        0x17: "service fragmentation sequence not in progress",
        0x18: "no stored attribute data",
        0x19: "store operation failure",
        0x1A: "routing failure: request packet too large",
        0x1B: "routing failure: response packet too large",
        0x1C: "missing attribute list entry data",
        0x1D: "invalid attribute value list",
        0x1E: "embedded service error",
        0x1F: "vendor-specific error",
        0x20: "invalid parameter",
        0x21: "write-once value or medium already written",
        0x22: "invalid reply received",
        0x25: "key failure in path",
        0x26: "path size invalid",
        0x27: "unexpected attribute in list",
        0x28: "invalid member id",
        0x29: "member not settable",
    }.get(status, "unknown CIP status")


_BP_ERR_MAP: dict[int, type[BpError]] = {
    BP_ERR_GENERIC: BpGeneric,
    BP_ERR_SEND_REQUEST: BpSendRequest,
    BP_ERR_RECV_ANSWER: BpRecvAnswer,
    BP_ERR_NULL_ARG: BpNullArg,
    BP_ERR_PENDING: BpPending,
    BP_ERR_NOT_OPEN: BpNotOpen,
    BP_ERR_PARAM_RANGE: BpParamRange,
    BP_ERR_SLOT_TOO_LARGE: BpSlotTooLarge,
    BP_ERR_CLIENT_OPEN: BpClientOpen,
    BP_ERR_NO_FREE_SLOT: BpNoFreeSlot,
}


def raise_for_rc(rc: int) -> None:
    """Raise the matching error class for a non-zero rc; no-op on BP_OK."""
    if rc == BP_OK:
        return
    cls = _BP_ERR_MAP.get(rc)
    if cls is not None:
        raise cls(strerror(rc))
    # Engine codes are anything not in the BP_ERR_* table.
    raise BpEngine(rc)


def err_code(exc: BaseException | None) -> int:
    """Return the rc integer that the C SDK would have returned for
    this Python exception, or BP_OK if None.

    Used by diagnostic tools that need to print rc values that diff
    byte-for-byte against the C tooling output.
    """
    if exc is None:
        return BP_OK
    code = getattr(exc, "code", None)
    if isinstance(code, int):
        return code
    return BP_ERR_GENERIC


def strerror(rc: int) -> str:
    """Human-readable string for a BP_ERR_* / engine code.  Mirrors
    c/src/errors.c::bp_strerror."""
    return {
        BP_OK: "ok",
        BP_ERR_GENERIC: "generic failure",
        BP_ERR_SEND_REQUEST: "sem_post on bpReq failed",
        BP_ERR_RECV_ANSWER: "sem_wait on bpResp failed (server crashed?)",
        BP_ERR_NULL_ARG: "null argument",
        BP_ERR_PENDING: "still pending (server hasn't replied — server crash?)",
        BP_ERR_NOT_OPEN: "not open / Open() not called or IPC lost",
        BP_ERR_PARAM_RANGE: "parameter range error (check path string format: P:1,S:2 not 1,2)",
        BP_ERR_SLOT_TOO_LARGE: "response too large for slot (reduce batch size)",
        BP_ERR_CIP_STATUS: "CIP-layer rejection (BpCipError carries service/status/ext_status/slot)",
        BP_ERR_CLIENT_OPEN: "shm_open/ftruncate/mmap failed (is bpServer running? --ipc=host set?)",
        BP_ERR_NO_FREE_SLOT: "all 16 slots in use (other clients holding slots)",
    }.get(rc, "unknown error")
