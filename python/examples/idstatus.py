#!/usr/bin/env python3
"""idstatus — v0.10.0 Phase C validator for client.get_device_id_status.

SPDX-License-Identifier: MIT
"""
import sys

import bpclient


def main() -> int:
    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[idstatus] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()
        hits, mismatches = 0, 0
        for slot in range(4):
            path = f"P:1,S:{slot}"
            try:
                status_lite = c.get_device_id_status(path, 1)
                rc_lite = 0
            except Exception as e:
                status_lite = None
                rc_lite = bpclient.err_code(e)
            try:
                id_full = c.get_device_id(path, 1)
                rc_full = 0
            except Exception as e:
                id_full = None
                rc_full = bpclient.err_code(e)
            if rc_lite == 0 and rc_full == 0:
                match = status_lite == id_full.status
                match_str = "YES" if match else "NO"
                print(f"[idstatus] slot={slot}  lite=0x{status_lite:04x}  "
                      f"full=0x{id_full.status:04x}  match={match_str}")
                hits += 1
                if not match:
                    mismatches += 1
            else:
                print(f"[idstatus] slot={slot}  lite_rc={rc_lite} full_rc={rc_full}  "
                      "(empty or error)")
        passed = hits >= 1 and mismatches == 0
        print(f"[idstatus] SUMMARY hits={hits} mismatches={mismatches}")
        print(f"[idstatus] {'PASS' if passed else 'FAIL'}")
        return 0 if passed else 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
