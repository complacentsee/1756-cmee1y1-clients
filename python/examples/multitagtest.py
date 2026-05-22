#!/usr/bin/env python3
"""multitagtest — v0.9.0 Phase 2 read_tags validator.

Output mirrors c/examples/multitagtest.c + go/cmd/multitagtest
(modulo timing).

SPDX-License-Identifier: MIT
"""
import argparse
import sys
import time

import bpclient


def now_ms() -> float:
    return time.monotonic() * 1000.0


SCALAR_KIND_LABELS = {
    "bool":  "BOOL",
    "sint":  "SINT",
    "int":   "INT",
    "dint":  "DINT",
    "lint":  "LINT",
    "usint": "USINT",
    "uint":  "UINT",
    "udint": "UDINT",
    "ulint": "ULINT",
    "real":  "REAL",
    "lreal": "LREAL",
}


# Mirrors C / Go logic: derive a label from the Python type.
def label_for(v: object) -> str:
    if isinstance(v, bool):
        return "BOOL"
    if isinstance(v, int):
        # The decoder always returns int for SINT/INT/DINT/LINT and
        # USINT/UINT/UDINT/ULINT.  Without symbol-info we'd lose the
        # type label here — but for the typetest tag set, names start
        # with Test_<TYPE>, so just rely on order.
        return "INT?"
    if isinstance(v, float):
        return "REAL?"
    return "?"


def print_value(name: str, value: object, status: int,
                 type_label: str) -> None:
    if status != 0:
        print(f"[multitagtest] {name}: CIP status 0x{status:02x} (FAIL)")
        return
    if type_label == "BOOL":
        s = "TRUE" if value else "FALSE"
        print(f"[multitagtest] {name} = {s} ({type_label})")
    elif type_label in ("REAL", "LREAL"):
        print(f"[multitagtest] {name} = {float(value):f} ({type_label})")
    else:
        print(f"[multitagtest] {name} = {value} ({type_label})")


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--path", default="P:1,S:2")
    args = ap.parse_args()

    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[multitagtest] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()
        db = c.open_tagdb(args.path)
        try:
            n = db.build()
            print(f"[multitagtest] path={args.path} build n={n}")

            # Dynamically enumerate the first scalar tags so the test
            # works on any PLC.  Cap at 10 to stay inside per-batch limit.
            db.preload_symbols()
            names: list[str] = []
            from bpclient import _proto as P
            for i in range(n):
                if len(names) >= 10:
                    break
                sym = db.symbol_at(i)
                if sym.dim0 != 0 or sym.struct_type != 0 or not sym.name:
                    continue
                t = sym.data_type & 0x1FFF
                if t < P.TYPE_BOOL or t > P.TYPE_LREAL:
                    continue
                names.append(sym.name)
            if not names:
                print("[multitagtest] no scalar tags found on PLC",
                      file=sys.stderr)
                return 1

            t0 = now_ms()
            try:
                results = db.read_tags(names)
                statuses = {n: 0 for n in names}
                rc = 0
            except bpclient.BpGeneric as e:
                # Partial — pull results + statuses off the exception.
                results = getattr(e, "results", {})
                statuses = getattr(e, "statuses", {})
                rc = bpclient.err_code(e)
            t1 = now_ms()
            print(f"[multitagtest] read_tags {len(names)} tags "
                  f"dt={t1 - t0:.2f}ms rc={rc}")

            # Derive a type label per tag from the cached symbol so the
            # output line matches C / Go.
            type_for = {
                P.TYPE_BOOL:  "BOOL",  P.TYPE_SINT: "SINT",
                P.TYPE_INT:   "INT",   P.TYPE_DINT: "DINT",
                P.TYPE_LINT:  "LINT",  P.TYPE_USINT:"USINT",
                P.TYPE_UINT:  "UINT",  P.TYPE_UDINT:"UDINT",
                P.TYPE_ULINT: "ULINT", P.TYPE_REAL: "REAL",
                P.TYPE_LREAL: "LREAL",
            }
            labels: dict[str, str] = {}
            for nm in names:
                s = db.lookup_symbol(nm)
                labels[nm] = type_for.get(s.data_type & 0x1FFF, "?")

            ok, failed = 0, 0
            for name in names:
                value = results.get(name)
                status = statuses.get(name, 0xFF)
                print_value(name, value, status, labels.get(name, "?"))
                if status == 0:
                    ok += 1
                else:
                    failed += 1
            print(f"[multitagtest] SUMMARY ok={ok} failed={failed} "
                  f"total={len(names)}")
            passed = failed == 0
            print(f"[multitagtest] {'PASS' if passed else 'FAIL'}")
            return 0 if passed else 1
        finally:
            db.close()
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
