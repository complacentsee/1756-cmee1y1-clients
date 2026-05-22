// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// IDObject is the CIP Identity Object (class 0x01). 48 bytes /
// 6 qwords on the wire, decoded into native fields.
type IDObject struct {
	VendorID     uint16
	DeviceType   uint16
	ProductCode  uint16
	MajorRev     uint8
	MinorRev     uint8
	Status       uint16
	SerialNumber uint32
	ProductName  [32]byte // SHORT_STRING padded with NULs
}

// decodeID parses the 48-byte Identity object starting at p[0].
// Layout per docs/protocol.md "GetIdObject"/"GetDeviceIdObject".
func decodeID(p []byte) IDObject {
	var id IDObject
	id.VendorID = binary.LittleEndian.Uint16(p[0x00:])
	id.DeviceType = binary.LittleEndian.Uint16(p[0x02:])
	id.ProductCode = binary.LittleEndian.Uint16(p[0x04:])
	id.MajorRev = p[0x06]
	id.MinorRev = p[0x07]
	id.Status = binary.LittleEndian.Uint16(p[0x08:])
	id.SerialNumber = binary.LittleEndian.Uint32(p[0x0A:])
	copy(id.ProductName[:], p[0x0E:0x0E+32])
	return id
}

// DecodeGetIdLocal reads the local cm1756 Identity from slot+0x78.
func DecodeGetIdLocal(slot []byte) IDObject {
	return decodeID(slot[HdrPayloadStart:])
}

// EncodeGetDeviceId writes the path string at +0x78 and the
// instance uint16 at +0x178. Path NUL-terminated, max 255 bytes.
func EncodeGetDeviceId(slot []byte, path string, instance uint16) {
	n := len(path)
	if n > 254 {
		n = 254
	}
	copy(slot[HdrPayloadStart:HdrPayloadStart+n], path)
	// trailing NUL already present from the dispatcher's clear
	binary.LittleEndian.PutUint16(slot[0x178:], instance)
}

// DecodeGetDeviceId reads the Identity response at slot+0x178.
func DecodeGetDeviceId(slot []byte) IDObject {
	return decodeID(slot[0x178:])
}

// DecodeGetActiveNodes returns (lo, hi) 32-bit halves of the
// 64-bit active-node bitmap. Bit N in (lo | hi<<32) is set when
// node N is responsive.
func DecodeGetActiveNodes(slot []byte) (lo, hi uint32) {
	lo = binary.LittleEndian.Uint32(slot[HdrPayloadStart:])
	hi = binary.LittleEndian.Uint32(slot[HdrPayloadStart+4:])
	return
}
