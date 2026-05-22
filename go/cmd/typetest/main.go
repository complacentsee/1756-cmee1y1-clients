// typetest — exercises every type helper against a configurable
// mapping of test tags.  Mirrors c/examples/typetest.c flag-for-flag
// and output format byte-for-byte so runprobe.py diff against the C
// and Python versions round-trips.
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"math"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

var passCount, failCount int

func assertEq(expected, actual any, fmtStr string) {
	if expected == actual {
		fmt.Println("    ok")
		passCount++
		return
	}
	fmt.Printf("    FAIL: expected "+fmtStr+", got "+fmtStr+"\n", expected, actual)
	failCount++
}

func assertFEq(expected, actual, eps float64) {
	d := math.Abs(expected - actual)
	if d <= eps {
		fmt.Println("    ok")
		passCount++
		return
	}
	fmt.Printf("    FAIL: expected %.9g, got %.9g (delta %.9g)\n", expected, actual, d)
	failCount++
}

func maybeErr(err error, what string) bool {
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("    FAIL: %s -> %s (%d)\n", what, ocxbp.Strerror(rc), rc)
		failCount++
		return true
	}
	return false
}

func testSINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[sint scalar] tag=%s\n", tag)
	v0, err := db.ReadSINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = %d\n", v0)
	probe := int8(42)
	if maybeErr(db.WriteSINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadSINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = %d (probe=%d)\n", v1, probe)
	assertEq(probe, v1, "%d")
	if maybeErr(db.WriteSINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadSINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = %d (restore=%d)\n", v2, v0)
	assertEq(v0, v2, "%d")
}

func testINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[int scalar] tag=%s\n", tag)
	v0, err := db.ReadINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = %d\n", v0)
	probe := int16(12345)
	if maybeErr(db.WriteINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = %d (probe=%d)\n", v1, probe)
	assertEq(probe, v1, "%d")
	if maybeErr(db.WriteINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = %d (restore=%d)\n", v2, v0)
	assertEq(v0, v2, "%d")
}

func testDINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[dint scalar] tag=%s\n", tag)
	v0, err := db.ReadDINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = 0x%08x\n", uint32(v0))
	probeU := uint32(0xDEADBEEF)
	probe := int32(probeU)
	if maybeErr(db.WriteDINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadDINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = 0x%08x (probe=0x%08x)\n", uint32(v1), uint32(probe))
	assertEq(uint32(probe), uint32(v1), "0x%08x")
	if maybeErr(db.WriteDINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadDINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = 0x%08x (restore=0x%08x)\n", uint32(v2), uint32(v0))
	assertEq(uint32(v0), uint32(v2), "0x%08x")
}

func testLINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[lint scalar] tag=%s\n", tag)
	v0, err := db.ReadLINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = 0x%016x\n", uint64(v0))
	probe := int64(0x0123456789ABCDEF)
	if maybeErr(db.WriteLINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadLINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = 0x%016x (probe=0x%016x)\n", uint64(v1), uint64(probe))
	assertEq(uint64(probe), uint64(v1), "0x%016x")
	if maybeErr(db.WriteLINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadLINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = 0x%016x (restore=0x%016x)\n", uint64(v2), uint64(v0))
	assertEq(uint64(v0), uint64(v2), "0x%016x")
}

func testUSINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[usint scalar] tag=%s\n", tag)
	v0, err := db.ReadUSINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = %d\n", v0)
	probe := uint8(0xAB)
	if maybeErr(db.WriteUSINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadUSINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = %d (probe=%d)\n", v1, probe)
	assertEq(probe, v1, "%d")
	if maybeErr(db.WriteUSINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadUSINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = %d (restore=%d)\n", v2, v0)
	assertEq(v0, v2, "%d")
}

func testUINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[uint scalar] tag=%s\n", tag)
	v0, err := db.ReadUINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = 0x%04x\n", v0)
	probe := uint16(0xCAFE)
	if maybeErr(db.WriteUINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadUINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = 0x%04x (probe=0x%04x)\n", v1, probe)
	assertEq(probe, v1, "0x%04x")
	if maybeErr(db.WriteUINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadUINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = 0x%04x (restore=0x%04x)\n", v2, v0)
	assertEq(v0, v2, "0x%04x")
}

func testUDINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[udint scalar] tag=%s\n", tag)
	v0, err := db.ReadUDINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = 0x%08x\n", v0)
	probe := uint32(0xFEEDFACE)
	if maybeErr(db.WriteUDINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadUDINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = 0x%08x (probe=0x%08x)\n", v1, probe)
	assertEq(probe, v1, "0x%08x")
	if maybeErr(db.WriteUDINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadUDINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = 0x%08x (restore=0x%08x)\n", v2, v0)
	assertEq(v0, v2, "0x%08x")
}

func testULINT(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[ulint scalar] tag=%s\n", tag)
	v0, err := db.ReadULINT(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = 0x%016x\n", v0)
	probe := uint64(0xCAFEBABEDEADBEEF)
	if maybeErr(db.WriteULINT(tag, probe), "write probe") {
		return
	}
	v1, err := db.ReadULINT(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = 0x%016x (probe=0x%016x)\n", v1, probe)
	assertEq(probe, v1, "0x%016x")
	if maybeErr(db.WriteULINT(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadULINT(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	fmt.Printf("  V2 = 0x%016x (restore=0x%016x)\n", v2, v0)
	assertEq(v0, v2, "0x%016x")
}

func testREAL(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[real scalar] tag=%s\n", tag)
	v0, err := db.ReadREAL(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = %.6g\n", float64(v0))
	probe := float32(3.14159)
	if maybeErr(db.WriteREAL(tag, probe), "write") {
		return
	}
	v1, err := db.ReadREAL(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = %.6g (probe=%.6g)\n", float64(v1), float64(probe))
	assertFEq(float64(probe), float64(v1), 1e-5)
	if maybeErr(db.WriteREAL(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadREAL(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	assertFEq(float64(v0), float64(v2), 1e-5)
}

func testLREAL(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[lreal scalar] tag=%s\n", tag)
	v0, err := db.ReadLREAL(tag)
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 = %.15g\n", v0)
	probe := 2.71828182845904523
	if maybeErr(db.WriteLREAL(tag, probe), "write") {
		return
	}
	v1, err := db.ReadLREAL(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = %.15g (probe=%.15g)\n", v1, probe)
	assertFEq(probe, v1, 1e-12)
	if maybeErr(db.WriteLREAL(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadLREAL(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	assertFEq(v0, v2, 1e-12)
}

func testBOOL(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[bool scalar] tag=%s\n", tag)
	v0, err := db.ReadBOOL(tag)
	if maybeErr(err, "initial read") {
		return
	}
	v0i := 0
	if v0 {
		v0i = 1
	}
	fmt.Printf("  V0 = %d\n", v0i)
	probe := !v0
	probeI := 0
	if probe {
		probeI = 1
	}
	if maybeErr(db.WriteBOOL(tag, probe), "write") {
		return
	}
	v1, err := db.ReadBOOL(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	v1i := 0
	if v1 {
		v1i = 1
	}
	fmt.Printf("  V1 = %d (probe=%d)\n", v1i, probeI)
	assertEq(probeI, v1i, "%d")
	if maybeErr(db.WriteBOOL(tag, v0), "restore") {
		return
	}
	v2, err := db.ReadBOOL(tag)
	if maybeErr(err, "post-restore read") {
		return
	}
	v2i := 0
	if v2 {
		v2i = 1
	}
	assertEq(v0i, v2i, "%d")
}

func testString(db *ocxbp.TagDB, tag string) {
	if tag == "" {
		return
	}
	fmt.Printf("\n[string] tag=%s\n", tag)
	v0, err := db.ReadString(tag)
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("    FAIL: initial read -> %s (%d)\n", ocxbp.Strerror(rc), rc)
		failCount++
		return
	}
	fmt.Printf("  V0 = '%s' (len=%d)\n", v0, len(v0))
	probe := "hello from bpclient typetest"
	if maybeErr(db.WriteString(tag, probe), "write") {
		return
	}
	v1, err := db.ReadString(tag)
	if maybeErr(err, "post-write read") {
		return
	}
	fmt.Printf("  V1 = '%s' (len=%d)\n", v1, len(v1))
	if v1 == probe {
		fmt.Println("    ok")
		passCount++
	} else {
		fmt.Println("    FAIL: string mismatch")
		failCount++
	}
	maybeErr(db.WriteString(tag, v0), "restore")
}

func testBoolArray(db *ocxbp.TagDB, tag string, count int) {
	if tag == "" || count <= 0 {
		return
	}
	fmt.Printf("\n[bool array] tag=%s count=%d\n", tag, count)
	v0, err := db.ReadBOOLArray(tag, uint16(count))
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("    FAIL: initial read -> %s (%d)\n", ocxbp.Strerror(rc), rc)
		failCount++
		return
	}
	top := count
	if top > 16 {
		top = 16
	}
	fmt.Printf("  V0[0..%d] =", top-1)
	for i := 0; i < top; i++ {
		v := 0
		if v0[i] {
			v = 1
		}
		fmt.Printf(" %d", v)
	}
	if count > 16 {
		fmt.Println(" ...")
	} else {
		fmt.Println()
	}

	probe := make([]bool, count)
	for i := 0; i < count; i++ {
		probe[i] = (i & 1) == 0
	}
	if err := db.WriteBOOLArray(tag, probe); err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("    FAIL: write -> %s (%d)\n", ocxbp.Strerror(rc), rc)
		failCount++
		return
	}
	v1, err := db.ReadBOOLArray(tag, uint16(count))
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("    FAIL: readback -> %s (%d)\n", ocxbp.Strerror(rc), rc)
		failCount++
		return
	}
	allMatch := true
	for i := 0; i < count; i++ {
		if v1[i] != probe[i] {
			expI, gotI := 0, 0
			if probe[i] {
				expI = 1
			}
			if v1[i] {
				gotI = 1
			}
			fmt.Printf("    MISMATCH at bit[%d]: expected %d, got %d\n", i, expI, gotI)
			allMatch = false
		}
	}
	if allMatch {
		fmt.Printf("    ok (all %d bits match)\n", count)
		passCount++
	} else {
		failCount++
	}
	_ = db.WriteBOOLArray(tag, v0)
}

func testDintArray(db *ocxbp.TagDB, tag string, count int) {
	if tag == "" || count <= 0 {
		return
	}
	fmt.Printf("\n[dint array] tag=%s count=%d\n", tag, count)
	v0, err := db.ReadDINTArray(tag, uint16(count))
	if maybeErr(err, "initial read") {
		return
	}
	top := count
	if top > 8 {
		top = 8
	}
	fmt.Printf("  V0[0..%d] =", top-1)
	for i := 0; i < top; i++ {
		fmt.Printf(" %d", v0[i])
	}
	if count > 8 {
		fmt.Println(" ...")
	} else {
		fmt.Println()
	}

	probe := make([]int32, count)
	for i := 0; i < count; i++ {
		probe[i] = int32(0x1000 + i)
	}
	if maybeErr(db.WriteDINTArray(tag, probe), "write") {
		return
	}
	v1, err := db.ReadDINTArray(tag, uint16(count))
	if maybeErr(err, "post-write read") {
		return
	}
	allMatch := true
	for i := 0; i < count; i++ {
		if v1[i] != probe[i] {
			fmt.Printf("    MISMATCH at [%d]: expected %d, got %d\n", i, probe[i], v1[i])
			allMatch = false
		}
	}
	if allMatch {
		fmt.Printf("    ok (all %d elements match)\n", count)
		passCount++
	} else {
		failCount++
	}
	maybeErr(db.WriteDINTArray(tag, v0), "restore")
}

func testDint2D(db *ocxbp.TagDB, tag string, dim0, dim1 int) {
	if tag == "" || dim0 <= 0 || dim1 <= 0 {
		return
	}
	total := dim0 * dim1
	fmt.Printf("\n[dint 2-D] tag=%s dims=%d,%d (total=%d)\n", tag, dim0, dim1, total)
	zeroIdx := fmt.Sprintf("%s[0,0]", tag)
	v0, err := db.ReadDINTArray(zeroIdx, uint16(total))
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 (first 6 = first 2 rows): %d %d %d %d %d %d\n",
		v0[0], v0[1], v0[2], v0[3], v0[4], v0[5])

	probe := make([]int32, total)
	for r := 0; r < dim0; r++ {
		for c := 0; c < dim1; c++ {
			probe[r*dim1+c] = int32(1000*r + c)
		}
	}
	if maybeErr(db.WriteDINTArray(zeroIdx, probe), "write") {
		return
	}
	v1, err := db.ReadDINTArray(zeroIdx, uint16(total))
	if maybeErr(err, "post-write read") {
		return
	}
	allMatch := true
	for i := 0; i < total; i++ {
		if v1[i] != probe[i] {
			allMatch = false
			break
		}
	}
	if allMatch {
		fmt.Printf("    ok (batched readback row-major matches all %d)\n", total)
		passCount++
	} else {
		fmt.Println("    FAIL: batched readback mismatch")
		failCount++
	}

	mr, mc := dim0/2, dim1/2
	idx := fmt.Sprintf("%s[%d,%d]", tag, mr, mc)
	spot, err := db.ReadDINT(idx)
	if maybeErr(err, "indexed read") {
		return
	}
	expect := int32(1000*mr + mc)
	okStr := "FAIL"
	if spot == expect {
		okStr = "ok"
		passCount++
	} else {
		failCount++
	}
	fmt.Printf("  %s = %d (expect %d) %s\n", idx, spot, expect, okStr)

	_ = db.WriteDINTArray(zeroIdx, v0)
}

func testDint3D(db *ocxbp.TagDB, tag string, dim0, dim1, dim2 int) {
	if tag == "" || dim0 <= 0 || dim1 <= 0 || dim2 <= 0 {
		return
	}
	total := dim0 * dim1 * dim2
	fmt.Printf("\n[dint 3-D] tag=%s dims=%d,%d,%d (total=%d)\n", tag, dim0, dim1, dim2, total)
	zeroIdx := fmt.Sprintf("%s[0,0,0]", tag)
	v0, err := db.ReadDINTArray(zeroIdx, uint16(total))
	if maybeErr(err, "initial read") {
		return
	}
	fmt.Printf("  V0 (first 6 = first plane row): %d %d %d %d %d %d\n",
		v0[0], v0[1], v0[2], v0[3], v0[4], v0[5])

	probe := make([]int32, total)
	for i := 0; i < dim0; i++ {
		for j := 0; j < dim1; j++ {
			for k := 0; k < dim2; k++ {
				lin := i*(dim1*dim2) + j*dim2 + k
				probe[lin] = int32(100000*i + 1000*j + k)
			}
		}
	}
	if maybeErr(db.WriteDINTArray(zeroIdx, probe), "write") {
		return
	}
	v1, err := db.ReadDINTArray(zeroIdx, uint16(total))
	if maybeErr(err, "post-write read") {
		return
	}
	allMatch := true
	for i := 0; i < total; i++ {
		if v1[i] != probe[i] {
			allMatch = false
			break
		}
	}
	if allMatch {
		fmt.Printf("    ok (batched readback row-major matches all %d)\n", total)
		passCount++
	} else {
		fmt.Println("    FAIL: batched readback mismatch")
		failCount++
	}

	mi, mj, mk := dim0/2, dim1/2, dim2/2
	idx := fmt.Sprintf("%s[%d,%d,%d]", tag, mi, mj, mk)
	spot, err := db.ReadDINT(idx)
	if maybeErr(err, "indexed read") {
		return
	}
	expect := int32(100000*mi + 1000*mj + mk)
	okStr := "FAIL"
	if spot == expect {
		okStr = "ok"
		passCount++
	} else {
		failCount++
	}
	fmt.Printf("  %s = %d (expect %d) %s\n", idx, spot, expect, okStr)

	_ = db.WriteDINTArray(zeroIdx, v0)
}

func main() {
	path := flag.String("path", "P:1,S:1", "OldI CIP path")
	tagBool := flag.String("bool", "", "BOOL scalar tag")
	tagSint := flag.String("sint", "", "SINT scalar tag")
	tagInt := flag.String("int", "", "INT scalar tag")
	tagDint := flag.String("dint", "", "DINT scalar tag")
	tagLint := flag.String("lint", "", "LINT scalar tag")
	tagUsint := flag.String("usint", "", "USINT scalar tag")
	tagUint := flag.String("uint", "", "UINT scalar tag")
	tagUdint := flag.String("udint", "", "UDINT scalar tag")
	tagUlint := flag.String("ulint", "", "ULINT scalar tag")
	tagReal := flag.String("real", "", "REAL scalar tag")
	tagLreal := flag.String("lreal", "", "LREAL scalar tag")
	tagString := flag.String("string", "", "STRING tag")
	tagDintArr := flag.String("dint-array", "", "DINT[] tag")
	arrCount := flag.Int("array-count", 0, "")
	tagBoolArr := flag.String("bool-array", "", "BOOL[] tag")
	boolArrCount := flag.Int("bool-array-count", 0, "")
	tagDint2D := flag.String("dint-2d", "", "DINT[N,M] tag")
	dim0 := flag.Int("dint-2d-dim0", 0, "")
	dim1 := flag.Int("dint-2d-dim1", 0, "")
	tagDint3D := flag.String("dint-3d", "", "DINT[N,M,K] tag")
	d3d0 := flag.Int("dint-3d-dim0", 0, "")
	d3d1 := flag.Int("dint-3d-dim1", 0, "")
	d3d2 := flag.Int("dint-3d-dim2", 0, "")
	flag.Parse()

	client, err := ocxbp.Open()
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("FATAL Open: %s\n", ocxbp.Strerror(rc))
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	db, err := client.OpenTagDB(*path)
	if err != nil {
		rc := ocxbp.ErrCode(err)
		fmt.Printf("FATAL OpenTagDB: %s\n", ocxbp.Strerror(rc))
		os.Exit(2)
	}
	defer db.Close()
	n, _ := db.Build()
	fmt.Printf("[typetest] path=%s symbols=%d\n", *path, n)

	testBOOL(db, *tagBool)
	testSINT(db, *tagSint)
	testINT(db, *tagInt)
	testDINT(db, *tagDint)
	testLINT(db, *tagLint)
	testUSINT(db, *tagUsint)
	testUINT(db, *tagUint)
	testUDINT(db, *tagUdint)
	testULINT(db, *tagUlint)
	testREAL(db, *tagReal)
	testLREAL(db, *tagLreal)
	testString(db, *tagString)
	testDintArray(db, *tagDintArr, *arrCount)
	testBoolArray(db, *tagBoolArr, *boolArrCount)
	testDint2D(db, *tagDint2D, *dim0, *dim1)
	testDint3D(db, *tagDint3D, *d3d0, *d3d1, *d3d2)

	fmt.Printf("\n[typetest] PASS=%d FAIL=%d\n", passCount, failCount)
	if failCount == 0 {
		os.Exit(0)
	}
	os.Exit(1)
}
