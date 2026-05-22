// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// OCXcip_ParsePath slot layout (RE'd from libocxbpapi-w.so 0x1094f0):
//
//	+0x078 text_path (NUL-terminated, max 255 bytes)
//	+0x178 out_class (u16)
//	+0x17A out_segment_flags (u8)
//	+0x17C out_instance (u32)
//	+0x180 out_encoded_path bytes (up to 256)
//	+0x280 out_path_size (u16) — caller's cap on input, server-written on output
//	+0x282 out_attr_flags (u8)
const (
	PPPathOff       = 0x078
	PPClassOff      = 0x178
	PPSegFlagsOff   = 0x17A
	PPInstanceOff   = 0x17C
	PPEncodedOff    = 0x180
	PPSizeOff       = 0x280
	PPAttrFlagsOff  = 0x282
)

// ParsePathResult is the decoded ParsePath response.
type ParsePathResult struct {
	Encoded   []byte // copied from slot+0x180..+EncodedLen
	Class     uint16
	SegFlags  uint8
	Instance  uint32
	AttrFlags uint8
}

// EncodeParsePath writes the text path (NUL-terminated, max 255) at
// +0x078 and the caller's cap (u16) at +0x280.
func EncodeParsePath(slot []byte, textPath string, cap uint16) {
	n := len(textPath)
	if n > 254 {
		n = 254
	}
	copy(slot[PPPathOff:PPPathOff+n], textPath)
	// trailing NUL already zeroed by dispatcher
	binary.LittleEndian.PutUint16(slot[PPSizeOff:], cap)
}

// DecodeParsePath reads the response into a ParsePathResult.
func DecodeParsePath(slot []byte) ParsePathResult {
	encLen := binary.LittleEndian.Uint16(slot[PPSizeOff:])
	if encLen > 256 {
		encLen = 256
	}
	enc := make([]byte, encLen)
	copy(enc, slot[PPEncodedOff:PPEncodedOff+int(encLen)])
	return ParsePathResult{
		Encoded:   enc,
		Class:     binary.LittleEndian.Uint16(slot[PPClassOff:]),
		SegFlags:  slot[PPSegFlagsOff],
		Instance:  binary.LittleEndian.Uint32(slot[PPInstanceOff:]),
		AttrFlags: slot[PPAttrFlagsOff],
	}
}
