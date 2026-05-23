// errstr — v0.10.0 Phase A validator for Client.ErrorString.
//
// Output mirrors c/examples/errstr.c + python/examples/errstr.py;
// cross-language gate diffs per-code lines.
//
// SPDX-License-Identifier: MIT
package main

import (
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

var codes = []int32{
	0, -1, -200, -201, -300, -301, -303, -305, -311, -400,
	-101802, -103001,
	1, 3, 8, 14, 0x14, 0x15,
}

func main() {
	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[errstr] open failed")
		os.Exit(2)
	}
	defer c.Close()
	if _, err := c.OpenSession(); err != nil {
		fmt.Fprintln(os.Stderr, "[errstr] open_session failed")
		os.Exit(2)
	}

	// The engine's lookup table only contains POSITIVE engine codes;
	// negative OCX_ERR_* are wrapper-side and the engine returns
	// rc=1 ("Bad parameter") for them.  That's by design — PASS
	// just verifies the dispatch path works.
	engineHits := 0
	for _, code := range codes {
		engine, rc := c.ErrorString(code)
		local := ocxbp.Strerror(int(code))
		if rc == nil {
			fmt.Printf("[errstr] code=%-8d local=%q  engine=%q\n", code, local, engine)
			if engine != "" {
				engineHits++
			}
		} else if ocxbp.ErrCode(rc) == 1 {
			fmt.Printf("[errstr] code=%-8d local=%q  engine=<not in table>\n", code, local)
		} else {
			fmt.Printf("[errstr] code=%-8d local=%q  engine=<rc=%d>\n",
				code, local, ocxbp.ErrCode(rc))
		}
	}
	pass := engineHits >= 1
	fmt.Printf("[errstr] engine table hits: %d/%d\n", engineHits, len(codes))
	if pass {
		fmt.Println("[errstr] PASS")
	} else {
		fmt.Println("[errstr] FAIL")
		os.Exit(1)
	}
}
