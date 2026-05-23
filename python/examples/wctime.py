#!/usr/bin/env python3
"""wctime — v0.10.3 Phase E/H live validator.

Mirrors c/examples/wctime.c + go/cmd/wctime.  Pass --raw to also
dump the six raw qwords (sec/nsec/aux0..3) per response.

SPDX-License-Identifier: MIT
"""
import sys

import bpclient

G_RAW = False


def epoch_for(slot: int, is_utc: bool) -> int:
    """Per-PLC epoch identifier table (empirical, 2026-05-22)."""
    if slot == 2:
        return bpclient.WCTIME_EPOCH_UNIX if is_utc else bpclient.WCTIME_EPOCH_1972
    return bpclient.WCTIME_EPOCH_1998 if is_utc else bpclient.WCTIME_EPOCH_2000


def print_wc(label: str, path: str, wc, rc: int,
              epoch: int, try_tz: bool, try_local: bool) -> bool:
    if wc is None:
        print(f"[wctime] {label} {path}: rc={rc}")
        return False
    dt = wc.to_datetime(epoch)
    ts = dt.strftime("%Y-%m-%dT%H:%M:%S")
    tz = wc.tz_name() if try_tz else ""
    print(f"[wctime] {label} {path}: {ts} UTC  tz=\"{tz}\"")
    if try_local:
        loc = wc.decode_local()
        print(f"[wctime] {label} {path}: aux2-decoded local "
              f"d={loc.day} h={loc.hour} m={loc.minute} s={loc.second}")
    if G_RAW:
        print(f"[wctime] {label} {path}: raw sec=0x{wc.sec:016x} "
              f"nsec=0x{wc.nsec:016x}")
        print(f"[wctime] {label} {path}: raw aux0=0x{wc.aux0:016x} "
              f"aux1=0x{wc.aux1:016x}")
        print(f"[wctime] {label} {path}: raw aux2=0x{wc.aux2:016x} "
              f"aux3=0x{wc.aux3:016x}")
    return True


def main() -> int:
    global G_RAW
    if "--raw" in sys.argv[1:]:
        G_RAW = True
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
            for label, fn, is_utc in (("LOCAL", c.get_wctime, False),
                                       ("UTC  ", c.get_wctime_utc, True)):
                try:
                    wc = fn(path, 1)
                    rc = 0
                except Exception as e:
                    wc = None
                    rc = bpclient.err_code(e)
                if print_wc(label, path, wc, rc, epoch_for(s, is_utc),
                             is_utc, not is_utc):
                    any_ok = True
        print(f"[wctime] {'PASS' if any_ok else 'FAIL'}")
        return 0 if any_ok else 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
