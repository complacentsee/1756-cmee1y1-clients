// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// Class-3 connected CIP messaging encoders.  v0.7.0 implements
// TxRxOpen / TxRxMsg / TxRxClose on top of MessageSend by building
// CIP Large Forward Open (0x5B) and Forward_Close (0x4E) bodies
// here and shipping them through the regular MessageSend transport
// (chip mailbox 0x200, UCMM).  The OCXcip_TxRx* OEM entry points
// are not used — they dispatch to OCXCN_* in a library missing
// from the cm1756 image.
//
// Wire format documented in docs/protocol.md "Connected messaging
// — wire format".  Sibling reference:
// historianupdate/driver/apex2/daemon/apex2_cip_connection.c.

// LFO and FC sizing.  ConnSpec.ConnParams, if non-zero, sets the
// O→T / T→O size in BYTES.  0 → SDK default 4000 (matches OCX).
// 4002 is the absolute hardware ceiling; values above are clamped
// with a warning in the conn-state layer to protect against stale
// OEM 16-bit conn_params encodings (e.g. 0x43E8 = 17384).
const (
	LFOMinReqSize     = 50           // request body bytes
	LFODefaultOTSize  = uint16(4000) // SDK default O→T/T→O size
	LFOMaxOTSize      = uint16(4002) // absolute hardware max
	lfoParamsHi       = uint32(0x42000000)
	lfoRPIMicroseconds = uint32(10000000) // 10 s, matches sibling FO_OT_RPI_US
	lfoOTHint         = uint32(0x80010000)
	lfoTOHint         = uint32(0x80000001)
	LFOVendorID       = uint16(0x0001)

	FCReqSize = 22

	TxRxMaxPath = 0xFF
)

// ConnSpec is the caller-managed connection descriptor; same fields
// flow through Open / Msg / Close.  The SDK's connection cache is
// keyed on AppHandle.
type ConnSpec struct {
	AppHandle   uint16
	Options     uint32   // accepted for API stability; not used in v0.7.0
	EncodedPath []byte   // backplane-direct only: {0x01, slot}
	PathSize    uint16
	ConnParams  uint16   // O→T/T→O size in bytes; 0 = default 4000
}

// ExtractSlot pulls the backplane slot out of a canonical
// {0x01, slot} EncodedPath.  Returns the slot and true on success.
// Multi-hop routes are out-of-scope for v0.7.0.
func ExtractSlot(path []byte, pathSize uint16) (uint8, bool) {
	if pathSize != 2 || len(path) < 2 || path[0] != 0x01 {
		return 0, false
	}
	return path[1], true
}

// BuildForwardOpen writes a Large Forward Open (CIP service 0x5B)
// request body into buf and returns the byte count.  buf must
// have ≥ LFOMinReqSize (50) bytes of capacity.
//
// Mirrors c/src/conn.c::build_lfo and historianupdate
// apex2_cip_connection.c::build_forward_open (lines 682-820).
func BuildForwardOpen(buf []byte, connSerial uint16, origSerial uint32, otSizeBytes uint16) int {
	off := 0
	buf[off] = 0x5B // Large Forward Open
	off++
	buf[off] = 0x02 // path size words
	off++
	buf[off] = 0x20
	off++
	buf[off] = 0x06 // class 6 (CM)
	off++
	buf[off] = 0x24
	off++
	buf[off] = 0x01 // instance 1
	off++
	buf[off] = 0x05 // priority/tick
	off++
	buf[off] = 0xF7 // timeout ticks
	off++
	binary.LittleEndian.PutUint32(buf[off:], lfoOTHint)
	off += 4
	binary.LittleEndian.PutUint32(buf[off:], lfoTOHint)
	off += 4
	binary.LittleEndian.PutUint16(buf[off:], connSerial)
	off += 2
	binary.LittleEndian.PutUint16(buf[off:], LFOVendorID)
	off += 2
	binary.LittleEndian.PutUint32(buf[off:], origSerial)
	off += 4
	binary.LittleEndian.PutUint32(buf[off:], 0x00000003) // timeout multiplier
	off += 4
	binary.LittleEndian.PutUint32(buf[off:], lfoRPIMicroseconds) // O→T RPI µs
	off += 4
	binary.LittleEndian.PutUint32(buf[off:], lfoParamsHi|uint32(otSizeBytes)) // O→T conn params
	off += 4
	binary.LittleEndian.PutUint32(buf[off:], lfoRPIMicroseconds) // T→O RPI µs
	off += 4
	binary.LittleEndian.PutUint32(buf[off:], lfoParamsHi|uint32(otSizeBytes)) // T→O conn params
	off += 4
	buf[off] = 0xA3 // transport trigger: Class 3, server
	off++
	buf[off] = 0x02 // conn path size words
	off++
	buf[off] = 0x20
	off++
	buf[off] = 0x02 // class 2 (Msg Router)
	off++
	buf[off] = 0x24
	off++
	buf[off] = 0x01 // instance 1
	off++
	return off
}

// ParseForwardOpen reads the LFO reply.  Returns (otConnID, toConnID,
// generalStatus, ok).  ok=true means service is 0xDB or 0xD4 AND
// general_status == 0; otherwise the connection wasn't established
// and otConnID / toConnID are zero.
func ParseForwardOpen(resp []byte) (otConnID, toConnID uint32, status uint8, ok bool) {
	if len(resp) < 12 {
		return 0, 0, 0xFF, false
	}
	if resp[0] != 0xDB && resp[0] != 0xD4 {
		return 0, 0, resp[2], false
	}
	status = resp[2]
	if status != 0x00 {
		return 0, 0, status, false
	}
	otConnID = binary.LittleEndian.Uint32(resp[4:])
	toConnID = binary.LittleEndian.Uint32(resp[8:])
	return otConnID, toConnID, status, true
}

// BuildForwardClose writes a Forward_Close (CIP service 0x4E)
// request body into buf and returns the byte count (always 22).
// All four identifier fields must match the LFO exactly — the PLC
// matches against its connection table.
//
// Mirrors c/src/conn.c::build_fc and apex2_cip_connection.c::
// build_forward_close (lines 1244-1283).
func BuildForwardClose(buf []byte, connSerial, vendorID uint16, origSerial uint32) int {
	off := 0
	buf[off] = 0x4E
	off++
	buf[off] = 0x02
	off++
	buf[off] = 0x20
	off++
	buf[off] = 0x06
	off++
	buf[off] = 0x24
	off++
	buf[off] = 0x01
	off++
	buf[off] = 0x0A // priority/tick
	off++
	buf[off] = 0x0E // timeout ticks
	off++
	binary.LittleEndian.PutUint16(buf[off:], connSerial)
	off += 2
	binary.LittleEndian.PutUint16(buf[off:], vendorID)
	off += 2
	binary.LittleEndian.PutUint32(buf[off:], origSerial)
	off += 4
	buf[off] = 0x02 // conn path size words
	off++
	buf[off] = 0x00 // reserved
	off++
	buf[off] = 0x20
	off++
	buf[off] = 0x02
	off++
	buf[off] = 0x24
	off++
	buf[off] = 0x01
	off++
	return off
}

// ParseForwardClose reads the FC reply.  Returns (general_status, ok).
// ok=true means service byte is 0xCE and general_status == 0.
func ParseForwardClose(resp []byte) (status uint8, ok bool) {
	if len(resp) < 4 {
		return 0xFF, false
	}
	if resp[0] != 0xCE {
		return resp[2], false
	}
	return resp[2], resp[2] == 0x00
}
