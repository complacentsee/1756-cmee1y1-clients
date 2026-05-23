#!/usr/bin/env python3
"""dummy — v0.10.3 OCXcip_Dummy liveness-probe validator.

Mirrors c/examples/dummy.c + go/cmd/dummy.

SPDX-License-Identifier: MIT
"""
import sys
import time

import bpclient


def main() -> int:
    n = 100
    if len(sys.argv) >= 2:
        try:
            n = max(1, int(sys.argv[1]))
        except ValueError:
            pass

    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[dummy] open failed: {e}", file=sys.stderr)
        return 2

    fail = 0
    t0 = time.monotonic()
    try:
        for i in range(n):
            try:
                c.dummy()
            except Exception as e:
                print(f"[dummy] call {i} err={e}", file=sys.stderr)
                fail += 1
    finally:
        c.close()
    dt_us = (time.monotonic() - t0) * 1e6
    print(f"[dummy] {n} calls, {dt_us:.0f} us total, "
          f"{dt_us / n:.1f} us/call, {fail} failures")
    print(f"[dummy] {'PASS' if fail == 0 else 'FAIL'}")
    return 0 if fail == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
