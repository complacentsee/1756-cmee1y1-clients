#!/usr/bin/env python3
"""routedident — v0.8.0 multi-hop Identity via Unconnected_Send.

Output format mirrors c/examples/routedident.c + go/cmd/routedident
(modulo hex spacing).

SPDX-License-Identifier: MIT
"""
import argparse
import sys

import bpclient


def hexspaced(b: bytes) -> str:
    return " ".join(f"{x:02x}" for x in b) + " "


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--router-slot", type=int, default=2)
    ap.add_argument("--port", type=int, default=1)
    ap.add_argument("--link", type=int, default=2)
    args = ap.parse_args()

    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[routedident] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()

        embedded = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
        route = bpclient.port_segment(args.port, args.link)
        wrapped = bpclient.build_unconnected_send(embedded, route, 5000)

        print(f"[routedident] router_slot={args.router_slot}  port={args.port}  "
              f"link={args.link}  wrapped_len={len(wrapped)}")
        print(f"[routedident] wrapped bytes: {hexspaced(wrapped)}")

        msg = bpclient.Message(
            slot=args.router_slot,
            cip_request=wrapped,
            resp_data=b"",
            resp_capacity=256,
            timeout_ms=5000,
        )
        c.message_send(msg)
        out = msg.resp_data
        print(f"[routedident] resp_len={len(out)}  bytes: {hexspaced(out)}")

        if len(out) < 4 or out[0] != 0x81:
            print(f"[routedident] reply not Identity GAA "
                  f"(svc=0x{out[0] if out else 0:02x} "
                  f"status=0x{out[2] if len(out) >= 3 else 0xFF:02x})",
                  file=sys.stderr)
            return 1
        if out[2] != 0x00:
            ext = 0
            if len(out) >= 6 and out[3]:
                ext = out[4] | (out[5] << 8)
            print(f"[routedident] routed CIP failure: status=0x{out[2]:02x} "
                  f"ext=0x{ext:04x} ({bpclient.cip_status_message(out[2], ext)})",
                  file=sys.stderr)
            return 1

        body_off = 4 + out[3] * 2
        if len(out) < body_off + 14:
            print("[routedident] body too short", file=sys.stderr)
            return 1
        body = out[body_off:]
        vendor = body[0] | (body[1] << 8)
        dev    = body[2] | (body[3] << 8)
        prod   = body[4] | (body[5] << 8)
        major  = body[6]
        minor  = body[7]
        name_len = body[14] if len(body) > 14 else 0
        if 15 + name_len > len(body):
            name_len = len(body) - 15
        name = body[15:15 + name_len].decode("ascii", "replace")
        print(f"[routedident] Identity: Vendor=0x{vendor:04x} "
              f"DevType=0x{dev:04x} Product=0x{prod:04x} fw={major}.{minor} Name='{name}'")
        print("[routedident] PASS")
        return 0
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
