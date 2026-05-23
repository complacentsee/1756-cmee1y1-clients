// SPDX-License-Identifier: MIT

package ocxbp

import (
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// AccessDb (v0.10.4+) performs one OCXcip_AccessTagDataDb call with
// `reqs` requests. Same shape as Access, but routes through the
// cached db_handle from CreateTagDbHandle instead of re-sending the
// path string per call — saves ~251 bytes of wire and the engine's
// per-call path parse.
//
// Per-tag CIP General Status lands in each request's Result field
// exactly as Access populates it; the slot-level errorcode is
// returned via the error. New code that doesn't need byte-identical
// behavior with the OLD path-string-based call should prefer this
// function; Access stays available for callers that need the
// AccessTagData semantics (e.g. cross-validation tests).
func (db *TagDB) AccessDb(reqs []TagRequest) error {
	if db == nil || len(reqs) == 0 {
		return ErrNullArg
	}
	if len(reqs) > 16 {
		return ErrParamRange
	}
	payloadSize := cip.AccessTagDataDbPayloadSize(reqs)
	if payloadSize > shm.SlotStride-0x80 {
		return ErrSlotTooLarge
	}
	handle := db.handle
	err := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnAccessTagDataDb,
		PayloadSize: payloadSize,
		Fill:        func(slot []byte) { cip.EncodeAccessTagDataDb(slot, handle, reqs) },
		Read:        func(slot []byte) { cip.DecodeAccessTagDataDb(slot, reqs) },
		TimeoutMs:   10000,
	})
	return translateCallErr(err)
}
