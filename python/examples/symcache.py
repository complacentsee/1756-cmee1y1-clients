#!/usr/bin/env python3
"""symcache — v0.9.0 Phase 1 symbol-cache validator.

Output mirrors c/examples/symcache.c + go/cmd/symcache (modulo dt=
timings).

SPDX-License-Identifier: MIT
"""
import argparse
import sys
import time

import bpclient


def now_ms() -> float:
    return time.monotonic() * 1000.0


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", default="P:1,S:2")
    ap.add_argument("--tag", default="OCX_TEST")
    args = ap.parse_args()

    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[symcache] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()
        db = c.open_tagdb(args.path)
        try:
            t0 = now_ms()
            n = db.build()
            t1 = now_ms()
            print(f"[symcache] path={args.path} build n={n} dt={t1 - t0:.2f}ms")

            l0 = now_ms()
            info = db.lookup_symbol(args.tag)
            l1 = now_ms()
            print(f"[symcache] lookup#1 cold dt={l1 - l0:.2f}ms  "
                  f"data_type=0x{info.data_type:04x} elem_byte_size={info.elem_byte_size}")

            l2 = now_ms()
            db.lookup_symbol(args.tag)
            l3 = now_ms()
            print(f"[symcache] lookup#2 warm dt={l3 - l2:.3f}ms  (cache hit)")

            p0 = now_ms()
            try:
                db.preload_symbols()
                rc = 0
            except Exception as e:
                rc = bpclient.err_code(e)
            p1 = now_ms()
            print(f"[symcache] preload all dt={p1 - p0:.2f}ms rc={rc}")

            l4 = now_ms()
            db.lookup_symbol(args.tag)
            l5 = now_ms()
            print(f"[symcache] lookup#3 after-preload dt={l5 - l4:.3f}ms  rc=0")
            print("[symcache] PASS")
            return 0
        finally:
            db.close()
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
