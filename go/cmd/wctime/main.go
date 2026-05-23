// wctime — v0.10.0 Phase E live validator.  Mirrors c/examples/wctime.c.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

var gRaw bool

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
	epoch ocxbp.WCTimeEpoch, tryTZ, tryLocal bool) bool {
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
	if tryLocal {
		loc := wc.DecodeLocal()
		fmt.Printf("[wctime] %s %s: aux2-decoded local d=%d h=%d m=%d s=%d\n",
			label, path, loc.Day, loc.Hour, loc.Minute, loc.Second)
	}
	if gRaw {
		fmt.Printf("[wctime] %s %s: raw sec=0x%016x nsec=0x%016x\n",
			label, path, wc.Sec, wc.Nsec)
		fmt.Printf("[wctime] %s %s: raw aux0=0x%016x aux1=0x%016x\n",
			label, path, wc.Aux0, wc.Aux1)
		fmt.Printf("[wctime] %s %s: raw aux2=0x%016x aux3=0x%016x\n",
			label, path, wc.Aux2, wc.Aux3)
	}
	return true
}

func main() {
	for _, a := range os.Args[1:] {
		if a == "--raw" {
			gRaw = true
		}
	}
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
		anyOK = printWC("LOCAL", path, local, errL, epochFor(s, false), false, true) || anyOK
		anyOK = printWC("UTC  ", path, utc, errU, epochFor(s, true), true, false) || anyOK
	}
	if anyOK {
		fmt.Println("[wctime] PASS")
	} else {
		fmt.Println("[wctime] FAIL")
		os.Exit(1)
	}
}
