// idstatus — v0.10.0 Phase C validator for Client.GetDeviceIDStatus.
//
// Output mirrors c/examples/idstatus.c + python/examples/idstatus.py.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[idstatus] open failed")
		os.Exit(2)
	}
	defer c.Close()
	_, _ = c.OpenSession()

	hits, mismatches := 0, 0
	for slot := 0; slot < 4; slot++ {
		path := fmt.Sprintf("P:1,S:%d", slot)
		statusLite, errLite := c.GetDeviceIDStatus(path, 1)
		idFull, errFull := c.GetDeviceID(path, 1)
		if errLite == nil && errFull == nil {
			match := statusLite == idFull.Status
			matchStr := "NO"
			if match {
				matchStr = "YES"
			}
			fmt.Printf("[idstatus] slot=%d  lite=0x%04x  full=0x%04x  match=%s\n",
				slot, statusLite, idFull.Status, matchStr)
			hits++
			if !match {
				mismatches++
			}
		} else {
			fmt.Printf("[idstatus] slot=%d  lite_rc=%d full_rc=%d  (empty or error)\n",
				slot, ocxbp.ErrCode(errLite), ocxbp.ErrCode(errFull))
		}
	}
	pass := hits >= 1 && mismatches == 0
	fmt.Printf("[idstatus] SUMMARY hits=%d mismatches=%d\n", hits, mismatches)
	if pass {
		fmt.Println("[idstatus] PASS")
	} else {
		fmt.Println("[idstatus] FAIL")
		os.Exit(1)
	}
}
