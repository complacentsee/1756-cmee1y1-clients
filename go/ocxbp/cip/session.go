// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// DecodeOpenSession reads the OCXcip_Open response handle.
// Per docs/protocol.md "OCXcip_Open": session_handle at slot+0x78.
// The handle is opaque (the wrapper tracks state via slot ownership,
// not the handle) — but the call must succeed before any other
// OCXcip_* opcode works.
func DecodeOpenSession(slot []byte) uint32 {
	return binary.LittleEndian.Uint32(slot[HdrPayloadStart:])
}
