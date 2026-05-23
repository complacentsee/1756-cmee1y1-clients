#!/usr/bin/env python3
"""pathprobe — invoke Client.parse_path and dump the encoded EPATH.
Mirrors c/examples/pathprobe.c CLI + output format.

SPDX-License-Identifier: MIT
"""
import sys

import bpclient
from bpclient.errors import BpEngine


def main() -> int:
    if len(sys.argv) < 2:
        sys.stderr.write(f"Usage: {sys.argv[0]} <text-path>\n")
        return 2
    text_path = sys.argv[1]

    try:
        client = bpclient.Client()
        client.open()
    except Exception:
        sys.stderr.write("client open failed\n")
        return 2
    try:
        client.open_session()
    except Exception:
        pass

    rc = 0
    parsed = None
    try:
        parsed = client.parse_path(text_path)
    except BpEngine as e:
        rc = e.code
    except Exception as e:
        rc = bpclient.err_code(e)

    encoded_len = len(parsed.encoded) if parsed else 0
    print(f"[pathprobe] text='{text_path}'  rc={rc}  "
          f"encoded_len={encoded_len}")
    if rc != 0 or parsed is None:
        client.close()
        return 1

    print(f"  out_class      = 0x{parsed.cip_class:04x}")
    print(f"  out_seg_flags  = 0x{parsed.segment_flags:02x}")
    print(f"  out_instance   = 0x{parsed.instance:08x}  ({parsed.instance})")
    print(f"  out_attr_flags = 0x{parsed.attr_flags:02x}")
    encoded_hex = " ".join(f"{b:02x}" for b in parsed.encoded)
    print(f"  encoded path   = {encoded_hex} ")
    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
