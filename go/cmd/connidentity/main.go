// connidentity — query a device's Identity via class-3 connected
// messaging (Phase 5).  Mirrors c/examples/connidentity.c CLI + output.
//
// STATUS: NOT FUNCTIONAL on cm1756 — the OCXCN_OpenClass3Connection
// library is missing from the chip image, so OpenConn returns 0x1001.
// This binary is kept for parity with the C tooling so the engine
// failure path can still be diffed across languages.
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

// parseHex from a "01 02 20 01" hex string.
func parseHex(s string) []byte {
	out := make([]byte, 0, len(s)/2)
	i := 0
	for i < len(s) {
		for i < len(s) && !isHex(s[i]) {
			i++
		}
		if i+1 >= len(s) || !isHex(s[i+1]) {
			break
		}
		var v byte
		for _, c := range s[i : i+2] {
			v <<= 4
			switch {
			case '0' <= c && c <= '9':
				v |= byte(c - '0')
			case 'a' <= c && c <= 'f':
				v |= byte(c-'a') + 10
			case 'A' <= c && c <= 'F':
				v |= byte(c-'A') + 10
			}
		}
		out = append(out, v)
		i += 2
	}
	return out
}

func isHex(c byte) bool {
	return ('0' <= c && c <= '9') || ('a' <= c && c <= 'f') || ('A' <= c && c <= 'F')
}

func printID(resp []byte) {
	if len(resp) < 4 {
		fmt.Println("  (response too short)")
		return
	}
	replySvc := resp[0]
	status := resp[2]
	extSize := resp[3]
	fmt.Printf("  CIP reply: service=0x%02x  general_status=0x%02x  ext_status_sz=%d\n",
		replySvc, status, extSize)
	if status != 0 {
		fmt.Println("  CIP error.")
		return
	}
	bodyStart := 4 + int(extSize)*2
	if bodyStart > len(resp) {
		fmt.Println("  (body offset past end)")
		return
	}
	body := resp[bodyStart:]
	if len(body) < 14 {
		fmt.Println("  (body too short for Identity)")
		return
	}
	vendor := uint16(body[0]) | (uint16(body[1]) << 8)
	devtype := uint16(body[2]) | (uint16(body[3]) << 8)
	prodcode := uint16(body[4]) | (uint16(body[5]) << 8)
	major := body[6]
	minor := body[7]
	serial := uint32(body[10]) | (uint32(body[11]) << 8) |
		(uint32(body[12]) << 16) | (uint32(body[13]) << 24)
	nameLen := 0
	if len(body) > 14 {
		nameLen = int(body[14])
	}
	if 15+nameLen > len(body) {
		nameLen = len(body) - 15
	}
	fmt.Printf("  Vendor=0x%04x  DevType=0x%04x  ProductCode=0x%04x  fw=%d.%d  serial=0x%08x\n",
		vendor, devtype, prodcode, major, minor, serial)
	if nameLen > 0 {
		fmt.Printf("  Name='%s'\n", string(body[15:15+nameLen]))
	}
}

func main() {
	slot := flag.Int("slot", 2, "backplane slot number (default 2 = L85)")
	connParams := flag.Int("conn-params", 0x43E8, "vendor conn params hint")
	pathHex := flag.String("path", "", "explicit EPATH bytes as hex; default = 01 <slot>")
	appHandle := flag.Int("app-handle", 1, "caller-assigned app handle")
	flag.Parse()

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	var epath []byte
	if *pathHex != "" {
		epath = parseHex(*pathHex)
	} else {
		epath = []byte{0x01, byte(*slot)}
	}
	spec := &ocxbp.ConnSpec{
		AppHandle:   uint16(*appHandle),
		Options:     0,
		EncodedPath: epath,
		PathSize:    uint16(len(epath)),
		ConnParams:  uint16(*connParams),
	}

	fmt.Printf("[connidentity] app_handle=%d  slot=%d  conn_params=0x%04x  path=",
		*appHandle, *slot, uint16(*connParams))
	for _, b := range epath {
		fmt.Printf("%02x ", b)
	}
	fmt.Printf("(%d bytes)\n", len(epath))

	connID, connSerial, oerr := client.TxRxOpen(spec)
	orc := ocxbp.ErrCode(oerr)
	fmt.Printf("[connidentity] txrx_open rc=%d (0x%x %s)  conn_id=0x%04x  serial=0x%04x\n",
		orc, uint32(orc), ocxbp.Strerror(orc), connID, connSerial)

	req := []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}
	resp := make([]byte, 256)
	got, merr := client.TxRxMsg(spec, req, resp, uint16(len(resp)))
	mrc := ocxbp.ErrCode(merr)
	fmt.Printf("[connidentity] txrx_msg rc=%d (%s)  resp_len=%d\n",
		mrc, ocxbp.Strerror(mrc), got)
	if merr == nil && got > 0 {
		fmt.Print("response: ")
		for _, b := range resp[:got] {
			fmt.Printf("%02x ", b)
		}
		fmt.Println()
		printID(resp[:got])
	}

	cerr := client.TxRxClose(spec)
	crc := ocxbp.ErrCode(cerr)
	fmt.Printf("[connidentity] txrx_close rc=%d (%s)\n", crc, ocxbp.Strerror(crc))
}
