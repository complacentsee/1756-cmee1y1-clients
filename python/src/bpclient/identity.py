"""CIP Identity-object dataclass.

SPDX-License-Identifier: MIT
"""
from dataclasses import dataclass


@dataclass
class Identity:
    """CIP Identity Object (class 0x01).  48 bytes on the wire."""
    vendor_id: int = 0
    device_type: int = 0
    product_code: int = 0
    major_rev: int = 0
    minor_rev: int = 0
    status: int = 0
    serial_number: int = 0
    product_name: bytes = b""  # SHORT_STRING padded with NULs; 32 bytes
