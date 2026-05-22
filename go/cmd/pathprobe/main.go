// pathprobe — invoke OCXcip_ParsePath and dump the encoded EPATH
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
	"errors"
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
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

	var result cip.ParsePathResult
	callErr := client.SHM().Call(shm.CallSpec{
		FnName:      cip.FnParsePath,
		PayloadSize: cip.SizeParsePath,
		Fill:        func(slot []byte) { cip.EncodeParsePath(slot, textPath, 256) },
		Read:        func(slot []byte) { result = cip.DecodeParsePath(slot) },
		TimeoutMs:   5000,
	})
	rc := 0
	if callErr != nil {
		// shm-layer returns shm.EngineError or named sentinels; map
		// to a numeric rc for byte-identical diff with the C version.
		// Reusing ocxbp.ErrCode is fine — it walks the same shape.
		// callErr may be a raw shm error (we bypassed ocxbp's
		// translateCallErr), so map it explicitly.
		rc = errCodeFromShm(callErr)
	}

	fmt.Printf("[pathprobe] text='%s'  rc=%d  encoded_len=%d\n",
		textPath, rc, len(result.Encoded))
	if rc != 0 {
		os.Exit(1)
	}

	fmt.Printf("  out_class      = 0x%04x\n", result.Class)
	fmt.Printf("  out_seg_flags  = 0x%02x\n", result.SegFlags)
	fmt.Printf("  out_instance   = 0x%08x  (%d)\n", result.Instance, result.Instance)
	fmt.Printf("  out_attr_flags = 0x%02x\n", result.AttrFlags)
	fmt.Print("  encoded path   = ")
	for _, b := range result.Encoded {
		fmt.Printf("%02x ", b)
	}
	fmt.Println()
}

// errCodeFromShm maps a shm-layer error to an integer rc consistent
// with the C SDK's int-return convention. Used so pathprobe's output
// diffs byte-for-byte against c/examples/pathprobe.c.
func errCodeFromShm(err error) int {
	var ee shm.EngineError
	if errors.As(err, &ee) {
		return int(ee)
	}
	return ocxbp.BPErrGeneric
}
