// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// Unconnected_Send (CIP svc 0x52) body assembly + port-segment
// helpers (v0.8.0 Phase 3 — multi-hop routes).  Wire format
// documented in docs/protocol.md "Multi-hop routes —
// Unconnected_Send (service 0x52)".  Keep this file in sync with
// c/src/route.c and python/src/bpclient/_route.py.

const (
	// Standard CIP Unconnected_Send encoding: tick=5 → 32 ms units
	// (priority 0).  Logix gateways accept these values; OEM tooling
	// uses the same.
	ucsTickVal = 5
	ucsTickMs  = 32
)

// BuildUnconnectedSend assembles an Unconnected_Send request body in
// buf and returns the number of bytes written.  Returns 0 on argument
// error (NULL inputs, odd routeLen, oversized inputs) and a negative
// errno-shaped value matching the C SDK on capacity / range errors:
//
//	0    arg-NULL / size-zero
//	-22  EINVAL — routeLen is odd, or a size exceeds the wire layout
//	-28  ENOSPC — buf is too small for the result
//
// Maximum assembled size: 10 + len(embeddedMsg) + (len(embeddedMsg) &
// 1) + 2 + len(routePath).  Cap embedded size at MsgMaxReqSize - 14
// (~486 bytes) to keep the wrapped body inside the MessageSend 500 B
// envelope.
func BuildUnconnectedSend(buf []byte,
	embeddedMsg, routePath []byte, timeoutMs uint16) int {
	if buf == nil || embeddedMsg == nil || routePath == nil {
		return 0
	}
	if len(routePath)&1 != 0 {
		return -22
	}
	if len(embeddedMsg) == 0 || len(embeddedMsg) > 0xFFFF {
		return -22
	}
	if len(routePath) == 0 || len(routePath) > 0x1FE {
		return -22
	}
	// timeoutMs → ticks (1..255) at 32 ms units.
	ticks := (uint32(timeoutMs) + ucsTickMs - 1) / ucsTickMs
	if ticks < 1 {
		ticks = 1
	}
	if ticks > 255 {
		ticks = 255
	}
	pad := len(embeddedMsg) & 1
	total := 10 + len(embeddedMsg) + pad + 2 + len(routePath)
	if total > len(buf) {
		return -28
	}
	off := 0
	buf[off] = 0x52 // service: Unconnected_Send
	off++
	buf[off] = 0x02 // path_size in words
	off++
	buf[off] = 0x20
	off++
	buf[off] = 0x06 // class 0x06 = ConnMgr
	off++
	buf[off] = 0x24
	off++
	buf[off] = 0x01 // instance 1
	off++
	buf[off] = byte(ucsTickVal & 0x0F) // priority 0, tick = 5
	off++
	buf[off] = byte(ticks)
	off++
	binary.LittleEndian.PutUint16(buf[off:], uint16(len(embeddedMsg)))
	off += 2
	copy(buf[off:], embeddedMsg)
	off += len(embeddedMsg)
	if pad == 1 {
		buf[off] = 0x00
		off++
	}
	buf[off] = byte(len(routePath) / 2) // route_path size in words
	off++
	buf[off] = 0x00 // reserved
	off++
	copy(buf[off:], routePath)
	off += len(routePath)
	return off
}

// AppendPortSegment writes a 2-byte port segment {port, link} into
// route at *off, advancing *off.  Returns false if the buffer is too
// small.
func AppendPortSegment(route []byte, off *int, port, link uint8) bool {
	if *off+2 > len(route) {
		return false
	}
	route[*off] = port
	route[*off+1] = link
	*off += 2
	return true
}
