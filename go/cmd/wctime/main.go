// wctime — v0.10.0 Phase E live validator.  Mirrors c/examples/wctime.c.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func epochFor(slot int, isUTC bool) ocxbp.WCTimeEpoch {
	if slot == 2 {
		if isUTC {
			return ocxbp.WCTimeEpochUnix
		}
		return ocxbp.WCTimeEpoch1972
	}
	if isUTC {
		return ocxbp.WCTimeEpoch1998
	}
	return ocxbp.WCTimeEpoch2000
}

func printWC(label, path string, wc ocxbp.WCTime, err error,
	epoch ocxbp.WCTimeEpoch, tryTZ bool) bool {
	if err != nil {
		fmt.Printf("[wctime] %s %s: rc=%d\n", label, path, ocxbp.ErrCode(err))
		return false
	}
	t := wc.ToTime(epoch)
	tz := ""
	if tryTZ {
		tz = wc.TZName()
	}
	fmt.Printf("[wctime] %s %s: %s UTC  tz=%q\n",
		label, path, t.Format("2006-01-02T15:04:05"), tz)
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
		anyOK = printWC("LOCAL", path, local, errL, epochFor(s, false), false) || anyOK
		anyOK = printWC("UTC  ", path, utc, errU, epochFor(s, true), true) || anyOK
	}
	if anyOK {
		fmt.Println("[wctime] PASS")
	} else {
		fmt.Println("[wctime] FAIL")
		os.Exit(1)
	}
}
