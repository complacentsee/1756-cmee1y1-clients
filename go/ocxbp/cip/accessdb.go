// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// AccessTagDataDb layout constants (v0.10.4+). Peer of AccessTagData
// using the cached db_handle from CreateTagDbHandle instead of the
// path string. Wire layout — including the +8 descriptor stride
// (0x128 vs 0x120), the widened u32 data_type/elem_byte_size/
// elem_count fields, and the new has_data / mask_seed pair — is
// described in docs/access-tag-data-db.md. Mirrors c/src/proto.h.
const (
	TagDataDbHandleOff   = 0x78  // uint32 db_handle
	TagDataDbHasExtraOff = 0x7C  // uint8  has_extra (we ship 0)
	TagDataDbOptValueOff = 0x7E  // uint16 opt_value (we ship 0)
	TagDataDbCountOff    = 0x80  // uint16 request count
	TagDataDbReq0Start   = 0x88  // first descriptor
	TagDataDbReqStride   = 0x128 // bytes per descriptor
	TagDataDbDataArea0   = 0x1B0 // data area start when count == 1

	// Within an AccessTagDataDb request descriptor (relative to
	// descriptor start). Field offsets and widths DIFFER from
	// AccessTagData — see the table in docs/access-tag-data-db.md.
	ReqDbTagNameOff      = 0x000 // char[255] + NUL
	ReqDbActionOff       = 0x100 // uint16 1=read 2=write
	ReqDbDataTypeOff     = 0x104 // uint32 (widened from u16)
	ReqDbElemByteSizeOff = 0x108 // uint32 (widened from u16)
	ReqDbElemCountOff    = 0x10C // uint32 (widened from u16)
	ReqDbHasDataOff      = 0x110 // uint8  (we ship 0)
	ReqDbMaskSeedOff     = 0x118 // uint64 (we ship 0)
	ReqDbResultOff       = 0x120 // uint32 server-written
)

// AccessTagDataDbPayloadSize returns the per-call payload_size for
// OCXcip_AccessTagDataDb. See docs/protocol.md "OCXcip_AccessTagDataDb".
func AccessTagDataDbPayloadSize(reqs []TagRequest) uint32 {
	dataAreaStart := uint32(TagDataDbDataArea0)
	if n := len(reqs); n > 1 {
		dataAreaStart += uint32(n-1) * TagDataDbReqStride
	}
	var totalData uint32
	for i := range reqs {
		totalData += uint32(reqs[i].ElemByteSize) * uint32(reqs[i].ElemCount)
	}
	return dataAreaStart + totalData
}

// EncodeAccessTagDataDb writes the db_handle, count, per-request
// descriptors, and outbound write data into the slot. Mirrors
// EncodeAccessTagData but for the AccessTagDataDb wire format —
// the descriptor stride, field offsets, and field widths all differ.
//
// has_extra, opt_value, has_data, and mask_seed are all left at 0
// (matching the OEM wrapper's NULL-pointer branches; engine reads
// data from the slot data area, which we populate inline below).
func EncodeAccessTagDataDb(slot []byte, dbHandle uint32, reqs []TagRequest) {
	binary.LittleEndian.PutUint32(slot[TagDataDbHandleOff:], dbHandle)
	binary.LittleEndian.PutUint16(slot[TagDataDbCountOff:], uint16(len(reqs)))
	// has_extra (byte) and opt_value (u16) stay 0 from the
	// dispatcher's bulk-clear of the slot.

	dataOff := uint32(TagDataDbDataArea0)
	if n := len(reqs); n > 1 {
		dataOff += uint32(n-1) * TagDataDbReqStride
	}

	for i := range reqs {
		r := &reqs[i]
		reqStart := TagDataDbReq0Start + uint32(i)*TagDataDbReqStride

		// tag_name (max 254 bytes + NUL terminator at +0xFF; the
		// terminator stays 0 from the dispatcher's bulk-clear).
		tn := len(r.TagName)
		if tn > 254 {
			tn = 254
		}
		copy(slot[reqStart+ReqDbTagNameOff:reqStart+ReqDbTagNameOff+uint32(tn)], r.TagName)

		binary.LittleEndian.PutUint16(slot[reqStart+ReqDbActionOff:], r.Action)
		binary.LittleEndian.PutUint32(slot[reqStart+ReqDbDataTypeOff:], uint32(r.DataType))
		binary.LittleEndian.PutUint32(slot[reqStart+ReqDbElemByteSizeOff:], uint32(r.ElemByteSize))
		binary.LittleEndian.PutUint32(slot[reqStart+ReqDbElemCountOff:], uint32(r.ElemCount))
		// has_data and mask_seed stay 0 from the dispatcher's clear.

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

// DecodeAccessTagDataDb reads per-request result codes (at +0x120,
// not +0x118 as for AccessTagData) and copies inbound read data
// from the slot data area back into each request's Data buffer.
func DecodeAccessTagDataDb(slot []byte, reqs []TagRequest) {
	dataOff := uint32(TagDataDbDataArea0)
	if n := len(reqs); n > 1 {
		dataOff += uint32(n-1) * TagDataDbReqStride
	}
	for i := range reqs {
		r := &reqs[i]
		reqStart := TagDataDbReq0Start + uint32(i)*TagDataDbReqStride
		r.Result = binary.LittleEndian.Uint32(slot[reqStart+ReqDbResultOff:])
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
