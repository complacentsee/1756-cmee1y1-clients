// msgprobe — manual OCXcip_MessageSend invocation + raw response
// hex dump. Mirrors c/examples/msgprobe.c CLI args and output format
// byte-for-byte so runprobe.py diff against the C tooling matches.
//
// Usage:
//
//	msgprobe --slot 1 --req "01 02 20 01 24 01"
//	msgprobe --slot 2 --req "0e 03 20 01 24 01 30 01"
//	msgprobe --slot 1 --req "52 02 20 06 24 01 ..." --timeout-ms 1000
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"
	"strings"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

// parseHex turns "01 02 20 01 24 01" (with optional spaces / commas /
// punctuation) into the corresponding bytes. Mirrors parse_hex in
// c/examples/msgprobe.c.
func parseHex(s string) []byte {
	out := make([]byte, 0, len(s)/2)
	i := 0
	for i < len(s) {
		for i < len(s) && !isHex(s[i]) {
			i++
		}
		if i+1 >= len(s) {
			break
		}
		if !isHex(s[i+1]) {
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

// hexdump matches the format from c/examples/msgprobe.c::hexdump:
//
//	+%03x BB BB BB ... BB  AAAA...AAAA
func hexdump(b []byte) {
	for i := 0; i < len(b); i += 16 {
		fmt.Printf("    +%03x ", i)
		line := len(b) - i
		if line > 16 {
			line = 16
		}
		for j := 0; j < line; j++ {
			fmt.Printf("%02x ", b[i+j])
		}
		for j := line; j < 16; j++ {
			fmt.Print("   ")
		}
		fmt.Print(" ")
		for j := 0; j < line; j++ {
			ch := b[i+j]
			if ch >= 0x20 && ch < 0x7F {
				fmt.Printf("%c", ch)
			} else {
				fmt.Print(".")
			}
		}
		fmt.Println()
	}
}

func main() {
	slot := flag.Int("slot", -1, "backplane slot number (0..0x13)")
	req := flag.String("req", "", "CIP request bytes as hex, e.g. \"01 02 20 01 24 01\"")
	timeoutMs := flag.Int("timeout-ms", 0, "per-attempt timeout in ms (engine min 26)")
	flag.Parse()

	if *slot < 0 || *req == "" {
		fmt.Fprintln(os.Stderr, "Use --slot 1 --req \"01 02 20 01 24 01\" [--timeout-ms 1000]")
		os.Exit(2)
	}

	reqBytes := parseHex(*req)
	if len(reqBytes) == 0 {
		fmt.Fprintln(os.Stderr, "empty req")
		os.Exit(2)
	}

	var sb strings.Builder
	for _, b := range reqBytes {
		fmt.Fprintf(&sb, "%02x ", b)
	}
	fmt.Printf("[msgprobe] slot=%d timeout_ms=%d req=%s(%d bytes)\n\n",
		*slot, *timeoutMs, sb.String(), len(reqBytes))

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "Open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	resp := make([]byte, 4096)
	m := &ocxbp.Message{
		Slot:         uint8(*slot),
		CipRequest:   reqBytes,
		ReqSize:      uint16(len(reqBytes)),
		TimeoutMs:    uint16(*timeoutMs),
		RespData:     resp,
		RespCapacity: uint16(len(resp)),
	}
	sendErr := client.MessageSend(m)
	rc := ocxbp.ErrCode(sendErr)

	fmt.Printf("rc            = %d (0x%x)\n", rc, uint32(rc))
	fmt.Printf("response_len  = %d\n", m.RespLen)
	fmt.Printf("status field  = 0x%08x\n", m.Status)
	if m.RespLen > 0 {
		fmt.Println("response bytes:")
		n := int(m.RespLen)
		if n > 256 {
			n = 256
		}
		hexdump(resp[:n])
	}
	if rc != 0 {
		os.Exit(1)
	}
}
