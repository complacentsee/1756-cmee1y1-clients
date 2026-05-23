// dummy — v0.10.3 OCXcip_Dummy liveness-probe validator.
// Mirrors c/examples/dummy.c + python/examples/dummy.py.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"
	"strconv"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
	n := 100
	if len(os.Args) >= 2 {
		if v, err := strconv.Atoi(os.Args[1]); err == nil && v > 0 {
			n = v
		}
	}

	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[dummy] open failed")
		os.Exit(2)
	}
	defer c.Close()

	fail := 0
	t0 := time.Now()
	for i := 0; i < n; i++ {
		if err := c.Dummy(); err != nil {
			fmt.Fprintf(os.Stderr, "[dummy] call %d err=%v\n", i, err)
			fail++
		}
	}
	dt := time.Since(t0)
	us := float64(dt.Microseconds())
	fmt.Printf("[dummy] %d calls, %.0f us total, %.1f us/call, %d failures\n",
		n, us, us/float64(n), fail)
	if fail == 0 {
		fmt.Println("[dummy] PASS")
	} else {
		fmt.Println("[dummy] FAIL")
		os.Exit(1)
	}
}
