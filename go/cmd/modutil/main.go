// modutil — exercise local cm1756 module utilities:
//
//	--display "ABCD"      set the 4-char display
//	--led <id> --state <0|1>  toggle an LED
//	--led-get <id>        print one LED's current state
//
// Always prints the current switch position and display contents.
// Mirrors c/examples/modutil.c CLI args.
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
	setDisplay := flag.String("display", "", "set 4-char display to this value (truncated/padded)")
	ledSetID := flag.Int("led", -1, "LED id to write (with --state)")
	ledSetState := flag.Int("state", -1, "LED state to write")
	ledGetID := flag.Int("led-get", -1, "LED id to read")
	flag.Parse()

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "client open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	sw, err := client.GetSwitchPosition()
	rc := ocxbp.ErrCode(err)
	fmt.Printf("[switch] rc=%d (%s)  position=%d (0x%08x)\n",
		rc, ocxbp.Strerror(rc), sw, sw)

	disp, err := client.GetDisplay()
	rc = ocxbp.ErrCode(err)
	fmt.Printf("[display read] rc=%d (%s)  text='%s' (hex %02x %02x %02x %02x)\n",
		rc, ocxbp.Strerror(rc),
		nullTrim(disp[:4]),
		disp[0], disp[1], disp[2], disp[3])

	if *setDisplay != "" {
		four := [4]byte{' ', ' ', ' ', ' '}
		n := len(*setDisplay)
		if n > 4 {
			n = 4
		}
		copy(four[:n], *setDisplay)
		err := client.SetDisplay(four)
		rc := ocxbp.ErrCode(err)
		fmt.Printf("[display set]  rc=%d (%s)  wrote '%c%c%c%c'\n",
			rc, ocxbp.Strerror(rc), four[0], four[1], four[2], four[3])
		if back, err := client.GetDisplay(); err == nil {
			fmt.Printf("[display read-after-write]  text='%s'\n", nullTrim(back[:4]))
		}
	}

	if *ledGetID >= 0 {
		state, err := client.GetLED(uint32(*ledGetID))
		rc := ocxbp.ErrCode(err)
		fmt.Printf("[led get] rc=%d (%s)  id=%d  state=%d (0x%08x)\n",
			rc, ocxbp.Strerror(rc), *ledGetID, state, state)
	}

	if *ledSetID >= 0 && *ledSetState >= 0 {
		err := client.SetLED(uint32(*ledSetID), uint32(*ledSetState))
		rc := ocxbp.ErrCode(err)
		fmt.Printf("[led set] rc=%d (%s)  id=%d -> state=%d\n",
			rc, ocxbp.Strerror(rc), *ledSetID, *ledSetState)
		if state, err := client.GetLED(uint32(*ledSetID)); err == nil {
			fmt.Printf("[led read-after-write] id=%d  state=%d\n", *ledSetID, state)
		}
	}
}

func nullTrim(b []byte) string {
	for i, c := range b {
		if c == 0 {
			return string(b[:i])
		}
	}
	return string(b)
}
