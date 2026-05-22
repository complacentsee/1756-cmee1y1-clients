// pooltest — v0.8.0 connection-pool validator.
//
// Output format mirrors c/examples/pooltest.c (modulo dt= timings and
// thread interleave order); the cross-language gate is the diffs of
// the summary line across C, Go, and Python.
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

var identityReq = []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}

func nowMs() float64 {
	return float64(time.Now().UnixNano()) / 1e6
}

func validateIdentity(resp []byte) bool {
	return len(resp) >= 4 && resp[0] == 0x81 && resp[2] == 0x00
}

func main() {
	slot := flag.Int("slot", 2, "backplane slot")
	size := flag.Int("size", 4, "pool size")
	workers := flag.Int("workers", 8, "concurrent workers")
	requests := flag.Int("requests", 25, "requests per worker")
	keepaliveMs := flag.Int("keepalive-ms", 10000, "keepalive interval (ms); 0 disables")
	keepaliveTest := flag.Bool("keepalive-test", false, "sleep 30s after fanout so keepalive can fire")
	batch := flag.Bool("batch", false, "use PoolBatch (Phase 4) instead of manual goroutine fanout")
	flag.Parse()

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[pooltest] open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	fmt.Printf("[pooltest] slot=%d size=%d workers=%d requests/worker=%d keepalive_ms=%d\n",
		*slot, *size, *workers, *requests, *keepaliveMs)

	spec := &ocxbp.PoolSpec{
		Slot:        uint8(*slot),
		Size:        uint8(*size),
		KeepaliveMs: uint16(*keepaliveMs),
	}
	tOpen0 := nowMs()
	if err := client.PoolOpen(spec); err != nil {
		fmt.Fprintf(os.Stderr, "[pooltest] pool_open: %v\n", err)
		os.Exit(1)
	}
	tOpen1 := nowMs()
	fmt.Printf("[pooltest] pool_open  dt=%.2fms\n", tOpen1-tOpen0)

	var success, failed atomic.Int64
	total := int64(*workers) * int64(*requests)
	var t0, t1 float64

	if *batch {
		items := make([]ocxbp.BatchItem, total)
		for i := range items {
			items[i].Req = identityReq
		}
		t0 = nowMs()
		_ = client.PoolBatch(uint8(*slot), items, 256)
		t1 = nowMs()
		for i := range items {
			if items[i].Err == nil && validateIdentity(items[i].Resp) {
				success.Add(1)
			} else {
				failed.Add(1)
				fmt.Fprintf(os.Stderr, "[pooltest] batch item[%d] err=%v\n", i, items[i].Err)
			}
		}
	} else {
		var wg sync.WaitGroup
		t0 = nowMs()
		for w := 0; w < *workers; w++ {
			wg.Add(1)
			go func(wid int) {
				defer wg.Done()
				resp := make([]byte, 256)
				for i := 0; i < *requests; i++ {
					got, err := client.PoolTxRx(uint8(*slot), identityReq, resp, uint16(len(resp)))
					if err == nil && validateIdentity(resp[:got]) {
						success.Add(1)
					} else {
						failed.Add(1)
						fmt.Fprintf(os.Stderr, "[pooltest] worker=%d req[%d] err=%v\n", wid, i, err)
					}
				}
			}(w)
		}
		wg.Wait()
		t1 = nowMs()
	}

	mode := "fanout"
	if *batch {
		mode = "batch "
	}
	fmt.Printf("[pooltest] %s %d workers × %d req = %d total  dt=%.2fms  (%.0f req/s)  success=%d failed=%d\n",
		mode, *workers, *requests, total, t1-t0, float64(total)/((t1-t0)/1000.0),
		success.Load(), failed.Load())

	if *keepaliveTest {
		fmt.Println("[pooltest] keepalive test: sleeping 30 s — watch stderr for ping lines")
		time.Sleep(30 * time.Second)
	}

	tClose0 := nowMs()
	cerr := client.PoolClose(uint8(*slot))
	tClose1 := nowMs()
	rc := 0
	if cerr != nil {
		rc = ocxbp.ErrCode(cerr)
	}
	fmt.Printf("[pooltest] pool_close dt=%.2fms rc=%d\n", tClose1-tClose0, rc)

	pass := success.Load() == total && cerr == nil
	if pass {
		fmt.Println("[pooltest] PASS")
	} else {
		fmt.Println("[pooltest] FAIL")
		os.Exit(1)
	}
}
