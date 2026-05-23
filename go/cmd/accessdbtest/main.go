// accessdbtest — v0.10.4 cross-validation of AccessTagDataDb vs
// AccessTagData. Mirrors c/examples/accessdbtest.c byte-for-byte on
// output format so runprobe.py diffs are meaningful.
//
// SPDX-License-Identifier: MIT
package main

import (
	"encoding/binary"
	"flag"
	"fmt"
	"os"
	"sort"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func msSince(a, b time.Time) float64 {
	return float64(b.Sub(a).Nanoseconds()) / 1e6
}

// readDINT issues one DINT read through either path; returns the
// CIP general status, the read value, and the slot-level rc.
func readDINT(db *ocxbp.TagDB, tag string, useDb bool) (int32, uint32, error) {
	buf := make([]byte, 4)
	r := []ocxbp.TagRequest{{
		TagName:      tag,
		DataType:     ocxbp.TypeDint,
		ElemByteSize: 4,
		Action:       ocxbp.ActionRead,
		ElemCount:    1,
		Data:         buf,
	}}
	var err error
	if useDb {
		err = db.AccessDb(r)
	} else {
		err = db.Access(r)
	}
	if err != nil {
		return 0, r[0].Result, err
	}
	return int32(binary.LittleEndian.Uint32(buf)), r[0].Result, nil
}

func writeDINT(db *ocxbp.TagDB, tag string, useDb bool, v int32) (uint32, error) {
	buf := make([]byte, 4)
	binary.LittleEndian.PutUint32(buf, uint32(v))
	r := []ocxbp.TagRequest{{
		TagName:      tag,
		DataType:     ocxbp.TypeDint,
		ElemByteSize: 4,
		Action:       ocxbp.ActionWrite,
		ElemCount:    1,
		Data:         buf,
	}}
	var err error
	if useDb {
		err = db.AccessDb(r)
	} else {
		err = db.Access(r)
	}
	if err != nil {
		return r[0].Result, err
	}
	return r[0].Result, nil
}

// runBatch fires `iters` batches of `batchN` reads of `tag` and
// stores per-call wall-clock in samples. Returns the failure count
// (rc != nil, any non-zero CIP general status, or any value mismatch
// against expect).
func runBatch(db *ocxbp.TagDB, tag string, expect int32, useDb bool,
	batchN, iters int, samples []float64) int {
	reqs := make([]ocxbp.TagRequest, batchN)
	bufs := make([][]byte, batchN)
	for k := range bufs {
		bufs[k] = make([]byte, 4)
	}
	failures := 0
	for i := 0; i < iters; i++ {
		for k := 0; k < batchN; k++ {
			for j := range bufs[k] {
				bufs[k][j] = 0
			}
			reqs[k] = ocxbp.TagRequest{
				TagName:      tag,
				DataType:     ocxbp.TypeDint,
				ElemByteSize: 4,
				Action:       ocxbp.ActionRead,
				ElemCount:    1,
				Data:         bufs[k],
			}
		}
		a := time.Now()
		var err error
		if useDb {
			err = db.AccessDb(reqs)
		} else {
			err = db.Access(reqs)
		}
		samples[i] = msSince(a, time.Now())
		if err != nil {
			failures++
			continue
		}
		for k := 0; k < batchN; k++ {
			v := int32(binary.LittleEndian.Uint32(bufs[k]))
			if reqs[k].Result != 0 || v != expect {
				failures++
				break
			}
		}
	}
	return failures
}

func main() {
	path := flag.String("path", "P:1,S:2", "OldI CIP path")
	tag := flag.String("tag", "OCX_TEST", "scalar DINT tag for the round-trip test")
	iters := flag.Int("iters", 100, "iteration count for latency comparison")
	batchN := flag.Int("batch", 1, "reads per batch (1..16)")
	doWrite := flag.Bool("write", false, "round-trip DEADBEEF via NEW, read back via OLD")
	flag.Parse()

	if *iters < 1 {
		*iters = 1
	}
	if *iters > 10000 {
		*iters = 10000
	}
	if *batchN < 1 {
		*batchN = 1
	}
	if *batchN > 16 {
		*batchN = 16
	}
	writeStr := "no"
	if *doWrite {
		writeStr = "yes"
	}
	fmt.Printf("[accessdbtest] path=%s tag=%s iters=%d batch=%d write=%s\n",
		*path, *tag, *iters, *batchN, writeStr)

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[accessdbtest] FATAL Open: %v\n", err)
		os.Exit(2)
	}
	defer client.Close()
	if _, err := client.OpenSession(); err != nil {
		fmt.Fprintf(os.Stderr, "[accessdbtest] FATAL OpenSession: %v\n", err)
		os.Exit(2)
	}
	db, err := client.OpenTagDB(*path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[accessdbtest] FATAL OpenTagDB(%s): %v\n", *path, err)
		os.Exit(2)
	}
	defer db.Close()
	nSymbols, err := db.Build()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[accessdbtest] FATAL Build: %v\n", err)
		os.Exit(2)
	}
	fmt.Printf("[accessdbtest] Build ok  symbols=%d\n", nSymbols)

	// Single-shot OLD vs NEW correctness check
	vOld, cipOld, err := readDINT(db, *tag, false)
	if err != nil || cipOld != 0 {
		fmt.Fprintf(os.Stderr,
			"[accessdbtest] FATAL Read OLD path: err=%v cip=0x%08x\n", err, cipOld)
		os.Exit(2)
	}
	vNew, cipNew, err := readDINT(db, *tag, true)
	if err != nil || cipNew != 0 {
		fmt.Fprintf(os.Stderr,
			"[accessdbtest] FATAL Read NEW path: err=%v cip=0x%08x\n", err, cipNew)
		os.Exit(2)
	}
	if vOld != vNew {
		fmt.Fprintf(os.Stderr,
			"[accessdbtest] FAIL  OLD=%d (0x%08x) NEW=%d (0x%08x)\n",
			vOld, uint32(vOld), vNew, uint32(vNew))
		os.Exit(1)
	}
	fmt.Printf("[accessdbtest] correctness: OLD == NEW = %d (0x%08x)\n",
		vOld, uint32(vOld))

	// Latency comparison
	tOld := make([]float64, *iters)
	tNew := make([]float64, *iters)
	failOld := runBatch(db, *tag, vOld, false, *batchN, *iters, tOld)
	failNew := runBatch(db, *tag, vOld, true, *batchN, *iters, tNew)
	sort.Float64s(tOld)
	sort.Float64s(tNew)
	p50 := *iters / 2
	p99 := (*iters * 99) / 100
	if p99 >= *iters {
		p99 = *iters - 1
	}
	perTagOld := tOld[p50] / float64(*batchN)
	perTagNew := tNew[p50] / float64(*batchN)
	speedup := 0.0
	if perTagOld > 0 {
		speedup = (perTagOld - perTagNew) * 100.0 / perTagOld
	}
	fmt.Printf("[accessdbtest] latency over %d batches of %d reads each path:\n",
		*iters, *batchN)
	fmt.Printf("  OLD path  median=%6.3fms  p99=%6.3fms  min=%6.3fms  max=%6.3fms"+
		"  per-tag=%6.3fms\n",
		tOld[p50], tOld[p99], tOld[0], tOld[*iters-1], perTagOld)
	fmt.Printf("  NEW path  median=%6.3fms  p99=%6.3fms  min=%6.3fms  max=%6.3fms"+
		"  per-tag=%6.3fms\n",
		tNew[p50], tNew[p99], tNew[0], tNew[*iters-1], perTagNew)
	fmt.Printf("  NEW vs OLD per-tag delta: %+.2f%% (positive = NEW faster)\n",
		speedup)
	if failOld > 0 || failNew > 0 {
		fmt.Printf("[accessdbtest] FAIL  OLD fails=%d  NEW fails=%d  (of %d batches each)\n",
			failOld, failNew, *iters)
		os.Exit(1)
	}

	passedWrite, passedRestore := true, true
	if *doWrite {
		sentinelU := uint32(0xDEADBEEF)
		sentinel := int32(sentinelU)
		cip, err := writeDINT(db, *tag, true, sentinel)
		if err != nil || cip != 0 {
			fmt.Fprintf(os.Stderr,
				"[accessdbtest] FATAL Write NEW: err=%v cip=0x%08x\n", err, cip)
			os.Exit(2)
		}
		vBack, cipBack, err := readDINT(db, *tag, false)
		if err != nil || cipBack != 0 {
			fmt.Fprintf(os.Stderr,
				"[accessdbtest] FATAL Readback OLD: err=%v cip=0x%08x\n", err, cipBack)
			os.Exit(2)
		}
		passedWrite = vBack == sentinel
		marker := "<-- OK"
		if !passedWrite {
			marker = "<-- WRITE DID NOT TAKE"
		}
		fmt.Printf("[accessdbtest] NEW-write -> OLD-readback = 0x%08x  %s\n",
			uint32(vBack), marker)

		cip, err = writeDINT(db, *tag, true, vOld)
		if err != nil || cip != 0 {
			fmt.Fprintf(os.Stderr,
				"[accessdbtest] FATAL Restore NEW: err=%v cip=0x%08x\n", err, cip)
			os.Exit(2)
		}
		vBack, cipBack, err = readDINT(db, *tag, false)
		if err != nil || cipBack != 0 {
			fmt.Fprintf(os.Stderr,
				"[accessdbtest] FATAL Confirm OLD: err=%v cip=0x%08x\n", err, cipBack)
			os.Exit(2)
		}
		passedRestore = vBack == vOld
		marker = "<-- OK"
		if !passedRestore {
			marker = "<-- RESTORE DID NOT TAKE"
		}
		fmt.Printf("[accessdbtest] NEW-restore -> OLD-readback = %d  %s\n",
			vBack, marker)
	}

	if passedWrite && passedRestore {
		fmt.Println("\n[accessdbtest] PASS")
		os.Exit(0)
	}
	fmt.Println("\n[accessdbtest] FAIL")
	os.Exit(1)
}
