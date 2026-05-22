// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// MessageSend slot layout (per docs/protocol.md "OCXcip_MessageSend").
const (
	MsgSendReqOff      = 0x00078
	MsgSendReqSizeOff  = 0x19078
	MsgSendRespDataOff = 0x1907A
	MsgSendRespLenOff  = 0x3207A // resp_capacity (in) / resp_len (out)
	MsgSendStatusOff   = 0x3207C
	MsgSendSlotOff     = 0x32080
	MsgSendTimeoutOff  = 0x32082

	// MaxSlot is the engine-validated backplane slot range (< 0x14).
	MaxSlot uint8 = 0x13
	// MaxReqSize is the engine-validated cip_request byte count.
	MaxReqSize uint16 = 500
	// MinTimeoutMs is the engine clamp (anything < 26 is silently clamped).
	MinTimeoutMs uint16 = 26
)

// Message describes one UCMM CIP request/response pair. Field names
// follow the Phase 4 wire correction (the OEM wrapper's "service",
// "encoded_path", "class_or_misc" are misnomers — see c/src/message.c
// + docs/protocol.md).
type Message struct {
	Slot         uint8  // backplane slot 0..0x13
	CipRequest   []byte // full CIP body: [service, path_size_words, path..., body...]
	ReqSize      uint16 // byte count of CipRequest, 1..500
	TimeoutMs    uint16 // per-attempt timeout (engine min 26)
	RespCapacity uint16 // caller buffer size (must be > 0)
	RespData     []byte // OUT: response bytes (raw CIP reply)
	RespLen      uint16 // OUT: actual bytes the engine wrote
	Status       uint32 // OUT: wrapper status field
}

// EncodeMessageSend lays the Message into the slot. Caller must
// pre-validate ReqSize / RespCapacity / Slot ranges.
func EncodeMessageSend(slot []byte, m *Message) {
	if m.ReqSize > 0 {
		copy(slot[MsgSendReqOff:MsgSendReqOff+int(m.ReqSize)], m.CipRequest[:m.ReqSize])
	}
	binary.LittleEndian.PutUint16(slot[MsgSendReqSizeOff:], m.ReqSize)
	binary.LittleEndian.PutUint16(slot[MsgSendRespLenOff:], m.RespCapacity)
	slot[MsgSendSlotOff] = m.Slot
	binary.LittleEndian.PutUint16(slot[MsgSendTimeoutOff:], m.TimeoutMs)
}

// DecodeMessageSend reads the response back into the Message. The
// caller-provided RespData buffer is filled with min(RespCapacity,
// engine resp_len) bytes; RespLen is set to the actual count.
func DecodeMessageSend(slot []byte, m *Message) {
	got := binary.LittleEndian.Uint16(slot[MsgSendRespLenOff:])
	m.Status = binary.LittleEndian.Uint32(slot[MsgSendStatusOff:])
	if got > m.RespCapacity {
		got = m.RespCapacity
	}
	m.RespLen = got
	if len(m.RespData) > 0 && got > 0 {
		n := int(got)
		if n > len(m.RespData) {
			n = len(m.RespData)
		}
		copy(m.RespData[:n], slot[MsgSendRespDataOff:MsgSendRespDataOff+n])
	}
}
