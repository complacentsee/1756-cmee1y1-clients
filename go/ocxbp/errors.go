// Package ocxbp is the public outbound-CIP-over-bpServer API for
// the 1756-CMEE1Y1 Embedded Edge Compute card. Mirrors the C SDK
// (libbpclient) shape-for-shape; uses idiomatic Go I/O (return
// values + error instead of out-param + rc).
//
// See docs/protocol.md for the wire-protocol spec and
// docs/container-plumbing.md for the required Docker flags.
//
// SPDX-License-Identifier: MIT
package ocxbp

import (
	"errors"
	"fmt"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// BP_ERR_* numeric codes — match the vendor OCX_ERR_* numbering.
// Most are surfaced as sentinel errors below; raw codes are kept for
// callers that need to compare with positive engine codes (e.g.
// MessageSend rc=1/3/14, TestTagDbVer 0x14/0x15).
const (
	BPOk             = 0
	BPErrGeneric     = -1
	BPErrSendRequest = -200
	BPErrRecvAnswer  = -201
	BPErrNullArg     = -300
	BPErrPending     = -301
	BPErrNotOpen     = -303
	BPErrParamRange  = -305
	BPErrSlotTooLarge = -311
	BPErrCIPStatus    = -400
	BPErrClientOpen   = -101802
	BPErrNoFreeSlot   = -103001
)

// Public sentinel errors. shm.* counterparts are wrapped where the
// transport surfaces them; engine codes flow through as EngineError.
var (
	ErrGeneric      = errors.New("ocxbp: generic failure")
	ErrSendRequest  = errors.New("ocxbp: sem_post on bpReq failed")
	ErrRecvAnswer   = errors.New("ocxbp: sem_wait on bpResp failed (server crashed?)")
	ErrNullArg      = errors.New("ocxbp: null argument")
	ErrPending      = errors.New("ocxbp: still pending (server hasn't replied)")
	ErrNotOpen      = errors.New("ocxbp: not open / Open() not called or IPC lost")
	ErrParamRange   = errors.New("ocxbp: parameter range error (check path string format: P:1,S:2 not 1,2)")
	ErrSlotTooLarge = errors.New("ocxbp: response too large for slot (reduce batch size)")
	ErrClientOpen   = errors.New("ocxbp: shm_open/mmap/sem_open failed (is bpServer running? --ipc=host set?)")
	ErrNoFreeSlot   = errors.New("ocxbp: all 16 slots in use (other clients holding slots)")
)

// EngineError carries a non-zero slot errorcode written by the
// bpServer engine. Use errors.As to extract the int code:
//
//	var ee ocxbp.EngineError
//	if errors.As(err, &ee) { ... ee.Code is the raw int ... }
type EngineError struct {
	Code int
}

func (e EngineError) Error() string {
	return fmt.Sprintf("ocxbp: engine errorcode %d (0x%x): %s",
		e.Code, uint32(e.Code), Strerror(e.Code))
}

// CIPError carries the structured fields of a CIP-layer rejection —
// the request transport succeeded but the PLC returned a non-zero
// general_status.  Use errors.As to extract:
//
//	var ce *ocxbp.CIPError
//	if errors.As(err, &ce) {
//	    if ce.Status == 0x01 && ce.ExtStatus == 0x0100 {
//	        // "Connection in use" — caller can retry after idle timeout
//	    }
//	}
//
// Service is the reply service byte (0xDB for LFO reply, 0xCE for
// FC reply, request_service|0x80 for other services).  Slot is the
// backplane slot that received the request.  ErrCode(*CIPError)
// returns BPErrCIPStatus (-400).
type CIPError struct {
	Service   uint8
	Status    uint8
	ExtStatus uint16
	Slot      uint8
}

func (e *CIPError) Error() string {
	return fmt.Sprintf("ocxbp: CIP rejection svc=0x%02x status=0x%02x ext=0x%04x slot=%d (%s)",
		e.Service, e.Status, e.ExtStatus, e.Slot,
		CIPStatusString(e.Status, e.ExtStatus))
}

// CIPStatusString returns a human-readable string for a (status,
// ext_status) pair.  Mirrors c/src/errors.c::bp_cip_status_string —
// keep in sync.
func CIPStatusString(status uint8, ext uint16) string {
	switch status {
	case 0x00:
		return "success"
	case 0x01:
		switch ext {
		case 0x0100:
			return "connection in use (stale conn from prior session — let PLC idle-time-out ~40s or restart bpServer)"
		case 0x0103:
			return "transport class unsupported (controller firmware rejected class 0xA3)"
		case 0x0107:
			return "connection ID not found in Forward_Close (PLC already cleaned up; safe to ignore on close)"
		case 0x0113:
			return "no more connections available on target"
		case 0x0114:
			return "vendor id or product code mismatch in Forward_Close"
		case 0x0115:
			return "device type mismatch in Forward_Close"
		case 0x0116:
			return "revision mismatch in Forward_Close"
		case 0x0117:
			return "non-listen-only connection not opened"
		case 0x0119:
			return "Forward_Close conn ID mismatch"
		case 0x011A:
			return "target application out of connections"
		case 0x0203:
			return "connection timeout"
		case 0x0204:
			return "Unconnected_Send timeout"
		case 0x0205:
			return "parameter error in Unconnected_Send"
		case 0x0206:
			return "message too large for Unconnected_Send"
		case 0x0311:
			return "port not available"
		case 0x0312:
			return "link address not available"
		case 0x0315:
			return "invalid segment type or value in path"
		case 0x0316:
			return "invalid attribute (connection path malformed)"
		case 0x0317:
			return "key segment not preceded by port segment"
		case 0x0318:
			return "link address to self invalid"
		default:
			return "connection failure"
		}
	case 0x02:
		return "resource unavailable (most often: conn_params requesting oversized buffer — try conn_params=0)"
	case 0x03:
		return "invalid parameter value"
	case 0x04:
		return "path segment error (bad tag name or EPATH)"
	case 0x05:
		return "path destination unknown (slot empty, or object doesn't accept this service)"
	case 0x06:
		return "partial transfer"
	case 0x07:
		return "connection lost"
	case 0x08:
		return "service not supported by target object"
	case 0x09:
		return "invalid attribute value"
	case 0x0A:
		return "attribute list error"
	case 0x0B:
		return "already in requested state"
	case 0x0C:
		return "object state conflict"
	case 0x0D:
		return "object already exists"
	case 0x0E:
		return "attribute not settable (write to read-only)"
	case 0x0F:
		return "privilege violation"
	case 0x10:
		return "device state conflict"
	case 0x11:
		return "reply data too large"
	case 0x12:
		return "fragmentation of primitive value"
	case 0x13:
		return "not enough data"
	case 0x14:
		return "attribute not supported"
	case 0x15:
		return "too much data"
	case 0x16:
		return "object does not exist"
	case 0x17:
		return "service fragmentation sequence not in progress"
	case 0x18:
		return "no stored attribute data"
	case 0x19:
		return "store operation failure"
	case 0x1A:
		return "routing failure: request packet too large"
	case 0x1B:
		return "routing failure: response packet too large"
	case 0x1C:
		return "missing attribute list entry data"
	case 0x1D:
		return "invalid attribute value list"
	case 0x1E:
		return "embedded service error"
	case 0x1F:
		return "vendor-specific error"
	case 0x20:
		return "invalid parameter"
	case 0x21:
		return "write-once value or medium already written"
	case 0x22:
		return "invalid reply received"
	case 0x25:
		return "key failure in path"
	case 0x26:
		return "path size invalid"
	case 0x27:
		return "unexpected attribute in list"
	case 0x28:
		return "invalid member id"
	case 0x29:
		return "member not settable"
	}
	return "unknown CIP status"
}

// translateCallErr maps shm-layer errors to the public sentinels +
// EngineError so callers don't need to import the shm package.
func translateCallErr(err error) error {
	if err == nil {
		return nil
	}
	var ee shm.EngineError
	if errors.As(err, &ee) {
		return EngineError{Code: int(ee)}
	}
	switch {
	case errors.Is(err, shm.ErrNullArg):
		return ErrNullArg
	case errors.Is(err, shm.ErrClientOpen):
		return fmt.Errorf("%w: %v", ErrClientOpen, err)
	case errors.Is(err, shm.ErrSendRequest):
		return fmt.Errorf("%w: %v", ErrSendRequest, err)
	case errors.Is(err, shm.ErrRecvAnswer):
		return fmt.Errorf("%w: %v", ErrRecvAnswer, err)
	case errors.Is(err, shm.ErrNoFreeSlot):
		return ErrNoFreeSlot
	}
	return err
}

// ErrCode reverse-maps an error from this package back to the
// integer rc the C SDK would have returned. Used by diagnostic
// tools (msgprobe etc.) that need to print an rc value that diffs
// byte-for-byte against the C tooling output.
//
// nil       → BPOk
// EngineError → its Code
// sentinels → the matching BP_ERR_* constant
// anything else (wrapped transport errors) → BPErrGeneric
func ErrCode(err error) int {
	if err == nil {
		return BPOk
	}
	var ee EngineError
	if errors.As(err, &ee) {
		return ee.Code
	}
	var ce *CIPError
	if errors.As(err, &ce) {
		return BPErrCIPStatus
	}
	switch {
	case errors.Is(err, ErrSendRequest):
		return BPErrSendRequest
	case errors.Is(err, ErrRecvAnswer):
		return BPErrRecvAnswer
	case errors.Is(err, ErrNullArg):
		return BPErrNullArg
	case errors.Is(err, ErrPending):
		return BPErrPending
	case errors.Is(err, ErrNotOpen):
		return BPErrNotOpen
	case errors.Is(err, ErrParamRange):
		return BPErrParamRange
	case errors.Is(err, ErrSlotTooLarge):
		return BPErrSlotTooLarge
	case errors.Is(err, ErrClientOpen):
		return BPErrClientOpen
	case errors.Is(err, ErrNoFreeSlot):
		return BPErrNoFreeSlot
	}
	return BPErrGeneric
}

// Strerror returns a human-readable string for a BP_ERR_* / engine
// code. Mirrors c/src/errors.c::bp_strerror.
func Strerror(rc int) string {
	switch rc {
	case BPOk:
		return "ok"
	case BPErrGeneric:
		return "generic failure"
	case BPErrSendRequest:
		return "sem_post on bpReq failed"
	case BPErrRecvAnswer:
		return "sem_wait on bpResp failed (server crashed?)"
	case BPErrNullArg:
		return "null argument"
	case BPErrPending:
		return "still pending (server hasn't replied — server crash?)"
	case BPErrNotOpen:
		return "not open / Open() not called or IPC lost"
	case BPErrParamRange:
		return "parameter range error (check path string format: P:1,S:2 not 1,2)"
	case BPErrSlotTooLarge:
		return "response too large for slot (reduce batch size)"
	case BPErrCIPStatus:
		return "CIP-layer rejection (errors.As(err, &CIPError) for details)"
	case BPErrClientOpen:
		return "shm_open/ftruncate/mmap failed (is bpServer running? --ipc=host set?)"
	case BPErrNoFreeSlot:
		return "all 16 slots in use (other clients holding slots)"
	}
	return "unknown error"
}
