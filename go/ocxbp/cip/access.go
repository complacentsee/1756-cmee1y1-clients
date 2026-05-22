// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// AccessTagData layout constants (within a slot, after the header).
// Mirrors c/src/proto.h.
const (
	TagDataServiceOff    = 0x178 // uint16 service (unused by engine)
	TagDataCountOff      = 0x17A // uint16 request count
	TagDataReq0Start     = 0x180 // first descriptor
	TagDataReqStride     = 0x120 // bytes per descriptor

	// Within a request descriptor (relative to descriptor start)
	ReqTagNameOff       = 0x000 // char[256]
	ReqDataTypeOff      = 0x100 // uint16
	ReqElemByteSizeOff  = 0x102 // uint16  (vendor header named this "count" — wrong)
	ReqActionOff        = 0x104 // uint16  1=read, 2=write
	ReqElemCountOff     = 0x106 // uint16  (vendor header named this "elem_size" — wrong)
	ReqHasExtraOff      = 0x108 // uint8   always 0 in this SDK
	ReqDataPtrOff       = 0x110 // uint64  unused
	ReqResultOff        = 0x118 // uint32  server-written
)

// AccessTagData actions.
const (
	ActionRead  uint16 = 1
	ActionWrite uint16 = 2
)

// TagRequest is one entry in a batched AccessTagData call.
// For ActionRead, Data is the output buffer; for ActionWrite,
// it's the bytes to send. After the call, Result holds the
// per-request CIP General Status (0 = ok).
type TagRequest struct {
	TagName      string
	DataType     uint16
	ElemByteSize uint16
	Action       uint16 // ActionRead or ActionWrite
	ElemCount    uint16
	Data         []byte
	Result       uint32
}

// AccessTagDataPayloadSize returns the per-call payload_size to
// thread into shm.CallSpec. See docs/protocol.md "OCXcip_AccessTagData".
func AccessTagDataPayloadSize(reqs []TagRequest) uint32 {
	dataAreaStart := uint32(0x2A0)
	if n := len(reqs); n > 1 {
		dataAreaStart += uint32(n-1) * TagDataReqStride
	}
	var totalData uint32
	for i := range reqs {
		totalData += uint32(reqs[i].ElemByteSize) * uint32(reqs[i].ElemCount)
	}
	return dataAreaStart + totalData
}

// EncodeAccessTagData writes the path, count, per-request descriptors,
// and outbound write data into the slot. path is the OldI CIP path
// string (same format as CreateTagDbHandle: "P:1,S:2" etc.).
func EncodeAccessTagData(slot []byte, path string, reqs []TagRequest) {
	// Path at +0x78, NUL-terminated, up to 255 bytes (region already
	// zero from the dispatcher's bulk-clear, so we just memcpy).
	n := len(path)
	if n > 254 {
		n = 254
	}
	copy(slot[HdrPayloadStart:HdrPayloadStart+n], path)

	binary.LittleEndian.PutUint16(slot[TagDataServiceOff:], 0)
	binary.LittleEndian.PutUint16(slot[TagDataCountOff:], uint16(len(reqs)))

	dataOff := uint32(0x2A0)
	if n := len(reqs); n > 1 {
		dataOff += uint32(n-1) * TagDataReqStride
	}

	for i := range reqs {
		r := &reqs[i]
		reqStart := TagDataReq0Start + uint32(i)*TagDataReqStride

		// tag_name (max 255 bytes, NUL-padded by the dispatcher's clear).
		tn := len(r.TagName)
		if tn > 254 {
			tn = 254
		}
		copy(slot[reqStart+ReqTagNameOff:reqStart+ReqTagNameOff+uint32(tn)], r.TagName)

		binary.LittleEndian.PutUint16(slot[reqStart+ReqDataTypeOff:], r.DataType)
		binary.LittleEndian.PutUint16(slot[reqStart+ReqElemByteSizeOff:], r.ElemByteSize)
		binary.LittleEndian.PutUint16(slot[reqStart+ReqActionOff:], r.Action)
		binary.LittleEndian.PutUint16(slot[reqStart+ReqElemCountOff:], r.ElemCount)
		slot[reqStart+ReqHasExtraOff] = 0
		binary.LittleEndian.PutUint64(slot[reqStart+ReqDataPtrOff:], 0)

		nbytes := uint32(r.ElemByteSize) * uint32(r.ElemCount)
		if r.Action == ActionWrite && len(r.Data) > 0 && nbytes > 0 {
			cnt := nbytes
			if uint32(len(r.Data)) < cnt {
				cnt = uint32(len(r.Data))
			}
			copy(slot[dataOff:dataOff+cnt], r.Data[:cnt])
		}
		dataOff += nbytes
	}
}

// DecodeAccessTagData reads per-request result codes and (for reads)
// copies the data area back into each request's Data buffer.
func DecodeAccessTagData(slot []byte, reqs []TagRequest) {
	dataOff := uint32(0x2A0)
	if n := len(reqs); n > 1 {
		dataOff += uint32(n-1) * TagDataReqStride
	}
	for i := range reqs {
		r := &reqs[i]
		reqStart := TagDataReq0Start + uint32(i)*TagDataReqStride
		r.Result = binary.LittleEndian.Uint32(slot[reqStart+ReqResultOff:])
		nbytes := uint32(r.ElemByteSize) * uint32(r.ElemCount)
		if r.Action == ActionRead && len(r.Data) > 0 && nbytes > 0 {
			cnt := nbytes
			if uint32(len(r.Data)) < cnt {
				cnt = uint32(len(r.Data))
			}
			copy(r.Data[:cnt], slot[dataOff:dataOff+cnt])
		}
		dataOff += nbytes
	}
}
