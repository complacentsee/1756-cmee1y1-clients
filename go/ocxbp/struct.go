// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
)

// Structured (whole-UDT) tag access via CIP Read Tag (0x4C) / Write Tag
// (0x4D) over the raw MessageSend (UCMM) path.
//
// Unlike the AccessTagData family, this reads/writes a UDT instance as
// ONE CIP transaction — atomic on the controller. A structured Write
// Tag must carry the 2-byte structure-template handle the controller
// assigned; the controller hands that handle back in the reply to a
// structured Read Tag, so ReadStruct returns it and WriteStruct (or
// the caller, via a cached handle) supplies it back.
//
// Wire shapes (CIP request body — service, path_size_words, path, body):
//
//	Read Tag (0x4C):  [0x4C][words][0x91 len name pad][elem_count u16]
//	  reply data:     [type u16 = 0x02A0][handle u16][payload...]
//
//	Write Tag (0x4D): [0x4D][words][0x91 len name pad]
//	                  [0xA0 0x02][handle u16][elem_count u16][payload...]
//
// Limits: MessageSend is UCMM, so request+reply must fit MaxReqSize
// (500) / the response buffer. The SUF registers (104 B) fit easily;
// larger UDTs would need a connected/fragmented path (future work).
//
// Slot is the backplane slot of the controller (the N in "P:1,S:N").

// StructTypeAbbrev is the CIP "abbreviated structure" type code that
// prefixes structured-tag data on the wire (0x02A0 = 0xA0 with the
// structured bit). The 2-byte template handle follows it.
const StructTypeAbbrev uint16 = 0x02A0

// symbolicIOIPath builds an ANSI Extended Symbol Segment (0x91) request
// path for tag and returns the bytes plus the path size in 16-bit
// words (the value the CIP request header carries). The name is padded
// to an even length with a trailing NUL per CIP path encoding.
func symbolicIOIPath(tag string) ([]byte, uint8) {
	b := make([]byte, 0, 2+len(tag)+1)
	b = append(b, 0x91, byte(len(tag)))
	b = append(b, tag...)
	if len(b)%2 != 0 {
		b = append(b, 0x00)
	}
	return b, uint8(len(b) / 2)
}

// ReadStruct reads a whole structured (UDT) tag in one CIP Read Tag
// transaction and returns the raw struct payload plus the controller-
// assigned 2-byte structure handle. Pass the handle to WriteStruct to
// write the same UDT back.
//
// slot is the controller's backplane slot (the N in "P:1,S:N").
// respCap bounds the reply buffer; pass the struct's byte size + a
// little headroom (the reply adds a ~6-byte CIP header + the 4-byte
// type/handle prefix). A nonzero CIP general status returns *CIPError.
func (c *Client) ReadStruct(slot uint8, tag string, respCap int) (data []byte, handle uint16, err error) {
	if c == nil {
		return nil, 0, ErrNullArg
	}
	if tag == "" || len(tag) > 250 {
		return nil, 0, ErrParamRange
	}
	if respCap < 16 || respCap > 0xFFFF {
		return nil, 0, ErrParamRange
	}
	ioi, words := symbolicIOIPath(tag)
	req := make([]byte, 0, 2+len(ioi)+2)
	req = append(req, 0x4C, words)
	req = append(req, ioi...)
	req = append(req, 0x01, 0x00) // elem_count = 1

	resp := make([]byte, respCap)
	m := &Message{
		Slot:         slot,
		CipRequest:   req,
		ReqSize:      uint16(len(req)),
		RespData:     resp,
		RespCapacity: uint16(respCap),
	}
	if err := c.MessageSend(m); err != nil {
		return nil, 0, err
	}
	r := m.RespData[:m.RespLen]
	if len(r) < 4 {
		return nil, 0, ErrGeneric
	}
	svc, status, extWords := r[0], r[2], int(r[3])
	if status != 0 {
		var ext uint16
		if extWords >= 1 && len(r) >= 4+extWords*2 {
			ext = binary.LittleEndian.Uint16(r[4:])
		}
		return nil, 0, &CIPError{Service: svc, Status: status, ExtStatus: ext, Slot: slot}
	}
	body := r[4+extWords*2:]
	if len(body) < 4 {
		return nil, 0, ErrGeneric
	}
	// body = [type u16][handle u16][payload...]
	handle = binary.LittleEndian.Uint16(body[2:4])
	payload := make([]byte, len(body)-4)
	copy(payload, body[4:])
	return payload, handle, nil
}

// WriteStruct writes a whole structured (UDT) tag in one CIP Write Tag
// transaction — atomic on the controller. handle is the 2-byte template
// handle from ReadStruct for this tag; data is the full struct payload
// (exactly the struct's byte size, controller-authoritative). A nonzero
// CIP general status returns *CIPError.
func (c *Client) WriteStruct(slot uint8, tag string, handle uint16, data []byte) error {
	if c == nil {
		return ErrNullArg
	}
	if tag == "" || len(tag) > 250 {
		return ErrParamRange
	}
	if len(data) == 0 {
		return ErrParamRange
	}
	ioi, words := symbolicIOIPath(tag)
	req := make([]byte, 0, 2+len(ioi)+6+len(data))
	req = append(req, 0x4D, words)
	req = append(req, ioi...)
	// abbreviated-structure type + handle
	req = append(req, 0xA0, 0x02)
	hb := make([]byte, 2)
	binary.LittleEndian.PutUint16(hb, handle)
	req = append(req, hb...)
	req = append(req, 0x01, 0x00) // elem_count = 1
	req = append(req, data...)
	if len(req) > int(cip.MaxReqSize) {
		// UCMM request cap — larger structs need a connected/fragmented
		// path; surface a clear range error rather than a truncated write.
		return ErrParamRange
	}

	resp := make([]byte, 64)
	m := &Message{
		Slot:         slot,
		CipRequest:   req,
		ReqSize:      uint16(len(req)),
		RespData:     resp,
		RespCapacity: uint16(len(resp)),
	}
	if err := c.MessageSend(m); err != nil {
		return err
	}
	r := m.RespData[:m.RespLen]
	if len(r) < 3 {
		return ErrGeneric
	}
	svc, status, extWords := r[0], r[2], int(r[3])
	if status != 0 {
		var ext uint16
		if extWords >= 1 && len(r) >= 4+extWords*2 {
			ext = binary.LittleEndian.Uint16(r[4:])
		}
		return &CIPError{Service: svc, Status: status, ExtStatus: ext, Slot: slot}
	}
	return nil
}
