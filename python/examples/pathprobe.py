#!/usr/bin/env python3
"""pathprobe — invoke OCXcip_ParsePath and dump the encoded EPATH
bytes.  Mirrors c/examples/pathprobe.c CLI + output format.

ParsePath is a debug-only opcode (not in the public Client API).
Dispatched via Client.raw with hand-rolled fill/read.

SPDX-License-Identifier: MIT
"""
import struct
import sys

import bpclient
from bpclient import _proto as P
from bpclient.errors import BpEngine

PP_PATH_OFF = 0x078
PP_CLASS_OFF = 0x178
PP_SEGFLAGS_OFF = 0x17A
PP_INSTANCE_OFF = 0x17C
PP_ENCODED_OFF = 0x180
PP_SIZE_OFF = 0x280
PP_ATTRFLAGS_OFF = 0x282
PP_PAYLOAD_SIZE = 0x288


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

    path_bytes = text_path.encode("ascii", "strict")
    state = {"encoded": b"", "encoded_len": 0,
             "out_class": 0, "out_seg_flags": 0,
             "out_instance": 0, "out_attr_flags": 0}

    def fill(slot):
        slot[PP_PATH_OFF:PP_PATH_OFF + len(path_bytes)] = path_bytes
        slot[PP_PATH_OFF + len(path_bytes)] = 0
        struct.pack_into("<H", slot, PP_SIZE_OFF, 256)

    def read(slot):
        n = struct.unpack_from("<H", slot, PP_SIZE_OFF)[0]
        if n > 256:
            n = 256
        state["encoded_len"] = n
        state["encoded"] = bytes(slot[PP_ENCODED_OFF:PP_ENCODED_OFF + n])
        state["out_class"] = struct.unpack_from("<H", slot, PP_CLASS_OFF)[0]
        state["out_seg_flags"] = slot[PP_SEGFLAGS_OFF]
        state["out_instance"] = struct.unpack_from("<I", slot, PP_INSTANCE_OFF)[0]
        state["out_attr_flags"] = slot[PP_ATTRFLAGS_OFF]

    rc = 0
    try:
        client.raw.call("OCXcip_ParsePath", PP_PAYLOAD_SIZE,
                        fill=fill, read=read, timeout_ms=5000)
    except BpEngine as e:
        rc = e.code
    except Exception as e:
        rc = bpclient.err_code(e)

    print(f"[pathprobe] text='{text_path}'  rc={rc}  "
          f"encoded_len={state['encoded_len']}")
    if rc != 0:
        client.close()
        return 1

    print(f"  out_class      = 0x{state['out_class']:04x}")
    print(f"  out_seg_flags  = 0x{state['out_seg_flags']:02x}")
    print(f"  out_instance   = 0x{state['out_instance']:08x}  ({state['out_instance']})")
    print(f"  out_attr_flags = 0x{state['out_attr_flags']:02x}")
    encoded_hex = " ".join(f"{b:02x}" for b in state['encoded'])
    print(f"  encoded path   = {encoded_hex} ")
    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
