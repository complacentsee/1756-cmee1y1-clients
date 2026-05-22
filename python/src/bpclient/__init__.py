"""bpclient — outbound CIP-over-bpServer SDK for the 1756-CMEE1Y1.

Mirrors the C SDK (libbpclient) shape-for-shape; uses idiomatic
Python (dataclasses, exceptions, return values) instead of C's
out-param + rc.

See docs/protocol.md for the wire-protocol spec and
docs/container-plumbing.md for the required Docker flags.

SPDX-License-Identifier: MIT
"""

from .errors import (
    BpCipError,
    BpClientOpen,
    BpEngine,
    BpError,
    BpGeneric,
    BpNoFreeSlot,
    BpNotOpen,
    BpNullArg,
    BpParamRange,
    BpPending,
    BpRecvAnswer,
    BpSendRequest,
    BpSlotTooLarge,
    cip_status_message,
    err_code,
    strerror,
)
from .client import Client
from .tagdb import StructInfo, StructMember, Symbol, TagDB
from .access import TagRequest
from .identity import Identity
from .message import Message
from .conn import ConnSpec

__all__ = [
    "Client",
    "TagDB",
    "Symbol",
    "StructInfo",
    "StructMember",
    "TagRequest",
    "Identity",
    "Message",
    "ConnSpec",
    "BpError",
    "BpGeneric",
    "BpSendRequest",
    "BpRecvAnswer",
    "BpNullArg",
    "BpPending",
    "BpNotOpen",
    "BpParamRange",
    "BpSlotTooLarge",
    "BpClientOpen",
    "BpNoFreeSlot",
    "BpEngine",
    "BpCipError",
    "cip_status_message",
    "strerror",
    "err_code",
]
