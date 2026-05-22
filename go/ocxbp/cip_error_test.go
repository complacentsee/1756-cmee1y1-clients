// SPDX-License-Identifier: MIT

package ocxbp

import (
	"errors"
	"strings"
	"testing"
)

// Verify *CIPError carries the four wire-level fields and that
// errors.As + ErrCode round-trip correctly.
func TestCIPErrorAsExtractsStructuredFields(t *testing.T) {
	var err error = &CIPError{
		Service:   0xDB,
		Status:    0x01,
		ExtStatus: 0x0100,
		Slot:      2,
	}
	var ce *CIPError
	if !errors.As(err, &ce) {
		t.Fatalf("errors.As should match *CIPError")
	}
	if ce.Service != 0xDB || ce.Status != 0x01 || ce.ExtStatus != 0x0100 || ce.Slot != 2 {
		t.Fatalf("unexpected fields: %+v", ce)
	}
}

func TestCIPErrorCodeMapsToCIPStatusSentinel(t *testing.T) {
	var err error = &CIPError{Service: 0xDB, Status: 0x01, ExtStatus: 0x0100, Slot: 2}
	if got, want := ErrCode(err), BPErrCIPStatus; got != want {
		t.Fatalf("ErrCode = %d, want %d", got, want)
	}
}

func TestCIPStatusStringKnownPairs(t *testing.T) {
	cases := []struct {
		status uint8
		ext    uint16
		want   string
	}{
		{0x01, 0x0100, "connection in use"},
		{0x01, 0x0103, "transport class"},
		{0x01, 0x0107, "connection id not found"},
		{0x02, 0x0000, "resource unavailable"},
		{0x05, 0x0000, "path destination"},
	}
	for _, tc := range cases {
		got := strings.ToLower(CIPStatusString(tc.status, tc.ext))
		if !strings.Contains(got, tc.want) {
			t.Errorf("CIPStatusString(0x%02x, 0x%04x) = %q, want substring %q",
				tc.status, tc.ext, got, tc.want)
		}
	}
}

func TestCIPStatusStringUnknownFallsBack(t *testing.T) {
	// Status 0x01 with no matching ext → "connection failure".
	got := strings.ToLower(CIPStatusString(0x01, 0xFFFF))
	if !strings.Contains(got, "connection failure") {
		t.Errorf("unknown 0x01 ext: got %q", got)
	}
	// Status outside the table → "unknown CIP status".
	got = strings.ToLower(CIPStatusString(0xFF, 0))
	if !strings.Contains(got, "unknown") {
		t.Errorf("unknown status: got %q", got)
	}
}

func TestCIPErrorErrorIncludesStatusString(t *testing.T) {
	ce := &CIPError{Service: 0xDB, Status: 0x01, ExtStatus: 0x0100, Slot: 2}
	s := strings.ToLower(ce.Error())
	for _, want := range []string{"svc=0xdb", "status=0x01", "ext=0x0100", "slot=2", "connection in use"} {
		if !strings.Contains(s, want) {
			t.Errorf("CIPError.Error() missing %q in %q", want, s)
		}
	}
}

func TestStrerrorForCIPStatusCode(t *testing.T) {
	s := strings.ToLower(Strerror(BPErrCIPStatus))
	if !strings.Contains(s, "cip-layer rejection") {
		t.Errorf("Strerror(BPErrCIPStatus) = %q, want substring 'CIP-layer rejection'", s)
	}
}
