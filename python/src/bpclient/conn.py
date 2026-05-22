"""Class-3 connection dataclass — used by Client.txrx_open / txrx_msg /
txrx_close.

STATUS: NOT FUNCTIONAL on cm1756 (the OCXCN_OpenClass3Connection
library is missing from the chip image).  See docs/protocol.md
"Connected messaging — open issues" + bpclient.message_send for
the Large Forward Open workaround.

SPDX-License-Identifier: MIT
"""
from dataclasses import dataclass


@dataclass
class ConnSpec:
    """Caller-managed class-3 connection descriptor.  Pass the same
    instance unchanged to txrx_open / txrx_msg / txrx_close — the
    OEM API duplicates fields across calls and we follow suit."""
    app_handle: int = 1           # caller-assigned ID; reuse across the lifecycle
    options: int = 0              # vendor flags, normally 0
    encoded_path: bytes = b""     # route to target (e.g. b"\x01\x02" = slot 2)
    path_size: int = 0
    conn_params: int = 0          # vendor conn params; normally 0 (engine defaults)
