// SPDX-License-Identifier: MIT

package ocxbp

import (
	"bytes"
	"testing"
)

// TestSymbolicIOIPath verifies the ANSI Extended Symbol Segment encoding
// for structured-tag request paths: 0x91, name length, name bytes, then
// a NUL pad to an even byte count, with the size reported in 16-bit
// words. This is the framing the live controller accepted (handle
// 0x0A2C round-trip); a regression here would silently corrupt the CIP
// path. Pure-Go, no PLC.
func TestSymbolicIOIPath(t *testing.T) {
	cases := []struct {
		name      string
		wantBytes []byte
		wantWords uint8
	}{
		{
			// even-length name (4): no pad. 0x91,len + 4 = 6 bytes = 3 words.
			name:      "TSEQ",
			wantBytes: []byte{0x91, 0x04, 'T', 'S', 'E', 'Q'},
			wantWords: 3,
		},
		{
			// odd-length name (3): trailing NUL pad. 2+3+1 = 6 bytes = 3 words.
			name:      "Abc",
			wantBytes: []byte{0x91, 0x03, 'A', 'b', 'c', 0x00},
			wantWords: 3,
		},
		{
			// the real SUF register (26 chars, even): 2+26 = 28 bytes = 14 words.
			name:      "Tran_From_iSeries_Register",
			wantWords: 14,
		},
	}
	for _, c := range cases {
		t.Run(c.name, func(t *testing.T) {
			b, w := symbolicIOIPath(c.name)
			if len(b)%2 != 0 {
				t.Errorf("path not word-aligned: %d bytes", len(b))
			}
			if w != c.wantWords {
				t.Errorf("words = %d; want %d", w, c.wantWords)
			}
			if int(w)*2 != len(b) {
				t.Errorf("words (%d) * 2 != len bytes (%d)", w, len(b))
			}
			if c.wantBytes != nil && !bytes.Equal(b, c.wantBytes) {
				t.Errorf("bytes = % x; want % x", b, c.wantBytes)
			}
			if b[0] != 0x91 || b[1] != byte(len(c.name)) {
				t.Errorf("header = %02x %02x; want 91 %02x", b[0], b[1], len(c.name))
			}
		})
	}
}

// TestReadWriteStructArgValidation covers the pre-IPC guards so callers
// get a clear ErrParamRange instead of a malformed CIP request. No PLC.
func TestReadWriteStructArgValidation(t *testing.T) {
	var c *Client // nil client → ErrNullArg before any IPC
	if _, _, err := c.ReadStruct(2, "Tag", 600); err != ErrNullArg {
		t.Errorf("ReadStruct(nil client) = %v; want ErrNullArg", err)
	}
	if err := c.WriteStruct(2, "Tag", 0x0A2C, []byte{1}); err != ErrNullArg {
		t.Errorf("WriteStruct(nil client) = %v; want ErrNullArg", err)
	}
}
