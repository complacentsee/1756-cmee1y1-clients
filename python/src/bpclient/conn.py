"""Class-3 connection dataclass — used by Client.txrx_open / txrx_msg /
txrx_close.

v0.7.0+: functional.  The SDK builds Large Forward Open + Forward_Close
internally and routes through Client.message_send (chip mailbox 0x200,
UCMM transport).  See docs/protocol.md "Connected messaging — wire
format".

SPDX-License-Identifier: MIT
"""
from dataclasses import dataclass


@dataclass
class ConnSpec:
    """Caller-managed class-3 connection descriptor.  Pass the same
    instance unchanged to txrx_open / txrx_msg / txrx_close — the
    cache is keyed on app_handle."""
    app_handle: int = 1           # caller-assigned ID; reuse across the lifecycle
    options: int = 0              # accepted for API stability; unused in v0.7.0
    encoded_path: bytes = b""     # route to target (e.g. b"\x01\x02" = slot 2)
    path_size: int = 0
    conn_params: int = 0          # O→T/T→O size in bytes; 0 = SDK default 4000, max 4002
