// actnodes — dump the OCXcip_GetActiveNodeTable bitmap.
// Mirrors c/examples/actnodes.c output format.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
	client, err := ocxbp.Open()
	if err != nil {
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	lo, hi, err := client.GetActiveNodes()
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("[actnodes] rc=%d (%s)\n", rc, ocxbp.Strerror(rc))
		os.Exit(1)
	}
	fmt.Printf("[actnodes] active_lo = 0x%08x  active_hi = 0x%08x\n", lo, hi)
	fmt.Print("active slots:")
	count := 0
	for i := uint32(0); i < 32; i++ {
		if lo&(1<<i) != 0 {
			fmt.Printf(" %d", i)
			count++
		}
	}
	for i := uint32(0); i < 32; i++ {
		if hi&(1<<i) != 0 {
			fmt.Printf(" %d", i+32)
			count++
		}
	}
	fmt.Printf("  (%d total)\n", count)
}
