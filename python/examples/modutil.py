#!/usr/bin/env python3
"""modutil — local cm1756 module utilities (switch / display / LED).
Mirrors c/examples/modutil.c CLI + output format byte-for-byte.

SPDX-License-Identifier: MIT
"""
import argparse
import sys

import bpclient


def main() -> int:
    ap = argparse.ArgumentParser(prog="modutil")
    ap.add_argument("--display", default=None)
    ap.add_argument("--led", type=int, default=-1)
    ap.add_argument("--state", type=int, default=-1)
    ap.add_argument("--led-get", type=int, default=-1, dest="led_get")
    args = ap.parse_args()

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

    # Switch
    try:
        sw = client.get_switch_position()
        rc = 0
    except Exception as e:
        sw = 0
        rc = bpclient.err_code(e)
    print(f"[switch] rc={rc} ({bpclient.strerror(rc)})  position={sw} (0x{sw:08x})")

    # Display read
    try:
        disp = client.get_display()
        rc = 0
    except Exception as e:
        disp = b"\x00\x00\x00\x00\x00"
        rc = bpclient.err_code(e)
    text = disp[:4].split(b"\x00", 1)[0].decode("ascii", "replace")
    print(f"[display read] rc={rc} ({bpclient.strerror(rc)})  text='{text}' "
          f"(hex {disp[0]:02x} {disp[1]:02x} {disp[2]:02x} {disp[3]:02x})")

    if args.display is not None:
        four = (args.display + "    ")[:4].encode("ascii", "replace")
        try:
            client.set_display(four)
            rc = 0
        except Exception as e:
            rc = bpclient.err_code(e)
        print(f"[display set]  rc={rc} ({bpclient.strerror(rc)})  wrote '{four.decode('ascii')}'")
        try:
            back = client.get_display()
            print(f"[display read-after-write]  text='{back[:4].decode('ascii', 'replace').rstrip(chr(0))}'")
        except Exception:
            pass

    if args.led_get >= 0:
        try:
            state = client.get_led(args.led_get)
            rc = 0
        except Exception as e:
            state = 0
            rc = bpclient.err_code(e)
        print(f"[led get] rc={rc} ({bpclient.strerror(rc)})  id={args.led_get}  "
              f"state={state} (0x{state:08x})")

    if args.led >= 0 and args.state >= 0:
        try:
            client.set_led(args.led, args.state)
            rc = 0
        except Exception as e:
            rc = bpclient.err_code(e)
        print(f"[led set] rc={rc} ({bpclient.strerror(rc)})  id={args.led} "
              f"-> state={args.state}")
        try:
            state = client.get_led(args.led)
            print(f"[led read-after-write] id={args.led}  state={state}")
        except Exception:
            pass

    client.close()
    return 0


if __name__ == "__main__":
    sys.exit(main())
