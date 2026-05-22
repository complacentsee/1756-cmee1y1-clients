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
	case BPErrClientOpen:
		return "shm_open/ftruncate/mmap failed (is bpServer running? --ipc=host set?)"
	case BPErrNoFreeSlot:
		return "all 16 slots in use (other clients holding slots)"
	}
	return "unknown error"
}
