#!/usr/bin/env python3
"""typetest — exercises every type helper against a configurable
mapping of test tags.  Mirrors c/examples/typetest.c and
go/cmd/typetest/main.go flag-for-flag and output format byte-for-
byte so runprobe.py diff against the C + Go versions round-trips.

SPDX-License-Identifier: MIT
"""
import argparse
import sys

import bpclient

PASS = 0
FAIL = 0


def assert_eq(expected, actual, fmt):
    global PASS, FAIL
    if expected == actual:
        print("    ok")
        PASS += 1
    else:
        print(f"    FAIL: expected {fmt % expected}, got {fmt % actual}")
        FAIL += 1


def assert_feq(expected, actual, eps):
    global PASS, FAIL
    d = abs(expected - actual)
    if d <= eps:
        print("    ok")
        PASS += 1
    else:
        print(f"    FAIL: expected {expected:.9g}, got {actual:.9g} (delta {d:.9g})")
        FAIL += 1


def maybe_err(callable_fn, what):
    """Try `callable_fn`; on exception print FAIL line and return None.
    On success return the value (or True for fns returning None)."""
    global FAIL
    try:
        v = callable_fn()
        return v if v is not None else True
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"    FAIL: {what} -> {bpclient.strerror(rc)} ({rc})")
        FAIL += 1
        return None


def test_sint(db, tag):
    if not tag:
        return
    print(f"\n[sint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_sint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = {v0}")
    probe = 42
    if maybe_err(lambda: db.write_sint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_sint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = {v1} (probe={probe})")
    assert_eq(probe, v1, "%d")
    if maybe_err(lambda: db.write_sint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_sint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = {v2} (restore={v0})")
    assert_eq(v0, v2, "%d")


def test_int(db, tag):
    if not tag:
        return
    print(f"\n[int scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_int(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = {v0}")
    probe = 12345
    if maybe_err(lambda: db.write_int(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_int(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = {v1} (probe={probe})")
    assert_eq(probe, v1, "%d")
    if maybe_err(lambda: db.write_int(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_int(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = {v2} (restore={v0})")
    assert_eq(v0, v2, "%d")


def test_dint(db, tag):
    if not tag:
        return
    print(f"\n[dint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_dint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = 0x{v0 & 0xFFFFFFFF:08x}")
    probe = -559038737  # int32 of 0xDEADBEEF
    if maybe_err(lambda: db.write_dint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_dint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = 0x{v1 & 0xFFFFFFFF:08x} (probe=0x{probe & 0xFFFFFFFF:08x})")
    assert_eq(probe & 0xFFFFFFFF, v1 & 0xFFFFFFFF, "0x%08x")
    if maybe_err(lambda: db.write_dint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_dint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = 0x{v2 & 0xFFFFFFFF:08x} (restore=0x{v0 & 0xFFFFFFFF:08x})")
    assert_eq(v0 & 0xFFFFFFFF, v2 & 0xFFFFFFFF, "0x%08x")


def test_lint(db, tag):
    if not tag:
        return
    print(f"\n[lint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_lint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = 0x{v0 & 0xFFFFFFFFFFFFFFFF:016x}")
    probe = 0x0123456789ABCDEF
    if maybe_err(lambda: db.write_lint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_lint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = 0x{v1 & 0xFFFFFFFFFFFFFFFF:016x} "
          f"(probe=0x{probe & 0xFFFFFFFFFFFFFFFF:016x})")
    assert_eq(probe & 0xFFFFFFFFFFFFFFFF, v1 & 0xFFFFFFFFFFFFFFFF, "0x%016x")
    if maybe_err(lambda: db.write_lint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_lint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = 0x{v2 & 0xFFFFFFFFFFFFFFFF:016x} "
          f"(restore=0x{v0 & 0xFFFFFFFFFFFFFFFF:016x})")
    assert_eq(v0 & 0xFFFFFFFFFFFFFFFF, v2 & 0xFFFFFFFFFFFFFFFF, "0x%016x")


def test_usint(db, tag):
    if not tag:
        return
    print(f"\n[usint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_usint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = {v0}")
    probe = 0xAB
    if maybe_err(lambda: db.write_usint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_usint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = {v1} (probe={probe})")
    assert_eq(probe, v1, "%d")
    if maybe_err(lambda: db.write_usint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_usint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = {v2} (restore={v0})")
    assert_eq(v0, v2, "%d")


def test_uint(db, tag):
    if not tag:
        return
    print(f"\n[uint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_uint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = 0x{v0:04x}")
    probe = 0xCAFE
    if maybe_err(lambda: db.write_uint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_uint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = 0x{v1:04x} (probe=0x{probe:04x})")
    assert_eq(probe, v1, "0x%04x")
    if maybe_err(lambda: db.write_uint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_uint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = 0x{v2:04x} (restore=0x{v0:04x})")
    assert_eq(v0, v2, "0x%04x")


def test_udint(db, tag):
    if not tag:
        return
    print(f"\n[udint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_udint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = 0x{v0:08x}")
    probe = 0xFEEDFACE
    if maybe_err(lambda: db.write_udint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_udint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = 0x{v1:08x} (probe=0x{probe:08x})")
    assert_eq(probe, v1, "0x%08x")
    if maybe_err(lambda: db.write_udint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_udint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = 0x{v2:08x} (restore=0x{v0:08x})")
    assert_eq(v0, v2, "0x%08x")


def test_ulint(db, tag):
    if not tag:
        return
    print(f"\n[ulint scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_ulint(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = 0x{v0:016x}")
    probe = 0xCAFEBABEDEADBEEF
    if maybe_err(lambda: db.write_ulint(tag, probe), "write probe") is None:
        return
    v1 = maybe_err(lambda: db.read_ulint(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = 0x{v1:016x} (probe=0x{probe:016x})")
    assert_eq(probe, v1, "0x%016x")
    if maybe_err(lambda: db.write_ulint(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_ulint(tag), "post-restore read")
    if v2 is None:
        return
    print(f"  V2 = 0x{v2:016x} (restore=0x{v0:016x})")
    assert_eq(v0, v2, "0x%016x")


def test_real(db, tag):
    if not tag:
        return
    print(f"\n[real scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_real(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = {v0:.6g}")
    probe = 3.14159
    if maybe_err(lambda: db.write_real(tag, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_real(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = {v1:.6g} (probe={probe:.6g})")
    assert_feq(probe, v1, 1e-5)
    if maybe_err(lambda: db.write_real(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_real(tag), "post-restore read")
    if v2 is None:
        return
    assert_feq(v0, v2, 1e-5)


def test_lreal(db, tag):
    if not tag:
        return
    print(f"\n[lreal scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_lreal(tag), "initial read")
    if v0 is None:
        return
    print(f"  V0 = {v0:.15g}")
    probe = 2.71828182845904523
    if maybe_err(lambda: db.write_lreal(tag, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_lreal(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = {v1:.15g} (probe={probe:.15g})")
    assert_feq(probe, v1, 1e-12)
    if maybe_err(lambda: db.write_lreal(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_lreal(tag), "post-restore read")
    if v2 is None:
        return
    assert_feq(v0, v2, 1e-12)


def test_bool(db, tag):
    if not tag:
        return
    print(f"\n[bool scalar] tag={tag}")
    v0 = maybe_err(lambda: db.read_bool(tag), "initial read")
    if v0 is None:
        return
    v0i = 1 if v0 else 0
    print(f"  V0 = {v0i}")
    probe = not v0
    probe_i = 1 if probe else 0
    if maybe_err(lambda: db.write_bool(tag, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_bool(tag), "post-write read")
    if v1 is None:
        return
    v1i = 1 if v1 else 0
    print(f"  V1 = {v1i} (probe={probe_i})")
    assert_eq(probe_i, v1i, "%d")
    if maybe_err(lambda: db.write_bool(tag, v0), "restore") is None:
        return
    v2 = maybe_err(lambda: db.read_bool(tag), "post-restore read")
    if v2 is None:
        return
    v2i = 1 if v2 else 0
    assert_eq(v0i, v2i, "%d")


def test_string(db, tag):
    global PASS, FAIL
    if not tag:
        return
    print(f"\n[string] tag={tag}")
    try:
        v0 = db.read_string(tag)
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"    FAIL: initial read -> {bpclient.strerror(rc)} ({rc})")
        FAIL += 1
        return
    print(f"  V0 = '{v0}' (len={len(v0)})")
    probe = "hello from bpclient typetest"
    if maybe_err(lambda: db.write_string(tag, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_string(tag), "post-write read")
    if v1 is None:
        return
    print(f"  V1 = '{v1}' (len={len(v1)})")
    if v1 == probe:
        print("    ok")
        PASS += 1
    else:
        print("    FAIL: string mismatch")
        FAIL += 1
    maybe_err(lambda: db.write_string(tag, v0), "restore")


def test_bool_array(db, tag, count):
    global PASS, FAIL
    if not tag or count <= 0:
        return
    print(f"\n[bool array] tag={tag} count={count}")
    try:
        v0 = db.read_bool_array(tag, count)
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"    FAIL: initial read -> {bpclient.strerror(rc)} ({rc})")
        FAIL += 1
        return
    top = min(count, 16)
    bits = " ".join(str(1 if v0[i] else 0) for i in range(top))
    suffix = " ..." if count > 16 else ""
    print(f"  V0[0..{top - 1}] = {bits}{suffix}")

    probe = [(i & 1) == 0 for i in range(count)]
    try:
        db.write_bool_array(tag, probe)
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"    FAIL: write -> {bpclient.strerror(rc)} ({rc})")
        FAIL += 1
        return
    try:
        v1 = db.read_bool_array(tag, count)
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"    FAIL: readback -> {bpclient.strerror(rc)} ({rc})")
        FAIL += 1
        return
    all_match = True
    for i in range(count):
        if v1[i] != probe[i]:
            exp_i = 1 if probe[i] else 0
            got_i = 1 if v1[i] else 0
            print(f"    MISMATCH at bit[{i}]: expected {exp_i}, got {got_i}")
            all_match = False
    if all_match:
        print(f"    ok (all {count} bits match)")
        PASS += 1
    else:
        FAIL += 1
    try:
        db.write_bool_array(tag, v0)
    except Exception:
        pass


def test_dint_array(db, tag, count):
    global PASS, FAIL
    if not tag or count <= 0:
        return
    print(f"\n[dint array] tag={tag} count={count}")
    v0 = maybe_err(lambda: db.read_dint_array(tag, count), "initial read")
    if v0 is None:
        return
    top = min(count, 8)
    vals = " ".join(str(v0[i]) for i in range(top))
    suffix = " ..." if count > 8 else ""
    print(f"  V0[0..{top - 1}] = {vals}{suffix}")

    probe = [0x1000 + i for i in range(count)]
    if maybe_err(lambda: db.write_dint_array(tag, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_dint_array(tag, count), "post-write read")
    if v1 is None:
        return
    all_match = True
    for i in range(count):
        if v1[i] != probe[i]:
            print(f"    MISMATCH at [{i}]: expected {probe[i]}, got {v1[i]}")
            all_match = False
    if all_match:
        print(f"    ok (all {count} elements match)")
        PASS += 1
    else:
        FAIL += 1
    maybe_err(lambda: db.write_dint_array(tag, v0), "restore")


def test_dint_2d(db, tag, dim0, dim1):
    global PASS, FAIL
    if not tag or dim0 <= 0 or dim1 <= 0:
        return
    total = dim0 * dim1
    print(f"\n[dint 2-D] tag={tag} dims={dim0},{dim1} (total={total})")
    zero_idx = f"{tag}[0,0]"
    v0 = maybe_err(lambda: db.read_dint_array(zero_idx, total), "initial read")
    if v0 is None:
        return
    print(f"  V0 (first 6 = first 2 rows): {v0[0]} {v0[1]} {v0[2]} "
          f"{v0[3]} {v0[4]} {v0[5]}")

    probe = [0] * total
    for r in range(dim0):
        for c in range(dim1):
            probe[r * dim1 + c] = 1000 * r + c
    if maybe_err(lambda: db.write_dint_array(zero_idx, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_dint_array(zero_idx, total), "post-write read")
    if v1 is None:
        return
    all_match = all(v1[i] == probe[i] for i in range(total))
    if all_match:
        print(f"    ok (batched readback row-major matches all {total})")
        PASS += 1
    else:
        print("    FAIL: batched readback mismatch")
        FAIL += 1

    mr, mc = dim0 // 2, dim1 // 2
    idx = f"{tag}[{mr},{mc}]"
    spot = maybe_err(lambda: db.read_dint(idx), "indexed read")
    if spot is None:
        return
    expect = 1000 * mr + mc
    ok_str = "ok" if spot == expect else "FAIL"
    if spot == expect:
        PASS += 1
    else:
        FAIL += 1
    print(f"  {idx} = {spot} (expect {expect}) {ok_str}")

    maybe_err(lambda: db.write_dint_array(zero_idx, v0), "restore")


def test_dint_3d(db, tag, dim0, dim1, dim2):
    global PASS, FAIL
    if not tag or dim0 <= 0 or dim1 <= 0 or dim2 <= 0:
        return
    total = dim0 * dim1 * dim2
    print(f"\n[dint 3-D] tag={tag} dims={dim0},{dim1},{dim2} (total={total})")
    zero_idx = f"{tag}[0,0,0]"
    v0 = maybe_err(lambda: db.read_dint_array(zero_idx, total), "initial read")
    if v0 is None:
        return
    print(f"  V0 (first 6 = first plane row): {v0[0]} {v0[1]} {v0[2]} "
          f"{v0[3]} {v0[4]} {v0[5]}")

    probe = [0] * total
    for i in range(dim0):
        for j in range(dim1):
            for k in range(dim2):
                lin = i * (dim1 * dim2) + j * dim2 + k
                probe[lin] = 100000 * i + 1000 * j + k
    if maybe_err(lambda: db.write_dint_array(zero_idx, probe), "write") is None:
        return
    v1 = maybe_err(lambda: db.read_dint_array(zero_idx, total), "post-write read")
    if v1 is None:
        return
    all_match = all(v1[i] == probe[i] for i in range(total))
    if all_match:
        print(f"    ok (batched readback row-major matches all {total})")
        PASS += 1
    else:
        print("    FAIL: batched readback mismatch")
        FAIL += 1

    mi, mj, mk = dim0 // 2, dim1 // 2, dim2 // 2
    idx = f"{tag}[{mi},{mj},{mk}]"
    spot = maybe_err(lambda: db.read_dint(idx), "indexed read")
    if spot is None:
        return
    expect = 100000 * mi + 1000 * mj + mk
    ok_str = "ok" if spot == expect else "FAIL"
    if spot == expect:
        PASS += 1
    else:
        FAIL += 1
    print(f"  {idx} = {spot} (expect {expect}) {ok_str}")

    maybe_err(lambda: db.write_dint_array(zero_idx, v0), "restore")


def main():
    ap = argparse.ArgumentParser(prog="typetest")
    ap.add_argument("--path", default="P:1,S:1")
    ap.add_argument("--bool", dest="tag_bool", default="")
    ap.add_argument("--sint", dest="tag_sint", default="")
    ap.add_argument("--int", dest="tag_int", default="")
    ap.add_argument("--dint", dest="tag_dint", default="")
    ap.add_argument("--lint", dest="tag_lint", default="")
    ap.add_argument("--usint", dest="tag_usint", default="")
    ap.add_argument("--uint", dest="tag_uint", default="")
    ap.add_argument("--udint", dest="tag_udint", default="")
    ap.add_argument("--ulint", dest="tag_ulint", default="")
    ap.add_argument("--real", dest="tag_real", default="")
    ap.add_argument("--lreal", dest="tag_lreal", default="")
    ap.add_argument("--string", dest="tag_string", default="")
    ap.add_argument("--dint-array", dest="dint_arr", default="")
    ap.add_argument("--array-count", type=int, default=0)
    ap.add_argument("--bool-array", dest="bool_arr", default="")
    ap.add_argument("--bool-array-count", type=int, default=0)
    ap.add_argument("--dint-2d", dest="dint_2d", default="")
    ap.add_argument("--dint-2d-dim0", type=int, default=0)
    ap.add_argument("--dint-2d-dim1", type=int, default=0)
    ap.add_argument("--dint-3d", dest="dint_3d", default="")
    ap.add_argument("--dint-3d-dim0", type=int, default=0)
    ap.add_argument("--dint-3d-dim1", type=int, default=0)
    ap.add_argument("--dint-3d-dim2", type=int, default=0)
    args = ap.parse_args()

    try:
        client = bpclient.Client()
        client.open()
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"FATAL Open: {bpclient.strerror(rc)}")
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    try:
        db = client.open_tagdb(args.path)
    except Exception as e:
        rc = bpclient.err_code(e)
        print(f"FATAL OpenTagDB: {bpclient.strerror(rc)}")
        client.close()
        return 2
    n = db.build()
    print(f"[typetest] path={args.path} symbols={n}")

    test_bool (db, args.tag_bool)
    test_sint (db, args.tag_sint)
    test_int  (db, args.tag_int)
    test_dint (db, args.tag_dint)
    test_lint (db, args.tag_lint)
    test_usint(db, args.tag_usint)
    test_uint (db, args.tag_uint)
    test_udint(db, args.tag_udint)
    test_ulint(db, args.tag_ulint)
    test_real (db, args.tag_real)
    test_lreal(db, args.tag_lreal)
    test_string(db, args.tag_string)
    test_dint_array(db, args.dint_arr, args.array_count)
    test_bool_array(db, args.bool_arr, args.bool_array_count)
    test_dint_2d(db, args.dint_2d, args.dint_2d_dim0, args.dint_2d_dim1)
    test_dint_3d(db, args.dint_3d, args.dint_3d_dim0,
                 args.dint_3d_dim1, args.dint_3d_dim2)

    db.close()
    client.close()

    print(f"\n[typetest] PASS={PASS} FAIL={FAIL}")
    return 0 if FAIL == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
