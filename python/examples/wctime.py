#!/usr/bin/env python3
"""wctime — v0.10.0 Phase E live validator.  Mirrors c/examples/wctime.c.

SPDX-License-Identifier: MIT
"""
import datetime
import sys

import bpclient


def print_wc(label: str, path: str, wc: "bpclient.WCTime | None", rc: int) -> bool:
    if wc is None:
        print(f"[wctime] {label} {path}: rc={rc}")
        return False
    try:
        dt = datetime.datetime.fromtimestamp(wc.sec, tz=datetime.timezone.utc)
        ts = dt.strftime("%Y-%m-%dT%H:%M:%S")
    except (OverflowError, OSError, ValueError):
        ts = "<out of range>"
    print(f"[wctime] {label} {path}: sec={wc.sec} nsec={wc.nsec} -> {ts} "
          f"aux=(0x{wc.aux0:x},0x{wc.aux1:x},0x{wc.aux2:x},0x{wc.aux3:x})")
    return True


def main() -> int:
    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[wctime] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()
        any_ok = False
        for s in (1, 2, 3):
            path = f"P:1,S:{s}"
            for label, fn in (("LOCAL", c.get_wctime), ("UTC  ", c.get_wctime_utc)):
                try:
                    wc = fn(path, 1)
                    rc = 0
                except Exception as e:
                    wc = None
                    rc = bpclient.err_code(e)
                if print_wc(label, path, wc, rc):
                    any_ok = True
        print(f"[wctime] {'PASS' if any_ok else 'FAIL'}")
        return 0 if any_ok else 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
