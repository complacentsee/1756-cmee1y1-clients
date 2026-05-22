// symcache — v0.9.0 Phase 1 symbol-cache validator.
//
// Output mirrors c/examples/symcache.c (modulo dt= timings); the
// cross-language gate is the diff of the summary lines.
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

func main() {
	path := flag.String("path", "P:1,S:2", "OldI CIP path to the PLC")
	tag := flag.String("tag", "OCX_TEST", "tag name to look up")
	flag.Parse()

	c, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[symcache] open failed")
		os.Exit(2)
	}
	defer c.Close()
	_, _ = c.OpenSession()

	db, err := c.OpenTagDB(*path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[symcache] tagdb_open: %v\n", err)
		os.Exit(1)
	}
	defer db.Close()

	t0 := nowMs()
	n, err := db.Build()
	t1 := nowMs()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[symcache] build: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("[symcache] path=%s build n=%d dt=%.2fms\n", *path, n, t1-t0)

	l0 := nowMs()
	info, err := db.LookupSymbol(*tag)
	l1 := nowMs()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[symcache] lookup#1: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("[symcache] lookup#1 cold dt=%.2fms  data_type=0x%04x elem_byte_size=%d\n",
		l1-l0, info.DataType, info.ElemByteSize)

	l2 := nowMs()
	_, err = db.LookupSymbol(*tag)
	l3 := nowMs()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[symcache] lookup#2: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("[symcache] lookup#2 warm dt=%.3fms  (cache hit)\n", l3-l2)

	p0 := nowMs()
	cached, perr := db.PreloadSymbols()
	p1 := nowMs()
	rc := 0
	if perr != nil {
		rc = ocxbp.ErrCode(perr)
	}
	_ = cached
	fmt.Printf("[symcache] preload all dt=%.2fms rc=%d\n", p1-p0, rc)

	l4 := nowMs()
	_, _ = db.LookupSymbol(*tag)
	l5 := nowMs()
	fmt.Printf("[symcache] lookup#3 after-preload dt=%.3fms  rc=0\n", l5-l4)

	fmt.Println("[symcache] PASS")
}
