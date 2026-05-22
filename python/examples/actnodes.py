#!/usr/bin/env python3
"""actnodes — dump the OCXcip_GetActiveNodeTable bitmap.
Mirrors c/examples/actnodes.c output format byte-for-byte.

SPDX-License-Identifier: MIT
"""
import sys

import bpclient


def main() -> int:
    try:
        client = bpclient.Client()
        client.open()
    except Exception:
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    try:
        lo, hi = client.get_active_nodes()
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"[actnodes] rc={rc} ({bpclient.strerror(rc)})")
        client.close()
        return 1

    print(f"[actnodes] active_lo = 0x{lo:08x}  active_hi = 0x{hi:08x}")
    sys.stdout.write("active slots:")
    count = 0
    for i in range(32):
        if lo & (1 << i):
            sys.stdout.write(f" {i}")
            count += 1
    for i in range(32):
        if hi & (1 << i):
            sys.stdout.write(f" {i + 32}")
            count += 1
    sys.stdout.write(f"  ({count} total)\n")
    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
