// wctime — v0.10.0 Phase E live validator.  Mirrors c/examples/wctime.c.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func printWC(label, path string, wc ocxbp.WCTime, err error) bool {
	if err != nil {
		fmt.Printf("[wctime] %s %s: rc=%d\n", label, path, ocxbp.ErrCode(err))
		return false
	}
	t := time.Unix(int64(wc.Sec), int64(wc.Nsec)).UTC()
	fmt.Printf("[wctime] %s %s: sec=%d nsec=%d -> %s aux=(0x%x,0x%x,0x%x,0x%x)\n",
		label, path, wc.Sec, wc.Nsec, t.Format("2006-01-02T15:04:05"),
		wc.Aux0, wc.Aux1, wc.Aux2, wc.Aux3)
	return true
}

func main() {
	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[wctime] open failed")
		os.Exit(2)
	}
	defer c.Close()
	_, _ = c.OpenSession()

	anyOK := false
	for s := 1; s <= 3; s++ {
		path := fmt.Sprintf("P:1,S:%d", s)
		local, errL := c.GetWCTime(path, 1)
		utc, errU := c.GetWCTimeUTC(path, 1)
		anyOK = printWC("LOCAL", path, local, errL) || anyOK
		anyOK = printWC("UTC  ", path, utc, errU) || anyOK
	}
	if anyOK {
		fmt.Println("[wctime] PASS")
	} else {
		fmt.Println("[wctime] FAIL")
		os.Exit(1)
	}
}
