// tagtest — canonical smoke test. Mirrors c/examples/tagtest.c
// flow + exit codes byte-for-byte so the runprobe.py diff against
// the C version round-trips.
//
// Sequence:
//  1. ocxbp.Open
//  2. Client.OpenSession
//  3. Client.OpenTagDB("P:1,S:2")
//  4. TagDB.Build → expect symbol_count > 0
//  5. TagDB.SymbolAt(0..9) → print names + type codes
//  6. TagDB.ReadDINT("OCX_TEST") → V0
//  7. TagDB.WriteDINT("OCX_TEST", 0xDEADBEEF)
//  8. TagDB.ReadDINT("OCX_TEST") → expect 0xDEADBEEF
//  9. TagDB.WriteDINT("OCX_TEST", V0) (restore)
//  10. TagDB.ReadDINT("OCX_TEST") → expect V0
//
// Exit 0 on PASS, 1 on FAIL, 2 on FATAL.
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

func main() {
	path := flag.String("path", "P:1,S:2", "OldI CIP path to the tag DB")
	tag := flag.String("tag", "OCX_TEST", "scalar DINT tag for the round-trip test")
	dumpN := flag.Int("dump", 10, "number of leading symbols to dump")
	noWrite := flag.Bool("no-write", false, "skip the write/restore steps")
	flag.Parse()

	tStart := time.Now()

	fmt.Println("[tagtest] opening wrapper IPC")
	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[tagtest] FATAL Open: %v\n", err)
		os.Exit(2)
	}
	defer client.Close()
	tA := time.Now()
	fmt.Printf("[tagtest]   ipc ready  dt=%.2fms\n", ms(tStart, tA))

	session, err := client.OpenSession()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[tagtest] FATAL OpenSession: %v\n", err)
		os.Exit(2)
	}
	fmt.Printf("[tagtest] OCXcip_Open ok  session=0x%08x\n", session)

	tA = time.Now()
	db, err := client.OpenTagDB(*path)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[tagtest] FATAL OpenTagDB(%s): %v\n", *path, err)
		os.Exit(2)
	}
	defer db.Close()
	tB := time.Now()
	fmt.Printf("[tagtest] OpenTagDB(%q) ok  dt=%.2fms\n", *path, ms(tA, tB))

	tA = time.Now()
	nSymbols, err := db.Build()
	if err != nil {
		fmt.Fprintf(os.Stderr, "[tagtest] FATAL Build: %v\n", err)
		os.Exit(2)
	}
	tB = time.Now()
	fmt.Printf("[tagtest] Build ok  symbols=%d  dt=%.2fms\n", nSymbols, ms(tA, tB))

	fmt.Printf("[tagtest] first %d symbols:\n", *dumpN)
	limit := *dumpN
	if limit > int(nSymbols) {
		limit = int(nSymbols)
	}
	for i := 0; i < limit; i++ {
		info, err := db.SymbolAt(uint16(i))
		if err != nil {
			fmt.Printf("  [%4d] error: %v\n", i, err)
			continue
		}
		fmt.Printf("  [%4d] %-40s type=0x%04x struct=0x%04x\n",
			i, info.Name, info.DataType&0x1FFF, info.StructType)
	}

	tA = time.Now()
	v0, err := db.ReadDINT(*tag)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[tagtest] FATAL ReadDINT(%s): %v\n", *tag, err)
		os.Exit(2)
	}
	tB = time.Now()
	fmt.Printf("[tagtest] V0 = %d (0x%08x)  dt=%.2fms\n", v0, uint32(v0), ms(tA, tB))

	passedWrite := true
	passedRestore := true

	if !*noWrite {
		// 0xDEADBEEF as int32: route through a uint32 *variable*
		// so the conversion is at runtime — Go constant arithmetic
		// rejects int32(uint32(0xDEADBEEF)) because the value
		// overflows the int32 range even after the intermediate cast.
		sentinelU := uint32(0xDEADBEEF)
		sentinel := int32(sentinelU)
		tA = time.Now()
		if err := db.WriteDINT(*tag, sentinel); err != nil {
			fmt.Fprintf(os.Stderr, "[tagtest] FATAL Write 0xDEADBEEF: %v\n", err)
			os.Exit(2)
		}
		tB = time.Now()
		fmt.Printf("[tagtest] wrote 0x%08x  dt=%.2fms\n", uint32(sentinel), ms(tA, tB))

		v1, err := db.ReadDINT(*tag)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[tagtest] FATAL Read post-write: %v\n", err)
			os.Exit(2)
		}
		passedWrite = v1 == sentinel
		marker := "<-- WRITE OK"
		if !passedWrite {
			marker = "<-- WRITE DID NOT TAKE"
		}
		fmt.Printf("[tagtest] V1 = %d (0x%08x)  %s\n", v1, uint32(v1), marker)

		if err := db.WriteDINT(*tag, v0); err != nil {
			fmt.Fprintf(os.Stderr, "[tagtest] FATAL Restore: %v\n", err)
			os.Exit(2)
		}

		v2, err := db.ReadDINT(*tag)
		if err != nil {
			fmt.Fprintf(os.Stderr, "[tagtest] FATAL Read post-restore: %v\n", err)
			os.Exit(2)
		}
		passedRestore = v2 == v0
		marker = "<-- RESTORED OK"
		if !passedRestore {
			marker = "<-- RESTORE DID NOT TAKE"
		}
		fmt.Printf("[tagtest] V2 = %d (0x%08x)  %s\n", v2, uint32(v2), marker)
	} else {
		fmt.Println("[tagtest] --no-write  skipping write/restore")
	}

	tEnd := time.Now()
	if passedWrite && passedRestore {
		fmt.Printf("\n[tagtest] READ-WRITE-READBACK: PASS  total dt=%.2fms\n", ms(tStart, tEnd))
		os.Exit(0)
	}
	fmt.Println("\n[tagtest] READ-WRITE-READBACK: FAIL")
	os.Exit(1)
}

func ms(a, b time.Time) float64 {
	return float64(b.Sub(a).Nanoseconds()) / 1e6
}
