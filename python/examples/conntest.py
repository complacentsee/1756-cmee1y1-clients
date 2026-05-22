#!/usr/bin/env python3
"""conntest — v0.7.0 connected-messaging round-trip validator.

Output format is byte-identical (modulo dt= timing values) to
c/examples/conntest.c + go/cmd/conntest.  The cross-language gate
is the three diffs.

SPDX-License-Identifier: MIT
"""
import argparse
import bisect
import sys
import time

import bpclient


IDENTITY_REQ = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])


def now_ms() -> float:
    return time.monotonic() * 1000.0


def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return 0.0
    s = sorted(xs)
    idx = p * (len(s) - 1)
    lo = int(idx)
    hi = lo + 1 if lo + 1 < len(s) else len(s) - 1
    frac = idx - lo
    return s[lo] * (1.0 - frac) + s[hi] * frac


def validate_identity(resp: bytes) -> bool:
    return len(resp) >= 4 and resp[0] == 0x81 and resp[2] == 0x00


def print_identity(resp: bytes) -> None:
    if len(resp) < 14:
        print("[conntest] identity: (body too short)")
        return
    off = 4 + resp[3] * 2
    body = resp[off:]
    if len(body) < 14:
        print("[conntest] identity: (body too short)")
        return
    vendor = body[0] | (body[1] << 8)
    dev = body[2] | (body[3] << 8)
    prod = body[4] | (body[5] << 8)
    major = body[6]
    minor = body[7]
    name_len = body[14] if len(body) > 14 else 0
    if 15 + name_len > len(body):
        name_len = len(body) - 15
    name = body[15:15 + name_len].decode("ascii", "replace")
    print(f"[conntest] identity: Vendor=0x{vendor:04x} DevType=0x{dev:04x} "
          f"Product=0x{prod:04x} fw={major}.{minor} Name='{name}'")


def run_bench(client: bpclient.Client, slot: int, n: int) -> int:
    print(f"[conntest] benchmark: {n} Identity round-trips per transport, slot={slot}")

    ucmm_dt: list[float] = []
    class3_dt: list[float] = []

    # UCMM loop.
    for i in range(n):
        msg = bpclient.Message(
            slot=slot, cip_request=IDENTITY_REQ,
            resp_data=b"", resp_capacity=256, timeout_ms=5000,
        )
        t0 = now_ms()
        client.message_send(msg)
        ucmm_dt.append(now_ms() - t0)
        if not validate_identity(msg.resp_data):
            sys.stderr.write(f"[conntest] UCMM bench: req[{i}] reply invalid\n")
            return 1

    # Class-3 loop.
    spec = bpclient.ConnSpec(
        app_handle=2, encoded_path=bytes([0x01, slot]), path_size=2,
    )
    client.txrx_open(spec)
    try:
        for i in range(n):
            t0 = now_ms()
            resp = client.txrx_msg(spec, IDENTITY_REQ, 256)
            class3_dt.append(now_ms() - t0)
            if not validate_identity(resp):
                sys.stderr.write(f"[conntest] Class3 bench: req[{i}] reply invalid\n")
                return 1
    finally:
        client.txrx_close(spec)

    print(f"[conntest]   UCMM     median dt={percentile(ucmm_dt, 0.50):.2f}ms  "
          f"p95 dt={percentile(ucmm_dt, 0.95):.2f}ms")
    print(f"[conntest]   Class3   median dt={percentile(class3_dt, 0.50):.2f}ms  "
          f"p95 dt={percentile(class3_dt, 0.95):.2f}ms")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(prog="conntest")
    ap.add_argument("--slot", type=int, default=2,
                    help="backplane slot number (default 2 = L85)")
    ap.add_argument("--n", type=int, default=10,
                    help="number of Identity round-trips on the connection")
    ap.add_argument("--bench", action="store_true",
                    help="run UCMM vs Class3 latency micro-benchmark (100 each)")
    args = ap.parse_args()

    try:
        client = bpclient.Client()
        client.open()
    except Exception:
        sys.stderr.write("[conntest] open failed\n")
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    spec = bpclient.ConnSpec(
        app_handle=1,
        encoded_path=bytes([0x01, args.slot]),
        path_size=2,
    )

    print(f"[conntest] slot={args.slot} N={args.n} app_handle={spec.app_handle}")

    try:
        conn_id, conn_serial = client.txrx_open(spec)
    except Exception as e:
        sys.stderr.write(f"[conntest] txrx_open: {e}\n")
        client.close()
        return 1
    print(f"[conntest] txrx_open  conn_id=0x{conn_id:04x}  serial=0x{conn_serial:04x}")

    dts: list[float] = []
    last_resp = b""
    success = 0
    for i in range(args.n):
        t0 = now_ms()
        try:
            resp = client.txrx_msg(spec, IDENTITY_REQ, 256)
        except Exception:
            resp = b""
        dts.append(now_ms() - t0)
        status = resp[2] if len(resp) >= 3 else 0xFF
        vendor = 0xFFFF
        if validate_identity(resp):
            off = 4 + resp[3] * 2
            if off + 2 <= len(resp):
                vendor = resp[off] | (resp[off + 1] << 8)
        print(f"[conntest] req[{i}] dt={dts[-1]:.2f}ms "
              f"status=0x{status:02x} vendor=0x{vendor:04x}")
        if validate_identity(resp):
            success += 1
            last_resp = resp

    if last_resp:
        print_identity(last_resp)

    close_ok = True
    try:
        client.txrx_close(spec)
    except Exception:
        close_ok = False
    print(f"[conntest] txrx_close {'ok' if close_ok else 'FAIL'}")

    print(f"[conntest] SUMMARY {success}/{args.n} success  "
          f"median dt={percentile(dts, 0.50):.2f}ms")
    pass_ = success == args.n and close_ok
    print(f"[conntest] {'PASS' if pass_ else 'FAIL'}")

    bench_rc = 0
    if args.bench:
        bench_rc = run_bench(client, args.slot, 100)

    client.close()
    return 0 if (pass_ and bench_rc == 0) else 1


if __name__ == "__main__":
    sys.exit(main())
