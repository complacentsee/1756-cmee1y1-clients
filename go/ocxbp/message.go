// SPDX-License-Identifier: MIT

package ocxbp

import (
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// Message describes one UCMM CIP request/response over the
// backplane. Field names match the Phase 4 wire correction in
// docs/protocol.md (the OEM wrapper's "service",
// "encoded_path", "class_or_misc" names are misnomers — see
// c/src/message.c).
//
// Slot is the BACKPLANE SLOT NUMBER (0..0x13), NOT a CIP service.
// CipRequest is the FULL CIP request body: [service, path_size_words,
// path..., body...]. The chip copies it verbatim into the UCMM TX
// buffer; no port segments are prepended.
type Message = cip.Message

// Engine validation limits — match c/include/bpclient.h.
const (
	MsgMaxSlot       = cip.MaxSlot
	MsgMaxReqSize    = cip.MaxReqSize
	MsgMinTimeoutMs  = cip.MinTimeoutMs
)

// MessageSend dispatches one UCMM CIP request to a chosen backplane
// slot and reads the raw CIP reply. Returns nil if the message
// round-tripped — caller must inspect the first ~4 bytes of
// m.RespData (reply_service, reserved, general_status, ext_status_size)
// for CIP-level success.
//
// Positive engine codes surface via EngineError:
//
//	1  = bad param (e.g. slot >= 0x14)
//	3  = engine refused / target unresponsive (empty slot)
//	14 = retry budget exhausted (raise TimeoutMs)
func (c *Client) MessageSend(m *Message) error {
	if c == nil || m == nil {
		return ErrNullArg
	}
	if m.CipRequest == nil || m.ReqSize == 0 || m.ReqSize > MsgMaxReqSize {
		return ErrParamRange
	}
	if m.RespData == nil || m.RespCapacity == 0 {
		return ErrNullArg
	}
	if m.Slot > MsgMaxSlot {
		return ErrSlotTooLarge
	}
	if int(m.ReqSize) > len(m.CipRequest) {
		return ErrParamRange
	}

	m.RespLen = 0
	m.Status = 0

	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnMessageSend,
		PayloadSize: cip.SizeMessageSend,
		Fill:        func(slot []byte) { cip.EncodeMessageSend(slot, m) },
		Read:        func(slot []byte) { cip.DecodeMessageSend(slot, m) },
		TimeoutMs:   5000,
	})
	return translateCallErr(err)
}
