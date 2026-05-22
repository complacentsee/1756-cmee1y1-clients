// conntest — v0.7.0 connected-messaging round-trip validator.
//
// Output format is byte-identical (modulo dt= timing values) to
// c/examples/conntest.c + python/examples/conntest.py.  The
// cross-language gate is the three diffs.
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"
	"sort"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

// Identity Get_Attributes_All on class 1 instance 1.
var identityReq = []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}

func nowMs() float64 {
	return float64(time.Now().UnixNano()) / 1e6
}

func percentile(xs []float64, p float64) float64 {
	if len(xs) == 0 {
		return 0
	}
	tmp := make([]float64, len(xs))
	copy(tmp, xs)
	sort.Float64s(tmp)
	idx := p * float64(len(tmp)-1)
	lo := int(idx)
	hi := lo + 1
	if hi >= len(tmp) {
		hi = len(tmp) - 1
	}
	frac := idx - float64(lo)
	return tmp[lo]*(1-frac) + tmp[hi]*frac
}

func validateIdentity(resp []byte) bool {
	return len(resp) >= 4 && resp[0] == 0x81 && resp[2] == 0x00
}

func printIdentity(resp []byte) {
	if len(resp) < 14 {
		fmt.Println("[conntest] identity: (body too short)")
		return
	}
	off := 4 + int(resp[3])*2
	body := resp[off:]
	if len(body) < 14 {
		fmt.Println("[conntest] identity: (body too short)")
		return
	}
	vendor := uint16(body[0]) | uint16(body[1])<<8
	dev := uint16(body[2]) | uint16(body[3])<<8
	prod := uint16(body[4]) | uint16(body[5])<<8
	major := body[6]
	minor := body[7]
	nameLen := 0
	if len(body) > 14 {
		nameLen = int(body[14])
	}
	if 15+nameLen > len(body) {
		nameLen = len(body) - 15
	}
	fmt.Printf("[conntest] identity: Vendor=0x%04x DevType=0x%04x Product=0x%04x "+
		"fw=%d.%d Name='%s'\n",
		vendor, dev, prod, major, minor, string(body[15:15+nameLen]))
}

func runBench(client *ocxbp.Client, slot, n int) error {
	fmt.Printf("[conntest] benchmark: %d Identity round-trips per transport, slot=%d\n",
		n, slot)

	ucmmDt := make([]float64, n)
	class3Dt := make([]float64, n)

	// UCMM loop.
	for i := 0; i < n; i++ {
		resp := make([]byte, 256)
		msg := &ocxbp.Message{
			Slot:         uint8(slot),
			CipRequest:   identityReq,
			ReqSize:      uint16(len(identityReq)),
			TimeoutMs:    5000,
			RespData:     resp,
			RespCapacity: uint16(len(resp)),
		}
		t0 := nowMs()
		if err := client.MessageSend(msg); err != nil {
			fmt.Fprintf(os.Stderr, "[conntest] UCMM bench: req[%d] failed: %v\n", i, err)
			return err
		}
		ucmmDt[i] = nowMs() - t0
		if !validateIdentity(resp[:msg.RespLen]) {
			return fmt.Errorf("UCMM req[%d] reply invalid", i)
		}
	}

	// Class-3 loop.
	spec := &ocxbp.ConnSpec{
		AppHandle:   2,
		EncodedPath: []byte{0x01, byte(slot)},
		PathSize:    2,
	}
	if _, _, err := client.TxRxOpen(spec); err != nil {
		fmt.Fprintf(os.Stderr, "[conntest] bench: txrx_open: %v\n", err)
		return err
	}
	for i := 0; i < n; i++ {
		resp := make([]byte, 256)
		t0 := nowMs()
		got, err := client.TxRxMsg(spec, identityReq, resp, uint16(len(resp)))
		class3Dt[i] = nowMs() - t0
		if err != nil {
			_ = client.TxRxClose(spec)
			return fmt.Errorf("Class3 req[%d]: %w", i, err)
		}
		if !validateIdentity(resp[:got]) {
			_ = client.TxRxClose(spec)
			return fmt.Errorf("Class3 req[%d] reply invalid", i)
		}
	}
	_ = client.TxRxClose(spec)

	fmt.Printf("[conntest]   UCMM     median dt=%.2fms  p95 dt=%.2fms\n",
		percentile(ucmmDt, 0.50), percentile(ucmmDt, 0.95))
	fmt.Printf("[conntest]   Class3   median dt=%.2fms  p95 dt=%.2fms\n",
		percentile(class3Dt, 0.50), percentile(class3Dt, 0.95))
	return nil
}

func main() {
	slot := flag.Int("slot", 2, "backplane slot number (default 2 = L85)")
	n := flag.Int("n", 10, "number of Identity round-trips on the connection")
	bench := flag.Bool("bench", false, "run UCMM vs Class3 latency micro-benchmark (100 each)")
	flag.Parse()

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "[conntest] open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	spec := &ocxbp.ConnSpec{
		AppHandle:   1,
		EncodedPath: []byte{0x01, byte(*slot)},
		PathSize:    2,
	}

	fmt.Printf("[conntest] slot=%d N=%d app_handle=%d\n", *slot, *n, spec.AppHandle)

	connID, connSerial, err := client.TxRxOpen(spec)
	if err != nil {
		fmt.Fprintf(os.Stderr, "[conntest] txrx_open: %v\n", err)
		os.Exit(1)
	}
	fmt.Printf("[conntest] txrx_open  conn_id=0x%04x  serial=0x%04x\n", connID, connSerial)

	dts := make([]float64, *n)
	var lastResp []byte
	success := 0
	for i := 0; i < *n; i++ {
		resp := make([]byte, 256)
		t0 := nowMs()
		got, merr := client.TxRxMsg(spec, identityReq, resp, uint16(len(resp)))
		dts[i] = nowMs() - t0
		status := byte(0xFF)
		vendor := uint16(0xFFFF)
		if got >= 3 {
			status = resp[2]
		}
		if validateIdentity(resp[:got]) && int(resp[3])*2+4+6 <= int(got) {
			off := 4 + int(resp[3])*2
			vendor = uint16(resp[off]) | uint16(resp[off+1])<<8
		}
		fmt.Printf("[conntest] req[%d] dt=%.2fms status=0x%02x vendor=0x%04x\n",
			i, dts[i], status, vendor)
		if merr == nil && validateIdentity(resp[:got]) {
			success++
			lastResp = make([]byte, got)
			copy(lastResp, resp[:got])
		}
	}

	if lastResp != nil {
		printIdentity(lastResp)
	}

	cerr := client.TxRxClose(spec)
	closeOk := cerr == nil
	if closeOk {
		fmt.Println("[conntest] txrx_close ok")
	} else {
		fmt.Println("[conntest] txrx_close FAIL")
	}

	fmt.Printf("[conntest] SUMMARY %d/%d success  median dt=%.2fms\n",
		success, *n, percentile(dts, 0.50))
	pass := success == *n && closeOk
	if pass {
		fmt.Println("[conntest] PASS")
	} else {
		fmt.Println("[conntest] FAIL")
	}

	benchErr := error(nil)
	if *bench {
		benchErr = runBench(client, *slot, 100)
	}

	if !pass || benchErr != nil {
		os.Exit(1)
	}
}
