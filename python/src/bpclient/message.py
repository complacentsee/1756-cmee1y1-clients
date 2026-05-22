"""MessageSend dataclass.

Field names follow the Phase 4 wire correction in docs/protocol.md
(the OEM wrapper's "service", "encoded_path", "class_or_misc"
names are misnomers — slot is the BACKPLANE SLOT NUMBER, cip_request
is the FULL CIP REQUEST BODY).

SPDX-License-Identifier: MIT
"""
from dataclasses import dataclass


@dataclass
class Message:
    """One UCMM CIP request/response.  Populate the IN fields,
    pass to Client.message_send(); the OUT fields are filled in
    on return.

    Engine validation:
      slot       < 0x14  (else engine rc=1)
      req_size   1..500  (else engine rc=1; pre-checked by SDK)
      timeout_ms clamped to min 26 ms by engine
    """
    # IN
    slot: int = 0
    cip_request: bytes = b""        # full CIP body: [service, path_size, path..., body...]
    req_size: int = 0               # auto-derived from len(cip_request) if 0
    timeout_ms: int = 0             # per-attempt; 0 = engine default
    resp_capacity: int = 256        # caller buffer size; must be > 0

    # OUT
    resp_data: bytes = b""          # raw CIP reply
    resp_len: int = 0
    status: int = 0                 # wrapper status field
