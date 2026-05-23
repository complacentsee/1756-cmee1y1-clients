#!/usr/bin/env python3
"""accessdbtest — v0.10.4 cross-validation of TagDB.access_db vs
TagDB.access.  Mirrors c/examples/accessdbtest.c and the Go
cmd/accessdbtest output format byte-for-byte so a runprobe.py diff
across the three SDKs round-trips (modulo timing).

Exit 0 on PASS, 1 on FAIL, 2 on FATAL.

SPDX-License-Identifier: MIT
"""
import argparse
import struct
import sys
import time

import bpclient
from bpclient import _proto as P
from bpclient.access import TagRequest


def _read_dint(db, tag, use_db_path):
    r = TagRequest(
        tag_name=tag,
        data_type=P.TYPE_DINT,
        elem_byte_size=4,
        action=P.ACTION_READ,
        elem_count=1,
        data=b"\x00" * 4,
    )
    if use_db_path:
        db.access_db([r])
    else:
        db.access([r])
    value = struct.unpack("<i", r.data[:4])[0] if r.data else 0
    return value, r.result


def _write_dint(db, tag, use_db_path, value):
    r = TagRequest(
        tag_name=tag,
        data_type=P.TYPE_DINT,
        elem_byte_size=4,
        action=P.ACTION_WRITE,
        elem_count=1,
        data=struct.pack("<i", value),
    )
    if use_db_path:
        db.access_db([r])
    else:
        db.access([r])
    return r.result


def _run_batch(db, tag, expect, use_db_path, batch_n, iters, samples):
    """Fire `iters` batches of `batch_n` reads of `tag` through one
    path. Returns the failure count (any exception, non-zero CIP
    status, or any value mismatch against expect)."""
    failures = 0
    for i in range(iters):
        reqs = [
            TagRequest(
                tag_name=tag,
                data_type=P.TYPE_DINT,
                elem_byte_size=4,
                action=P.ACTION_READ,
                elem_count=1,
                data=b"\x00" * 4,
            )
            for _ in range(batch_n)
        ]
        a = time.perf_counter()
        try:
            if use_db_path:
                db.access_db(reqs)
            else:
                db.access(reqs)
        except Exception:
            samples[i] = (time.perf_counter() - a) * 1000.0
            failures += 1
            continue
        samples[i] = (time.perf_counter() - a) * 1000.0
        for r in reqs:
            v = struct.unpack("<i", r.data[:4])[0] if r.data else 0
            if r.result != 0 or v != expect:
                failures += 1
                break
    return failures


def main() -> int:
    ap = argparse.ArgumentParser(prog="accessdbtest")
    ap.add_argument("--path", default="P:1,S:2")
    ap.add_argument("--tag", default="OCX_TEST")
    ap.add_argument("--iters", type=int, default=100)
    ap.add_argument("--batch", type=int, default=1)
    ap.add_argument("--write", action="store_true")
    args = ap.parse_args()

    iters = max(1, min(args.iters, 10000))
    batch_n = max(1, min(args.batch, 16))
    print(
        f"[accessdbtest] path={args.path} tag={args.tag} iters={iters} "
        f"batch={batch_n} write={'yes' if args.write else 'no'}"
    )

    try:
        client = bpclient.Client()
        client.open()
    except Exception as e:
        sys.stderr.write(f"[accessdbtest] FATAL Open: {e}\n")
        return 2
    try:
        client.open_session()
    except Exception as e:
        sys.stderr.write(f"[accessdbtest] FATAL OpenSession: {e}\n")
        client.close()
        return 2
    try:
        db = client.open_tagdb(args.path)
    except Exception as e:
        sys.stderr.write(f"[accessdbtest] FATAL OpenTagDB({args.path}): {e}\n")
        client.close()
        return 2
    try:
        n_symbols = db.build()
    except Exception as e:
        sys.stderr.write(f"[accessdbtest] FATAL Build: {e}\n")
        db.close()
        client.close()
        return 2
    print(f"[accessdbtest] Build ok  symbols={n_symbols}")

    # Single-shot OLD vs NEW correctness
    try:
        v_old, cip_old = _read_dint(db, args.tag, False)
    except Exception as e:
        sys.stderr.write(f"[accessdbtest] FATAL Read OLD: {e}\n")
        db.close(); client.close(); return 2
    if cip_old != 0:
        sys.stderr.write(f"[accessdbtest] FATAL Read OLD cip=0x{cip_old:08x}\n")
        db.close(); client.close(); return 2
    try:
        v_new, cip_new = _read_dint(db, args.tag, True)
    except Exception as e:
        sys.stderr.write(f"[accessdbtest] FATAL Read NEW: {e}\n")
        db.close(); client.close(); return 2
    if cip_new != 0:
        sys.stderr.write(f"[accessdbtest] FATAL Read NEW cip=0x{cip_new:08x}\n")
        db.close(); client.close(); return 2
    if v_old != v_new:
        sys.stderr.write(
            f"[accessdbtest] FAIL  OLD={v_old} (0x{v_old & 0xFFFFFFFF:08x}) "
            f"NEW={v_new} (0x{v_new & 0xFFFFFFFF:08x})\n")
        db.close(); client.close(); return 1
    print(
        f"[accessdbtest] correctness: OLD == NEW = {v_old} "
        f"(0x{v_old & 0xFFFFFFFF:08x})"
    )

    # Latency sweep
    t_old = [0.0] * iters
    t_new = [0.0] * iters
    fail_old = _run_batch(db, args.tag, v_old, False, batch_n, iters, t_old)
    fail_new = _run_batch(db, args.tag, v_old, True,  batch_n, iters, t_new)
    t_old.sort(); t_new.sort()
    p50 = iters // 2
    p99 = min((iters * 99) // 100, iters - 1)
    per_tag_old = t_old[p50] / float(batch_n)
    per_tag_new = t_new[p50] / float(batch_n)
    speedup = ((per_tag_old - per_tag_new) * 100.0 / per_tag_old
               if per_tag_old > 0 else 0.0)
    print(f"[accessdbtest] latency over {iters} batches of {batch_n} reads each path:")
    print(f"  OLD path  median={t_old[p50]:6.3f}ms  p99={t_old[p99]:6.3f}ms  "
          f"min={t_old[0]:6.3f}ms  max={t_old[-1]:6.3f}ms  per-tag={per_tag_old:6.3f}ms")
    print(f"  NEW path  median={t_new[p50]:6.3f}ms  p99={t_new[p99]:6.3f}ms  "
          f"min={t_new[0]:6.3f}ms  max={t_new[-1]:6.3f}ms  per-tag={per_tag_new:6.3f}ms")
    print(f"  NEW vs OLD per-tag delta: {speedup:+.2f}% (positive = NEW faster)")
    if fail_old or fail_new:
        print(
            f"[accessdbtest] FAIL  OLD fails={fail_old}  NEW fails={fail_new}  "
            f"(of {iters} batches each)"
        )
        db.close(); client.close(); return 1

    passed_write = passed_restore = True
    if args.write:
        sentinel = -559038737  # int32 of 0xDEADBEEF
        try:
            cip = _write_dint(db, args.tag, True, sentinel)
        except Exception as e:
            sys.stderr.write(f"[accessdbtest] FATAL Write NEW: {e}\n")
            db.close(); client.close(); return 2
        if cip != 0:
            sys.stderr.write(f"[accessdbtest] FATAL Write NEW cip=0x{cip:08x}\n")
            db.close(); client.close(); return 2
        try:
            v_back, cip = _read_dint(db, args.tag, False)
        except Exception as e:
            sys.stderr.write(f"[accessdbtest] FATAL Readback OLD: {e}\n")
            db.close(); client.close(); return 2
        if cip != 0:
            sys.stderr.write(f"[accessdbtest] FATAL Readback OLD cip=0x{cip:08x}\n")
            db.close(); client.close(); return 2
        passed_write = v_back == sentinel
        marker = "<-- OK" if passed_write else "<-- WRITE DID NOT TAKE"
        print(f"[accessdbtest] NEW-write -> OLD-readback = 0x{v_back & 0xFFFFFFFF:08x}  {marker}")

        try:
            cip = _write_dint(db, args.tag, True, v_old)
        except Exception as e:
            sys.stderr.write(f"[accessdbtest] FATAL Restore NEW: {e}\n")
            db.close(); client.close(); return 2
        if cip != 0:
            sys.stderr.write(f"[accessdbtest] FATAL Restore NEW cip=0x{cip:08x}\n")
            db.close(); client.close(); return 2
        try:
            v_back, cip = _read_dint(db, args.tag, False)
        except Exception as e:
            sys.stderr.write(f"[accessdbtest] FATAL Confirm OLD: {e}\n")
            db.close(); client.close(); return 2
        if cip != 0:
            sys.stderr.write(f"[accessdbtest] FATAL Confirm OLD cip=0x{cip:08x}\n")
            db.close(); client.close(); return 2
        passed_restore = v_back == v_old
        marker = "<-- OK" if passed_restore else "<-- RESTORE DID NOT TAKE"
        print(f"[accessdbtest] NEW-restore -> OLD-readback = {v_back}  {marker}")

    db.close()
    client.close()
    if passed_write and passed_restore:
        print("\n[accessdbtest] PASS")
        return 0
    print("\n[accessdbtest] FAIL")
    return 1


if __name__ == "__main__":
    sys.exit(main())
