"""AccessTagData request dataclass.

SPDX-License-Identifier: MIT
"""
from dataclasses import dataclass, field

from . import _proto as P


@dataclass
class TagRequest:
    """One entry in a batched AccessTagData call.

    For ActionRead the `data` field is the OUT buffer (set by the
    SDK on return).  For ActionWrite, `data` is the IN bytes to send.
    After the call, `result` holds the per-request CIP General
    Status (0 = ok).
    """
    tag_name: str = ""
    data_type: int = 0
    elem_byte_size: int = 0
    action: int = P.ACTION_READ
    elem_count: int = 1
    data: bytes = b""
    result: int = 0


# Action constants re-exported for caller convenience.
ACTION_READ = P.ACTION_READ
ACTION_WRITE = P.ACTION_WRITE
