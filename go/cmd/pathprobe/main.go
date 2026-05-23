// pathprobe — invoke Client.ParsePath and dump the encoded EPATH
// bytes it produces. Mirrors c/examples/pathprobe.c.
//
// Useful when hand-building the route_path inside a routed
// Unconnected_Send (svc 0x52) carried in Client.MessageSend.
//
// Usage:
//
//	pathprobe "P:1,S:2"
//	pathprobe "1,2,3"      (Rockwell-style — likely rejected)
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
	if len(os.Args) < 2 {
		fmt.Fprintf(os.Stderr, "Usage: %s <text-path>\n", os.Args[0])
		os.Exit(2)
	}
	textPath := os.Args[1]

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "client open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	parsed, perr := client.ParsePath(textPath)
	rc := 0
	if perr != nil {
		rc = ocxbp.ErrCode(perr)
	}
	fmt.Printf("[pathprobe] text='%s'  rc=%d  encoded_len=%d\n",
		textPath, rc, len(parsed.Encoded))
	if rc != 0 {
		os.Exit(1)
	}

	fmt.Printf("  out_class      = 0x%04x\n", parsed.CIPClass)
	fmt.Printf("  out_seg_flags  = 0x%02x\n", parsed.SegmentFlags)
	fmt.Printf("  out_instance   = 0x%08x  (%d)\n", parsed.Instance, parsed.Instance)
	fmt.Printf("  out_attr_flags = 0x%02x\n", parsed.AttrFlags)
	fmt.Print("  encoded path   = ")
	for _, b := range parsed.Encoded {
		fmt.Printf("%02x ", b)
	}
	fmt.Println()
}
