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
        BP_ERR_CLIENT_OPEN: "shm_open/ftruncate/mmap failed (is bpServer running? --ipc=host set?)",
        BP_ERR_NO_FREE_SLOT: "all 16 slots in use (other clients holding slots)",
    }.get(rc, "unknown error")
