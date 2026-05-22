// multitagtest — v0.9.0 Phase 2 read_tags validator.
//
// Output mirrors c/examples/multitagtest.c + python/examples/multitagtest.py
// (modulo timing); cross-language gate diffs the summary lines.
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func nowMs() float64 { return float64(time.Now().UnixNano()) / 1e6 }

func printValue(name string, res ocxbp.TagReadResult) {
	if res.CIPStatus != 0 {
		fmt.Printf("[multitagtest] %s: CIP status 0x%02x (FAIL)\n", name, res.CIPStatus)
		return
	}
	switch v := res.Value.(type) {
	case bool:
		s := "FALSE"
		if v {
			s = "TRUE"
		}
		fmt.Printf("[multitagtest] %s = %s (BOOL)\n", name, s)
	case int8:
		fmt.Printf("[multitagtest] %s = %d (SINT)\n", name, v)
	case int16:
		fmt.Printf("[multitagtest] %s = %d (INT)\n", name, v)
	case int32:
		fmt.Printf("[multitagtest] %s = %d (DINT)\n", name, v)
	case int64:
		fmt.Printf("[multitagtest] %s = %d (LINT)\n", name, v)
	case uint8:
		fmt.Printf("[multitagtest] %s = %d (USINT)\n", name, v)
	case uint16:
		fmt.Printf("[multitagtest] %s = %d (UINT)\n", name, v)
	case uint32:
		fmt.Printf("[multitagtest] %s = %d (UDINT)\n", name, v)
	case uint64:
		fmt.Printf("[multitagtest] %s = %d (ULINT)\n", name, v)
	case float32:
		fmt.Printf("[multitagtest] %s = %f (REAL)\n", name, v)
	case float64:
		fmt.Printf("[multitagtest] %s = %f (LREAL)\n", name, v)
	default:
		fmt.Printf("[multitagtest] %s: unknown type %T\n", name, v)
	}
}

func main() {
	path := flag.String("path", "P:1,S:2", "OldI CIP path")
	flag.Parse()

	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[multitagtest] open failed")
		os.Exit(2)
	}
	defer c.Close()
	_, _ = c.OpenSession()

	db, err := c.OpenTagDB(*path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[multitagtest] tagdb_open: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()

	n, err := db.Build()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[multitagtest] build: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("[multitagtest] path=%s build n=%d\n", *path, n)

	// Dynamically enumerate the first scalar tags so multitagtest
	// works on any PLC.  Cap at 10 to stay inside the per-batch limit.
	const maxTags = 10
	if _, err := db.PreloadSymbols(); err != nil {
		fmt.Fprintf(os.Stderr, "[multitagtest] preload_symbols: %v\n", err)
		os.Exit(1)
	}
	var names []string
	for i := uint16(0); i < n && len(names) < maxTags; i++ {
		sym, serr := db.SymbolAt(i)
		if serr != nil {
			continue
		}
		if sym.Dim0 != 0 || sym.StructType != 0 || sym.Name == "" {
			continue
		}
		t := sym.DataType & 0x1FFF
		if t < uint16(ocxbp.TypeBool) || t > uint16(ocxbp.TypeLreal) {
			continue
		}
		names = append(names, sym.Name)
	}
	if len(names) == 0 {
		fmt.Fprintln(os.Stderr, "[multitagtest] no scalar tags found on PLC")
		os.Exit(1)
	}

	t0 := nowMs()
	results, readErr := db.ReadTags(names)
	t1 := nowMs()
	rc := 0
	if readErr != nil {
		rc = ocxbp.ErrCode(readErr)
	}
	fmt.Printf("[multitagtest] read_tags %d tags dt=%.2fms rc=%d\n",
		len(names), t1-t0, rc)

	ok, failed := 0, 0
	for _, name := range names {
		res, present := results[name]
		if !present {
			res = ocxbp.TagReadResult{CIPStatus: 0xFF}
		}
		printValue(name, res)
		if res.CIPStatus == 0 {
			ok++
		} else {
			failed++
		}
	}
	pass := failed == 0
	fmt.Printf("[multitagtest] SUMMARY ok=%d failed=%d total=%d\n", ok, failed, len(names))

	// Phase 3 round-trip: write OCX_TEST, read back, restore.
	if sym, lerr := db.LookupSymbol("OCX_TEST"); lerr == nil &&
		(sym.DataType&0x1FFF) == uint16(ocxbp.TypeDint) {
		readBack, _ := db.ReadTags([]string{"OCX_TEST"})
		var original int32
		if v, ok := readBack["OCX_TEST"].Value.(int32); ok {
			original = v
		}
		werr := db.WriteTags(map[string]interface{}{"OCX_TEST": int32(0x12345678)})
		rc := 0
		if werr != nil {
			rc = ocxbp.ErrCode(werr)
		}
		fmt.Printf("[multitagtest] write_tags OCX_TEST=0x12345678 rc=%d\n", rc)

		after, _ := db.ReadTags([]string{"OCX_TEST"})
		v32, _ := after["OCX_TEST"].Value.(int32)
		verified := v32 == int32(0x12345678)
		fmt.Printf("[multitagtest] read-after-write OCX_TEST=0x%08x verified=%s\n",
			uint32(v32), boolString(verified))

		_ = db.WriteTags(map[string]interface{}{"OCX_TEST": original})

		berr := db.WriteTags(map[string]interface{}{"OCX_TEST": float32(1.5)})
		brc := 0
		if berr != nil {
			brc = ocxbp.ErrCode(berr)
		}
		fmt.Printf("[multitagtest] type-mismatch reject rc=%d (expect -305)\n", brc)

		if !verified {
			pass = false
		}
	} else {
		fmt.Println("[multitagtest] (skipped write roundtrip — OCX_TEST not found or not DINT)")
	}

	if pass {
		fmt.Println("[multitagtest] PASS")
	} else {
		fmt.Println("[multitagtest] FAIL")
		os.Exit(1)
	}
}

func boolString(b bool) string {
	if b {
		return "YES"
	}
	return "NO"
}
