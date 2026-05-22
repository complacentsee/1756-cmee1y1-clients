// routedident — v0.8.0 multi-hop Identity via Unconnected_Send.
//
// Output format mirrors c/examples/routedident.c (modulo hex spacing);
// the cross-language gate is the diffs of the Identity body line.
//
// SPDX-License-Identifier: MIT
package main

import (
	"encoding/hex"
	"flag"
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
)

func main() {
	routerSlot := flag.Int("router-slot", 2, "slot hosting the router (default 2 = L85)")
	port := flag.Int("port", 1, "route port: 1 = backplane, 2 = front EtherNet/IP")
	link := flag.Int("link", 2, "route link address (slot or EIP node)")
	flag.Parse()

	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[routedident] open failed")
		os.Exit(2)
	}
	defer c.Close()
	_, _ = c.OpenSession()

	embedded := []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}
	route := make([]byte, 8)
	off := 0
	if !cip.AppendPortSegment(route, &off, uint8(*port), uint8(*link)) {
		fmt.Fprintln(os.Stderr, "[routedident] route encoding failed")
		os.Exit(1)
	}
	route = route[:off]

	wrapped := make([]byte, 256)
	wlen := cip.BuildUnconnectedSend(wrapped, embedded, route, 5000)
	if wlen <= 0 {
		fmt.Fprintf(os.Stderr, "[routedident] BuildUnconnectedSend rc=%d\n", wlen)
		os.Exit(1)
	}
	wrapped = wrapped[:wlen]

	fmt.Printf("[routedident] router_slot=%d  port=%d  link=%d  wrapped_len=%d\n",
		*routerSlot, *port, *link, wlen)
	fmt.Printf("[routedident] wrapped bytes: %s\n",
		hexspaced(wrapped))

	resp := make([]byte, 256)
	msg := &ocxbp.Message{
		Slot:         uint8(*routerSlot),
		CipRequest:   wrapped,
		ReqSize:      uint16(len(wrapped)),
		TimeoutMs:    5000,
		RespData:     resp,
		RespCapacity: uint16(len(resp)),
	}
	if err := c.MessageSend(msg); err != nil {
		fmt.Fprintf(os.Stderr, "[routedident] message_send: %v\n", err)
		os.Exit(1)
	}
	out := resp[:msg.RespLen]
	fmt.Printf("[routedident] resp_len=%d  bytes: %s\n", msg.RespLen, hexspaced(out))

	if len(out) < 4 || out[0] != 0x81 {
		fmt.Fprintf(os.Stderr, "[routedident] reply not Identity GAA (svc=0x%02x status=0x%02x)\n",
			firstByte(out), thirdByte(out))
		os.Exit(1)
	}
	if out[2] != 0x00 {
		ext := uint16(0)
		if len(out) >= 6 && out[3] != 0 {
			ext = uint16(out[4]) | (uint16(out[5]) << 8)
		}
		fmt.Fprintf(os.Stderr, "[routedident] routed CIP failure: status=0x%02x ext=0x%04x (%s)\n",
			out[2], ext, ocxbp.CIPStatusString(out[2], ext))
		os.Exit(1)
	}
	bodyOff := 4 + int(out[3])*2
	if len(out) < bodyOff+14 {
		fmt.Fprintln(os.Stderr, "[routedident] body too short")
		os.Exit(1)
	}
	body := out[bodyOff:]
	vendor := uint16(body[0]) | uint16(body[1])<<8
	dev := uint16(body[2]) | uint16(body[3])<<8
	prod := uint16(body[4]) | uint16(body[5])<<8
	major := body[6]
	minor := body[7]
	nameLen := 0
	if len(body) > 14 {
		nameLen = int(body[14])
	}
	if 15+nameLen > len(body) {
		nameLen = len(body) - 15
	}
	fmt.Printf("[routedident] Identity: Vendor=0x%04x DevType=0x%04x Product=0x%04x fw=%d.%d Name='%s'\n",
		vendor, dev, prod, major, minor, string(body[15:15+nameLen]))
	fmt.Println("[routedident] PASS")
}

func hexspaced(b []byte) string {
	s := hex.EncodeToString(b)
	out := make([]byte, 0, len(s)+len(b))
	for i := 0; i < len(s); i += 2 {
		if i > 0 {
			out = append(out, ' ')
		}
		out = append(out, s[i], s[i+1])
	}
	out = append(out, ' ')
	return string(out)
}

func firstByte(b []byte) byte {
	if len(b) == 0 {
		return 0
	}
	return b[0]
}
func thirdByte(b []byte) byte {
	if len(b) < 3 {
		return 0xFF
	}
	return b[2]
}
