#!/usr/bin/env python3
"""msgprobe — raw OCXcip_MessageSend invocation + response hexdump.
Mirrors c/examples/msgprobe.c and go/cmd/msgprobe/main.go CLI +
output format byte-for-byte.

SPDX-License-Identifier: MIT
"""
import argparse
import sys

import bpclient


def parse_hex(s: str) -> bytes:
    out = bytearray()
    i = 0
    while i < len(s):
        while i < len(s) and not _is_hex(s[i]):
            i += 1
        if i + 1 >= len(s) or not _is_hex(s[i + 1]):
            break
        out.append(int(s[i:i + 2], 16))
        i += 2
    return bytes(out)


def _is_hex(c: str) -> bool:
    return c in "0123456789abcdefABCDEF"


def hexdump(b: bytes) -> None:
    for i in range(0, len(b), 16):
        line = b[i:i + 16]
        hex_part = " ".join(f"{c:02x}" for c in line)
        hex_part += "   " * (16 - len(line))
        ascii_part = "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in line)
        sys.stdout.write(f"    +{i:03x} {hex_part}  {ascii_part}\n")


def main() -> int:
    ap = argparse.ArgumentParser(prog="msgprobe")
    ap.add_argument("--slot", type=int, default=-1)
    ap.add_argument("--req", default="")
    ap.add_argument("--timeout-ms", type=int, default=0, dest="timeout_ms")
    args = ap.parse_args()

    if args.slot < 0 or not args.req:
        sys.stderr.write('Use --slot 1 --req "01 02 20 01 24 01" [--timeout-ms 1000]\n')
        return 2

    req_bytes = parse_hex(args.req)
    if not req_bytes:
        sys.stderr.write("empty req\n")
        return 2

    req_hex = " ".join(f"{b:02x}" for b in req_bytes)
    print(f"[msgprobe] slot={args.slot} timeout_ms={args.timeout_ms} "
          f"req={req_hex} ({len(req_bytes)} bytes)\n")

    try:
        client = bpclient.Client()
        client.open()
    except Exception:
        sys.stderr.write("Open failed\n")
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    msg = bpclient.Message(
        slot=args.slot,
        cip_request=req_bytes,
        req_size=len(req_bytes),
        timeout_ms=args.timeout_ms,
        resp_capacity=4096,
    )
    try:
        client.message_send(msg)
        rc = 0
    except Exception as e:
        rc = bpclient.err_code(e)

    print(f"rc            = {rc} (0x{rc & 0xFFFFFFFF:x})")
    print(f"response_len  = {msg.resp_len}")
    print(f"status field  = 0x{msg.status:08x}")
    if msg.resp_len > 0:
        print("response bytes:")
        n = min(msg.resp_len, 256)
        hexdump(msg.resp_data[:n])

    client.close()
    return 0 if rc == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
