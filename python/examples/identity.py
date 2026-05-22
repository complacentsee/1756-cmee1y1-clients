#!/usr/bin/env python3
"""identity — print the Identity object of a CIP device.
Mirrors c/examples/identity.c CLI + output format byte-for-byte.

SPDX-License-Identifier: MIT
"""
import argparse
import sys

import bpclient


def vendor_name(vid: int) -> str:
    return {
        0x0001: "Allen-Bradley (Rockwell)",
        0x0030: "Online Development Inc.",
    }.get(vid, "(unknown)")


def print_id(id_obj: bpclient.Identity) -> None:
    pn = id_obj.product_name
    skip = 0
    while skip < 31 and skip < len(pn) and pn[skip] == 0:
        skip += 1
    take = 0
    while skip + take < 32 and skip + take < len(pn) and pn[skip + take] != 0:
        take += 1
    name = pn[skip:skip + take].decode("ascii", "replace")
    print(f"  Vendor ID       : 0x{id_obj.vendor_id:04x}  ({vendor_name(id_obj.vendor_id)})")
    print(f"  Device Type     : 0x{id_obj.device_type:04x}")
    print(f"  Product Code    : 0x{id_obj.product_code:04x}  ({id_obj.product_code})")
    print(f"  Revision        : {id_obj.major_rev}.{id_obj.minor_rev}")
    print(f"  Status          : 0x{id_obj.status:04x}")
    print(f"  Serial Number   : 0x{id_obj.serial_number:08x}  ({id_obj.serial_number})")
    print(f"  Product Name    : '{name}'")


def main() -> int:
    ap = argparse.ArgumentParser(prog="identity")
    ap.add_argument("--path", default="")
    args = ap.parse_args()

    try:
        client = bpclient.Client()
        client.open()
    except Exception:
        sys.stderr.write("client open failed\n")
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    try:
        lo, hi = client.get_active_nodes()
        parts = []
        for i in range(32):
            if lo & (1 << i):
                parts.append(f" {i}")
        for i in range(32):
            if hi & (1 << i):
                parts.append(f" {i + 32}")
        print(f"[active nodes] mask_lo=0x{lo:08x}  mask_hi=0x{hi:08x}  =>{''.join(parts)}\n")
    except Exception:
        pass

    if not args.path:
        print("=== LOCAL Identity (Client.get_id_local) ===")
        try:
            id_obj = client.get_id_local()
            rc = 0
        except Exception as e:
            id_obj = None
            rc = bpclient.err_code(e)
        print(f"  rc={rc} ({bpclient.strerror(rc)})")
        if id_obj is not None:
            print_id(id_obj)
    else:
        print(f"=== REMOTE Identity via OCXcip_GetDeviceIdObject('{args.path}', inst=1) ===")
        try:
            id_obj = client.get_device_id(args.path, 1)
            rc = 0
        except Exception as e:
            id_obj = None
            rc = bpclient.err_code(e)
        print(f"  rc={rc} ({bpclient.strerror(rc)})")
        if id_obj is not None:
            print_id(id_obj)

    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
