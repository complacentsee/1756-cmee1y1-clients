// structrwtest — live validator for the SDK's atomic ReadStruct /
// WriteStruct (CIP 0x4C/0x4D over MessageSend). READ-ONLY unless -write,
// where it writes the just-read bytes back (no value change) and
// confirms the round-trip is byte-identical. Run with the SDK IPC flags
// (--ipc=host --pid=host -v /dev/shm:/dev/shm) on the module; bpServer
// is single-client, so stop any gateway first.
package main

import (
	"bytes"
	"flag"
	"fmt"
	"os"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp"
)

func main() {
	slot := flag.Int("slot", 2, "controller backplane slot (N in P:1,S:N)")
	tag := flag.String("tag", "Tran_From_iSeries_Register", "structured tag to access")
	doWrite := flag.Bool("write", false, "write the just-read bytes back (no value change) and verify round-trip")
	flag.Parse()
	if err := run(uint8(*slot), *tag, *doWrite); err != nil {
		fmt.Println("FAIL:", err)
		os.Exit(1)
	}
}

func run(slot uint8, tag string, doWrite bool) error {
	c, err := ocxbp.Open()
	if err != nil {
		return fmt.Errorf("Open: %w", err)
	}
	defer c.Close()
	if _, err := c.OpenSession(); err != nil {
		return fmt.Errorf("OpenSession: %w", err)
	}

	data, handle, err := c.ReadStruct(slot, tag, 600)
	if err != nil {
		return fmt.Errorf("ReadStruct: %w", err)
	}
	fmt.Printf("ReadStruct OK: tag=%q handle=0x%04x bytes=%d\n", tag, handle, len(data))

	if !doWrite {
		fmt.Println("VERDICT: SDK ReadStruct works (read-only).")
		return nil
	}

	if err := c.WriteStruct(slot, tag, handle, data); err != nil {
		return fmt.Errorf("WriteStruct: %w", err)
	}
	back, _, err := c.ReadStruct(slot, tag, 600)
	if err != nil {
		return fmt.Errorf("ReadStruct(verify): %w", err)
	}
	if !bytes.Equal(data, back) {
		return fmt.Errorf("round-trip mismatch (%d vs %d bytes / contents differ)", len(data), len(back))
	}
	fmt.Println("VERDICT: SDK WriteStruct accepted (CIP 0) + round-trip byte-identical — ATOMIC UDT I/O works.")
	return nil
}
