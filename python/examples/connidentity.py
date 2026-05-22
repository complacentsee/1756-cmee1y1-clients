#!/usr/bin/env python3
"""connidentity — class-3 connected Identity query.  Mirrors
c/examples/connidentity.c + go/cmd/connidentity CLI + output format.

v0.7.0+: Drives Client.txrx_open / txrx_msg / txrx_close end-to-end
against a real PLC.  The SDK internally builds Large Forward Open +
Forward_Close around the caller's Identity Get_Attributes_All
(svc 0x01) request, all via message_send.

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


def print_id(resp: bytes) -> None:
    if len(resp) < 4:
        print("  (response too short)")
        return
    reply_svc = resp[0]
    status = resp[2]
    ext_size = resp[3]
    print(f"  CIP reply: service=0x{reply_svc:02x}  general_status=0x{status:02x}  "
          f"ext_status_sz={ext_size}")
    if status != 0:
        print("  CIP error.")
        return
    body_start = 4 + ext_size * 2
    if body_start > len(resp):
        return
    body = resp[body_start:]
    if len(body) < 14:
        print("  (body too short for Identity)")
        return
    vendor = body[0] | (body[1] << 8)
    devtype = body[2] | (body[3] << 8)
    prodcode = body[4] | (body[5] << 8)
    major = body[6]
    minor = body[7]
    serial = body[10] | (body[11] << 8) | (body[12] << 16) | (body[13] << 24)
    name_len = body[14] if len(body) > 14 else 0
    if 15 + name_len > len(body):
        name_len = len(body) - 15
    print(f"  Vendor=0x{vendor:04x}  DevType=0x{devtype:04x}  "
          f"ProductCode=0x{prodcode:04x}  fw={major}.{minor}  serial=0x{serial:08x}")
    if name_len > 0:
        print(f"  Name='{body[15:15 + name_len].decode('ascii', 'replace')}'")


def main() -> int:
    ap = argparse.ArgumentParser(prog="connidentity")
    ap.add_argument("--slot", type=int, default=2)
    ap.add_argument("--conn-params", type=lambda x: int(x, 0), default=0,
                    dest="conn_params",
                    help="O→T/T→O size in bytes; 0 = SDK default 4000")
    ap.add_argument("--path", default="")
    ap.add_argument("--app-handle", type=int, default=1, dest="app_handle")
    args = ap.parse_args()

    try:
        client = bpclient.Client()
        client.open()
    except Exception:
        sys.stderr.write("open failed\n")
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    if args.path:
        epath = parse_hex(args.path)
    else:
        epath = bytes([0x01, args.slot])

    spec = bpclient.ConnSpec(
        app_handle=args.app_handle,
        options=0,
        encoded_path=epath,
        path_size=len(epath),
        conn_params=args.conn_params,
    )

    epath_hex = " ".join(f"{b:02x}" for b in epath)
    print(f"[connidentity] app_handle={args.app_handle}  slot={args.slot}  "
          f"conn_params=0x{args.conn_params:04x}  path={epath_hex} ({len(epath)} bytes)")

    try:
        conn_id, conn_serial = client.txrx_open(spec)
        orc = 0
    except Exception as e:
        conn_id = conn_serial = 0
        orc = bpclient.err_code(e)
    print(f"[connidentity] txrx_open rc={orc} (0x{orc & 0xFFFFFFFF:x} {bpclient.strerror(orc)})  "
          f"conn_id=0x{conn_id:04x}  serial=0x{conn_serial:04x}")

    req = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])
    try:
        resp = client.txrx_msg(spec, req, 256)
        mrc = 0
    except Exception as e:
        resp = b""
        mrc = bpclient.err_code(e)
    print(f"[connidentity] txrx_msg rc={mrc} ({bpclient.strerror(mrc)})  resp_len={len(resp)}")
    if mrc == 0 and resp:
        resp_hex = " ".join(f"{b:02x}" for b in resp)
        print(f"response: {resp_hex} ")
        print_id(resp)

    try:
        client.txrx_close(spec)
        crc = 0
    except Exception as e:
        crc = bpclient.err_code(e)
    print(f"[connidentity] txrx_close rc={crc} ({bpclient.strerror(crc)})")

    client.close()
    ok = (mrc == 0 and len(resp) >= 4 and resp[0] == 0x81 and resp[2] == 0x00)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
