#!/usr/bin/env python3
"""structrwtest — live validator for the SDK's atomic read_struct /
write_struct (CIP 0x4C/0x4D over MessageSend).

Mirrors go/examples/structrwtest.  READ-ONLY unless --write, where it
writes the just-read bytes back (no value change) and confirms the
round-trip is byte-identical.  Run on the module with the SDK IPC flags
(--ipc=host --pid=host -v /dev/shm:/dev/shm); bpServer is single-client,
so stop any gateway first.

Exit 0 on PASS, 1 on FAIL/FATAL.

SPDX-License-Identifier: MIT
"""
import argparse
import sys

import bpclient


def main() -> int:
    ap = argparse.ArgumentParser(prog="structrwtest", description=__doc__)
    ap.add_argument("--slot", type=int, default=2,
                    help="controller backplane slot (N in P:1,S:N)")
    ap.add_argument("--tag", default="Tran_From_iSeries_Register",
                    help="structured tag to access")
    ap.add_argument("--write", action="store_true",
                    help="write the just-read bytes back (no value change) "
                         "and verify the round-trip is byte-identical")
    args = ap.parse_args()

    try:
        client = bpclient.Client()
        client.open()
    except Exception as e:  # noqa: BLE001
        sys.stderr.write(f"[structrwtest] FATAL Open: {e}\n")
        return 1
    try:
        client.open_session()
    except Exception as e:  # noqa: BLE001
        sys.stderr.write(f"[structrwtest] FATAL OpenSession: {e}\n")
        client.close()
        return 1

    rc = 0
    try:
        data, handle = client.read_struct(args.slot, args.tag, 600)
        print(f"[structrwtest] read_struct OK: tag={args.tag!r} "
              f"handle=0x{handle:04x} bytes={len(data)}")

        if not args.write:
            print("[structrwtest] VERDICT: SDK read_struct works (read-only).")
        else:
            client.write_struct(args.slot, args.tag, handle, data)
            back, _ = client.read_struct(args.slot, args.tag, 600)
            if data != back:
                sys.stderr.write(
                    f"[structrwtest] FAIL: round-trip mismatch "
                    f"({len(data)} vs {len(back)} bytes / contents differ)\n")
                rc = 1
            else:
                print("[structrwtest] VERDICT: SDK write_struct accepted "
                      "(CIP 0) + round-trip byte-identical — ATOMIC UDT I/O works.")
    except Exception as e:  # noqa: BLE001
        sys.stderr.write(f"[structrwtest] FAIL: {e}\n")
        rc = 1
    finally:
        client.close()
    return rc


if __name__ == "__main__":
    sys.exit(main())
