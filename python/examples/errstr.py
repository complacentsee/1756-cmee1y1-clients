#!/usr/bin/env python3
"""errstr — v0.10.0 Phase A validator for client.error_string.

Output mirrors c/examples/errstr.c + go/cmd/errstr.

SPDX-License-Identifier: MIT
"""
import sys

import bpclient


CODES = [
    0, -1, -200, -201, -300, -301, -303, -305, -311, -400,
    -101802, -103001,
    1, 3, 8, 14, 0x14, 0x15,
]


def main() -> int:
    try:
        c = bpclient.Client()
        c.open()
    except Exception as e:
        print(f"[errstr] open failed: {e}", file=sys.stderr)
        return 2
    try:
        c.open_session()
        # The engine's lookup table only contains POSITIVE engine codes;
        # negative OCX_ERR_* are wrapper-side and the engine returns
        # rc=1 ("Bad parameter") for them.  PASS just verifies the
        # dispatch path works.
        engine_hits = 0
        for code in CODES:
            local = bpclient.strerror(code)
            try:
                engine = c.error_string(code)
                print(f"[errstr] code={code:<8} local=\"{local}\"  engine=\"{engine}\"")
                if engine:
                    engine_hits += 1
            except Exception as e:
                rc = bpclient.err_code(e)
                if rc == 1:
                    print(f"[errstr] code={code:<8} local=\"{local}\"  engine=<not in table>")
                else:
                    print(f"[errstr] code={code:<8} local=\"{local}\"  engine=<rc={rc}>")
        passed = engine_hits >= 1
        print(f"[errstr] engine table hits: {engine_hits}/{len(CODES)}")
        print(f"[errstr] {'PASS' if passed else 'FAIL'}")
        return 0 if passed else 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
