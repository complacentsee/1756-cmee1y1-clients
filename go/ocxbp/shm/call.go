//go:build linux

// SPDX-License-Identifier: MIT

package shm

import (
	"encoding/binary"
	"fmt"
	"time"

	"golang.org/x/sys/unix"
)

// CallSpec describes one OCXcip_* round-trip. Mirrors
// bp_call_spec_t from c/src/proto.h.
//
// Fill is called after the header has been zero-initialised and
// written, with the slot's full byte slice. The callback writes the
// per-opcode request payload into the slot (typically at +0x78
// and beyond). May be nil for opcodes with no input.
//
// Read is called after the server has responded with errorcode == 0.
// Receives the slot byte slice; reads the per-opcode response.
// May be nil if no reply parsing is needed.
//
// TimeoutMs is the sem_timedwait deadline. 0 selects the C SDK
// default of 30 s.
type CallSpec struct {
	FnName      string
	PayloadSize uint32
	Fill        func(slot []byte)
	Read        func(slot []byte)
	TimeoutMs   int
}

// Call performs one slot reserve → header-fill → sem_post →
// sem_timedwait → parse cycle. Mirrors bp_client_call in
// c/src/client.c. Returns nil on success; transport failures wrap
// the named sentinels (ErrSendRequest / ErrRecvAnswer / ...) and
// engine-level non-zero errorcodes are returned as EngineError(n).
func (c *Client) Call(spec CallSpec) error {
	if c == nil || spec.FnName == "" {
		return ErrNullArg
	}

	tid := unix.Gettid()
	slotIdx, err := c.reserveSlot(tid)
	if err != nil {
		return err
	}
	defer c.releaseSlot(slotIdx)

	slot := c.Slot(slotIdx)

	// Header
	binary.LittleEndian.PutUint16(slot[HdrOpcode:], OpcodeCIP)
	binary.LittleEndian.PutUint16(slot[HdrOpcode+2:], 0)
	binary.LittleEndian.PutUint32(slot[HdrPayloadSize:], spec.PayloadSize)

	// fn_name: 64 zeros, then up to 63 bytes copied in (NUL stays at +0x47).
	for i := 0; i < 64; i++ {
		slot[HdrFnName+i] = 0
	}
	fnLen := len(spec.FnName)
	if fnLen > 63 {
		fnLen = 63
	}
	copy(slot[HdrFnName:HdrFnName+fnLen], spec.FnName)

	binary.LittleEndian.PutUint32(slot[HdrClientPID:], uint32(c.pid))
	binary.LittleEndian.PutUint16(slot[HdrIsDocker:], 1) // always 1 from this SDK
	binary.LittleEndian.PutUint16(slot[HdrIsDocker+2:], 0)
	binary.LittleEndian.PutUint32(slot[HdrErrorcode:], PendingErrorBits)
	binary.LittleEndian.PutUint32(slot[HdrErrorcode+4:], 0)
	// slot_owner already set by reserveSlot
	binary.LittleEndian.PutUint32(slot[HdrSlotNumber:], uint32(slotIdx))
	binary.LittleEndian.PutUint32(slot[HdrSlotNumber+4:], 0)
	// +0x68..+0x77: 16 reserved zeros
	for i := 0; i < 16; i++ {
		slot[0x68+i] = 0
	}
	// Zero the entire payload+response area so unwritten fields
	// don't read stale neighbor data on the response side.
	for i := HdrPayloadStart; i < SlotStride; i++ {
		slot[i] = 0
	}

	if spec.Fill != nil {
		spec.Fill(slot)
	}

	if err := c.semReq[slotIdx].Post(); err != nil {
		return fmt.Errorf("%w: %v", ErrSendRequest, err)
	}

	timeoutMs := spec.TimeoutMs
	if timeoutMs <= 0 {
		timeoutMs = 30000
	}
	deadline := time.Now().Add(time.Duration(timeoutMs) * time.Millisecond)

	if waitErr := c.semResp[slotIdx].TimedWaitAbs(deadline); waitErr != nil {
		// Fallback: poll the errorcode field for 200 ms. Mirrors the
		// same behavior in the C SDK — covers cases where the sem
		// was already consumed by a prior request but the engine
		// has written the response anyway.
		pollDeadline := time.Now().Add(200 * time.Millisecond)
		gotReply := false
		for time.Now().Before(pollDeadline) {
			ec := binary.LittleEndian.Uint32(slot[HdrErrorcode:])
			if ec != PendingErrorBits {
				gotReply = true
				break
			}
			time.Sleep(2 * time.Millisecond)
		}
		if !gotReply {
			return fmt.Errorf("%w: %v", ErrRecvAnswer, waitErr)
		}
	}

	errcode := int32(binary.LittleEndian.Uint32(slot[HdrErrorcode:]))
	if errcode != 0 {
		return EngineError(errcode)
	}

	if spec.Read != nil {
		spec.Read(slot)
	}
	return nil
}
