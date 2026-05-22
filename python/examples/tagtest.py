#!/usr/bin/env python3
"""tagtest — canonical Python smoke test.  Mirrors c/examples/tagtest.c
and go/cmd/tagtest/main.go flow + output format byte-for-byte so a
runprobe.py diff against the C and Go versions round-trips.

Sequence:
  1. bpclient.Client.open
  2. Client.open_session
  3. Client.open_tagdb("P:1,S:2")
  4. TagDB.build → expect symbol_count > 0
  5. TagDB.symbol_at(0..9) → print names + type codes
  6. TagDB.read_dint("OCX_TEST") → V0
  7. TagDB.write_dint("OCX_TEST", 0xDEADBEEF)
  8. TagDB.read_dint("OCX_TEST") → expect 0xDEADBEEF
  9. TagDB.write_dint("OCX_TEST", V0) (restore)
  10. TagDB.read_dint("OCX_TEST") → expect V0

Exit 0 on PASS, 1 on FAIL, 2 on FATAL.

SPDX-License-Identifier: MIT
"""
import argparse
import sys
import time

import bpclient


def main() -> int:
    ap = argparse.ArgumentParser(prog="tagtest")
    ap.add_argument("--path", default="P:1,S:2")
    ap.add_argument("--tag", default="OCX_TEST")
    ap.add_argument("--dump", type=int, default=10)
    ap.add_argument("--no-write", action="store_true")
    args = ap.parse_args()

    t_start = time.perf_counter()

    print("[tagtest] opening wrapper IPC")
    try:
        client = bpclient.Client()
        client.open()
    except Exception as e:
        sys.stderr.write(f"[tagtest] FATAL Open: {e}\n")
        return 2
    t_a = time.perf_counter()
    print(f"[tagtest]   ipc ready  dt={(t_a - t_start) * 1000:.2f}ms")

    try:
        session = client.open_session()
    except Exception as e:
        sys.stderr.write(f"[tagtest] FATAL OpenSession: {e}\n")
        client.close()
        return 2
    print(f"[tagtest] OCXcip_Open ok  session=0x{session:08x}")

    t_a = time.perf_counter()
    try:
        db = client.open_tagdb(args.path)
    except Exception as e:
        sys.stderr.write(f"[tagtest] FATAL OpenTagDB({args.path}): {e}\n")
        client.close()
        return 2
    t_b = time.perf_counter()
    print(f"[tagtest] OpenTagDB(\"{args.path}\") ok  dt={(t_b - t_a) * 1000:.2f}ms")

    t_a = time.perf_counter()
    try:
        n_symbols = db.build()
    except Exception as e:
        sys.stderr.write(f"[tagtest] FATAL Build: {e}\n")
        db.close()
        client.close()
        return 2
    t_b = time.perf_counter()
    print(f"[tagtest] Build ok  symbols={n_symbols}  dt={(t_b - t_a) * 1000:.2f}ms")

    print(f"[tagtest] first {args.dump} symbols:")
    limit = min(args.dump, n_symbols)
    for i in range(limit):
        try:
            info = db.symbol_at(i)
        except Exception as e:
            print(f"  [{i:4d}] error: {e}")
            continue
        print(f"  [{i:4d}] {info.name:<40s} type=0x{info.data_type & 0x1FFF:04x} struct=0x{info.struct_type:04x}")

    t_a = time.perf_counter()
    try:
        v0 = db.read_dint(args.tag)
    except Exception as e:
        sys.stderr.write(f"[tagtest] FATAL ReadDINT({args.tag}): {e}\n")
        db.close()
        client.close()
        return 2
    t_b = time.perf_counter()
    print(f"[tagtest] V0 = {v0} (0x{v0 & 0xFFFFFFFF:08x})  dt={(t_b - t_a) * 1000:.2f}ms")

    passed_write = True
    passed_restore = True

    if not args.no_write:
        sentinel = -559038737  # int32 of 0xDEADBEEF
        t_a = time.perf_counter()
        try:
            db.write_dint(args.tag, sentinel)
        except Exception as e:
            sys.stderr.write(f"[tagtest] FATAL Write 0xDEADBEEF: {e}\n")
            db.close()
            client.close()
            return 2
        t_b = time.perf_counter()
        print(f"[tagtest] wrote 0x{sentinel & 0xFFFFFFFF:08x}  dt={(t_b - t_a) * 1000:.2f}ms")

        try:
            v1 = db.read_dint(args.tag)
        except Exception as e:
            sys.stderr.write(f"[tagtest] FATAL Read post-write: {e}\n")
            db.close()
            client.close()
            return 2
        passed_write = v1 == sentinel
        marker = "<-- WRITE OK" if passed_write else "<-- WRITE DID NOT TAKE"
        print(f"[tagtest] V1 = {v1} (0x{v1 & 0xFFFFFFFF:08x})  {marker}")

        try:
            db.write_dint(args.tag, v0)
        except Exception as e:
            sys.stderr.write(f"[tagtest] FATAL Restore: {e}\n")
            db.close()
            client.close()
            return 2

        try:
            v2 = db.read_dint(args.tag)
        except Exception as e:
            sys.stderr.write(f"[tagtest] FATAL Read post-restore: {e}\n")
            db.close()
            client.close()
            return 2
        passed_restore = v2 == v0
        marker = "<-- RESTORED OK" if passed_restore else "<-- RESTORE DID NOT TAKE"
        print(f"[tagtest] V2 = {v2} (0x{v2 & 0xFFFFFFFF:08x})  {marker}")
    else:
        print("[tagtest] --no-write  skipping write/restore")

    db.close()
    client.close()

    t_end = time.perf_counter()
    if passed_write and passed_restore:
        print(f"\n[tagtest] READ-WRITE-READBACK: PASS  total dt={(t_end - t_start) * 1000:.2f}ms")
        return 0
    print("\n[tagtest] READ-WRITE-READBACK: FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
