// identity — print the Identity object of a CIP device.
//
//	identity                    # local cm1756
//	identity --path "P:1,S:1"   # remote via OEM-parsed text path
//
// SPDX-License-Identifier: MIT
package main

import (
	"flag"
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func vendorName(id uint16) string {
	switch id {
	case 0x0001:
		return "Allen-Bradley (Rockwell)"
	case 0x0030:
		return "Online Development Inc."
	default:
		return "(unknown)"
	}
}

func printID(id ocxbp.IDObject) {
	// product_name in OCXcip_GetIdObject is sometimes SHORT_STRING
	// (one-byte length prefix), sometimes zero-padded. Skip leading
	// NULs, take until next NUL or end. Matches the C heuristic.
	skip := 0
	for skip < 31 && id.ProductName[skip] == 0 {
		skip++
	}
	take := 0
	for skip+take < 32 && id.ProductName[skip+take] != 0 && take < 32 {
		take++
	}
	name := string(id.ProductName[skip : skip+take])
	fmt.Printf("  Vendor ID       : 0x%04x  (%s)\n", id.VendorID, vendorName(id.VendorID))
	fmt.Printf("  Device Type     : 0x%04x\n", id.DeviceType)
	fmt.Printf("  Product Code    : 0x%04x  (%d)\n", id.ProductCode, id.ProductCode)
	fmt.Printf("  Revision        : %d.%d\n", id.MajorRev, id.MinorRev)
	fmt.Printf("  Status          : 0x%04x\n", id.Status)
	fmt.Printf("  Serial Number   : 0x%08x  (%d)\n", id.SerialNumber, id.SerialNumber)
	fmt.Printf("  Product Name    : '%s'\n", name)
}

func main() {
	textPath := flag.String("path", "", "remote device text path (e.g. \"P:1,S:2\"); empty = local cm1756")
	flag.Parse()

	client, err := ocxbp.Open()
	if err != nil {
		fmt.Fprintln(os.Stderr, "client open failed")
		os.Exit(2)
	}
	defer client.Close()
	_, _ = client.OpenSession()

	// Always print active node table for context.
	if lo, hi, err := client.GetActiveNodes(); err == nil {
		fmt.Printf("[active nodes] mask_lo=0x%08x  mask_hi=0x%08x  =>", lo, hi)
		for i := uint32(0); i < 32; i++ {
			if lo&(1<<i) != 0 {
				fmt.Printf(" %d", i)
			}
		}
		for i := uint32(0); i < 32; i++ {
			if hi&(1<<i) != 0 {
				fmt.Printf(" %d", i+32)
			}
		}
		fmt.Println()
		fmt.Println()
	}

	if *textPath == "" {
		fmt.Println("=== LOCAL Identity (Client.GetIDLocal) ===")
		id, err := client.GetIDLocal()
		rc := ocxbp.ErrCode(err)
		fmt.Printf("  rc=%d (%s)\n", rc, ocxbp.Strerror(rc))
		if err == nil {
			printID(id)
		}
	} else {
		fmt.Printf("=== REMOTE Identity via OCXcip_GetDeviceIdObject('%s', inst=1) ===\n", *textPath)
		id, err := client.GetDeviceID(*textPath, 1)
		rc := ocxbp.ErrCode(err)
		fmt.Printf("  rc=%d (%s)\n", rc, ocxbp.Strerror(rc))
		if err == nil {
			printID(id)
		}
	}
}
