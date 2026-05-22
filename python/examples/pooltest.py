#!/usr/bin/env python3
"""pooltest — v0.8.0 connection-pool validator.

Output format mirrors c/examples/pooltest.c + go/cmd/pooltest (modulo
dt= timings and thread interleave order); the cross-language gate is
the diffs of the summary line.

SPDX-License-Identifier: MIT
"""
import argparse
import sys
import threading
import time

import bpclient


IDENTITY_REQ = bytes([0x01, 0x02, 0x20, 0x01, 0x24, 0x01])


def now_ms() -> float:
    return time.monotonic() * 1000.0


def validate_identity(resp: bytes) -> bool:
    return len(resp) >= 4 and resp[0] == 0x81 and resp[2] == 0x00


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--slot", type=int, default=2)
    ap.add_argument("--size", type=int, default=4)
    ap.add_argument("--workers", type=int, default=8)
    ap.add_argument("--requests", type=int, default=25)
    ap.add_argument("--keepalive-ms", type=int, default=10000)
    ap.add_argument("--keepalive-test", action="store_true")
    ap.add_argument("--batch", action="store_true",
                    help="use pool_batch (Phase 4) instead of manual thread fanout")
    args = ap.parse_args()

    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[pooltest] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()

        print(f"[pooltest] slot={args.slot} size={args.size} "
              f"workers={args.workers} requests/worker={args.requests} "
              f"keepalive_ms={args.keepalive_ms}")

        spec = bpclient.PoolSpec(
            slot=args.slot, size=args.size,
            keepalive_ms=args.keepalive_ms, conn_params=0,
        )
        t_open0 = now_ms()
        c.pool_open(spec)
        t_open1 = now_ms()
        print(f"[pooltest] pool_open  dt={t_open1 - t_open0:.2f}ms")

        success = [0]
        failed = [0]
        total = args.workers * args.requests

        if args.batch:
            reqs = [IDENTITY_REQ] * total
            t0 = now_ms()
            results = c.pool_batch(args.slot, reqs, 256)
            t1 = now_ms()
            for i, (resp, err) in enumerate(results):
                if err is None and resp is not None and validate_identity(resp):
                    success[0] += 1
                else:
                    failed[0] += 1
                    print(f"[pooltest] batch item[{i}] err={err}",
                          file=sys.stderr)
        else:
            slock = threading.Lock()

            def worker(wid: int) -> None:
                local_succ = 0
                local_fail = 0
                for i in range(args.requests):
                    try:
                        resp = c.pool_txrx(args.slot, IDENTITY_REQ, 256)
                        if validate_identity(resp):
                            local_succ += 1
                        else:
                            local_fail += 1
                            print(f"[pooltest] worker={wid} req[{i}] invalid",
                                  file=sys.stderr)
                    except Exception as e:
                        local_fail += 1
                        print(f"[pooltest] worker={wid} req[{i}] err={e}",
                              file=sys.stderr)
                with slock:
                    success[0] += local_succ
                    failed[0]  += local_fail

            threads = [threading.Thread(target=worker, args=(w,))
                       for w in range(args.workers)]
            t0 = now_ms()
            for t in threads: t.start()
            for t in threads: t.join()
            t1 = now_ms()

        rate = total / ((t1 - t0) / 1000.0) if (t1 - t0) > 0 else 0
        mode = "batch " if args.batch else "fanout"
        print(f"[pooltest] {mode} {args.workers} workers × "
              f"{args.requests} req = {total} total  "
              f"dt={t1 - t0:.2f}ms  ({rate:.0f} req/s)  "
              f"success={success[0]} failed={failed[0]}")

        if args.keepalive_test:
            print("[pooltest] keepalive test: sleeping 30 s — watch stderr for ping lines")
            time.sleep(30)

        t_close0 = now_ms()
        try:
            c.pool_close(args.slot)
            rc = 0
        except Exception as e:
            rc = bpclient.err_code(e)
        t_close1 = now_ms()
        print(f"[pooltest] pool_close dt={t_close1 - t_close0:.2f}ms rc={rc}")

        passed = success[0] == total and rc == 0
        print(f"[pooltest] {'PASS' if passed else 'FAIL'}")
        return 0 if passed else 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
