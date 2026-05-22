// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// DecodeGetSwitchPosition reads the front-panel switch value at
// slot+0x78.
func DecodeGetSwitchPosition(slot []byte) uint32 {
	return binary.LittleEndian.Uint32(slot[HdrPayloadStart:])
}

// EncodeGetLED writes led_id at slot+0x78.
func EncodeGetLED(slot []byte, ledID uint32) {
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart:], ledID)
}

// DecodeGetLED reads the LED state at slot+0x7C.
func DecodeGetLED(slot []byte) uint32 {
	return binary.LittleEndian.Uint32(slot[HdrPayloadStart+4:])
}

// EncodeSetLED writes led_id at +0x78 and state at +0x7C.
func EncodeSetLED(slot []byte, ledID, state uint32) {
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart:], ledID)
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart+4:], state)
}

// DecodeGetDisplay copies the 4-char display value from slot+0x78
// into a fixed [5]byte buffer (with trailing NUL).
func DecodeGetDisplay(slot []byte) [5]byte {
	var out [5]byte
	copy(out[:4], slot[HdrPayloadStart:HdrPayloadStart+4])
	out[4] = 0
	return out
}

// EncodeSetDisplay writes 4 chars at +0x78 and a NUL at +0x7C.
func EncodeSetDisplay(slot []byte, fourChars [4]byte) {
	copy(slot[HdrPayloadStart:HdrPayloadStart+4], fourChars[:])
	slot[HdrPayloadStart+4] = 0
}
