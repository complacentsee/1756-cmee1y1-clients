// SPDX-License-Identifier: MIT

package ocxbp

import (
	"fmt"
	"math/rand"
	"os"
	"sync"
	"time"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
)

// ConnSpec is the public class-3 connection descriptor.  Mirrors
// cip.ConnSpec / bp_conn_spec_t / Python ConnSpec.
type ConnSpec = cip.ConnSpec

// Max concurrent class-3 connections per Client.  Matches
// C SDK BP_TXRX_MAX_CONNS.
const TxRxMaxConns = 16

// Per-process serial generator state.  We don't reseed for each
// open — one seed at first use is fine since the PLC removes
// serials on Forward_Close and on idle timeout, so collisions are
// rare and self-healing.
var (
	serialOnce sync.Once
	serialRng  *rand.Rand
)

func nextConnSerial() uint16 {
	serialOnce.Do(func() {
		seed := time.Now().UnixNano() ^ int64(os.Getpid())
		serialRng = rand.New(rand.NewSource(seed))
	})
	v := uint16(serialRng.Uint32() & 0xFFFF)
	if v == 0 {
		v = 0xBEEF
	}
	return v
}

func nextOrigSerial() uint32 {
	return uint32(os.Getpid()) ^ (uint32(time.Now().Unix()) << 16)
}

// TxRxOpen establishes a class-3 connection by sending a Large
// Forward Open (CIP svc 0x5B) via MessageSend.  Caches the
// connection state on this Client keyed by spec.AppHandle.  See
// docs/protocol.md "Connected messaging — wire format".
//
// Returns:
//   - connID: low 16 of the PLC-assigned O→T conn ID
//   - connSerial: the random 16-bit serial we sent in the LFO
//     (echoed by the PLC's Forward_Close reply)
//
// spec.ConnParams, if non-zero, sets O→T / T→O size in BYTES;
// 0 → 4000 (matches OCX).  Hardware max 4002; larger values are
// capped with a warning.
//
// v0.7.0 only supports the canonical backplane-direct route:
// EncodedPath = {0x01, slot}, PathSize = 2.
func (c *Client) TxRxOpen(spec *ConnSpec) (connID, connSerial uint16, err error) {
	if c == nil || spec == nil || spec.EncodedPath == nil {
		return 0, 0, ErrNullArg
	}
	slot, ok := cip.ExtractSlot(spec.EncodedPath, spec.PathSize)
	if !ok {
		return 0, 0, ErrParamRange
	}
	if slot > MsgMaxSlot {
		return 0, 0, ErrSlotTooLarge
	}

	otSize := cip.LFODefaultOTSize
	if spec.ConnParams != 0 {
		otSize = spec.ConnParams
	}
	if otSize > cip.LFOMaxOTSize {
		fmt.Fprintf(os.Stderr,
			"[TxRxOpen] conn_params=%d exceeds LFO max %d; capping "+
				"(caller probably passed a stale OEM 16-bit param)\n",
			otSize, cip.LFOMaxOTSize)
		otSize = cip.LFOMaxOTSize
	}

	cs := nextConnSerial()
	os_ := nextOrigSerial()

	lfo := make([]byte, 64)
	lfoLen := cip.BuildForwardOpen(lfo, cs, os_, otSize)

	resp := make([]byte, 64)
	msg := &Message{
		Slot:         slot,
		CipRequest:   lfo[:lfoLen],
		ReqSize:      uint16(lfoLen),
		TimeoutMs:    5000,
		RespData:     resp,
		RespCapacity: uint16(len(resp)),
	}
	if err := c.MessageSend(msg); err != nil {
		return 0, 0, err
	}

	otConnID, toConnID, status, lfoOk := cip.ParseForwardOpen(resp[:msg.RespLen])
	if !lfoOk {
		ext := uint16(0)
		if msg.RespLen >= 6 && resp[3] != 0 {
			ext = uint16(resp[4]) | (uint16(resp[5]) << 8)
		}
		svc := uint8(0)
		if msg.RespLen >= 1 {
			svc = resp[0]
		}
		ce := &CIPError{Service: svc, Status: status, ExtStatus: ext, Slot: slot}
		fmt.Fprintf(os.Stderr,
			"[TxRxOpen] LFO CIP failure: svc=0x%02x status=0x%02x ext=0x%04x slot=%d (%s)\n",
			svc, status, ext, slot, CIPStatusString(status, ext))
		return 0, 0, ce
	}

	c.txrxMu.Lock()
	if existing, dup := c.txrxMap[spec.AppHandle]; dup {
		// Already open under this handle — drop the new connection
		// with a best-effort FC so we don't leak PLC state.
		c.txrxMu.Unlock()
		fmt.Fprintf(os.Stderr,
			"[TxRxOpen] app_handle=%d already open (slot=%d, serial=0x%04x) "+
				"— call TxRxClose first\n",
			spec.AppHandle, existing.slot, existing.connSerial)
		c.bestEffortClose(slot, cs, cip.LFOVendorID, os_)
		return 0, 0, ErrGeneric
	}
	if len(c.txrxMap) >= TxRxMaxConns {
		c.txrxMu.Unlock()
		c.bestEffortClose(slot, cs, cip.LFOVendorID, os_)
		return 0, 0, ErrNoFreeSlot
	}
	c.txrxMap[spec.AppHandle] = &txrxState{
		slot:       slot,
		connSerial: cs,
		vendorID:   cip.LFOVendorID,
		origSerial: os_,
		otConnID:   otConnID,
		toConnID:   toConnID,
	}
	c.txrxMu.Unlock()

	return uint16(otConnID & 0xFFFF), cs, nil
}

// bestEffortClose sends a Forward_Close without caring about the
// outcome — used to clean up after TxRxOpen failure paths so the
// PLC's connection table doesn't accumulate orphans.
func (c *Client) bestEffortClose(slot uint8, connSerial, vendorID uint16, origSerial uint32) {
	fc := make([]byte, 32)
	fcLen := cip.BuildForwardClose(fc, connSerial, vendorID, origSerial)
	resp := make([]byte, 64)
	msg := &Message{
		Slot:         slot,
		CipRequest:   fc[:fcLen],
		ReqSize:      uint16(fcLen),
		TimeoutMs:    5000,
		RespData:     resp,
		RespCapacity: uint16(len(resp)),
	}
	_ = c.MessageSend(msg)
}

// TxRxMsg sends one CIP request over the connection identified by
// spec.AppHandle.  Returns ErrNotOpen if no matching TxRxOpen was
// made.  req is sent byte-for-byte (no sequence-number prepending
// — see docs/protocol.md "Sequence numbers are NOT prepended").
//
// v0.7.0 cap: len(req) ≤ MsgMaxReqSize (500 bytes).  Larger payloads
// require the v0.8 large-buffer transport.
func (c *Client) TxRxMsg(spec *ConnSpec, req []byte, resp []byte, respCap uint16) (uint16, error) {
	if c == nil || spec == nil {
		return 0, ErrNullArg
	}
	if len(req) == 0 || resp == nil || respCap == 0 {
		return 0, ErrNullArg
	}

	c.txrxMu.Lock()
	state, ok := c.txrxMap[spec.AppHandle]
	if !ok {
		c.txrxMu.Unlock()
		return 0, ErrNotOpen
	}
	slot := state.slot
	state.sequence++ // diagnostic only
	c.txrxMu.Unlock()

	msg := &Message{
		Slot:         slot,
		CipRequest:   req,
		ReqSize:      uint16(len(req)),
		TimeoutMs:    5000,
		RespData:     resp,
		RespCapacity: respCap,
	}
	if err := c.MessageSend(msg); err != nil {
		return 0, err
	}
	return msg.RespLen, nil
}

// TxRxClose releases the connection.  Sends Forward_Close using the
// cached identifiers, then evicts the state from the Client's
// connection map regardless of FC outcome (so the slot becomes
// available for re-open even if the PLC had already cleaned up by
// idle timeout).  Returns ErrNotOpen if no matching state exists.
func (c *Client) TxRxClose(spec *ConnSpec) error {
	if c == nil || spec == nil {
		return ErrNullArg
	}

	c.txrxMu.Lock()
	state, ok := c.txrxMap[spec.AppHandle]
	if !ok {
		c.txrxMu.Unlock()
		return ErrNotOpen
	}
	slot := state.slot
	connSerial := state.connSerial
	vendorID := state.vendorID
	origSerial := state.origSerial
	delete(c.txrxMap, spec.AppHandle)
	c.txrxMu.Unlock()

	fc := make([]byte, 32)
	fcLen := cip.BuildForwardClose(fc, connSerial, vendorID, origSerial)
	resp := make([]byte, 64)
	msg := &Message{
		Slot:         slot,
		CipRequest:   fc[:fcLen],
		ReqSize:      uint16(fcLen),
		TimeoutMs:    5000,
		RespData:     resp,
		RespCapacity: uint16(len(resp)),
	}
	if err := c.MessageSend(msg); err != nil {
		return err
	}

	if _, ok := cip.ParseForwardClose(resp[:msg.RespLen]); !ok {
		svc := uint8(0)
		st := uint8(0xFF)
		ext := uint16(0)
		if msg.RespLen >= 1 {
			svc = resp[0]
		}
		if msg.RespLen >= 3 {
			st = resp[2]
		}
		if msg.RespLen >= 6 && resp[3] != 0 {
			ext = uint16(resp[4]) | (uint16(resp[5]) << 8)
		}
		ce := &CIPError{Service: svc, Status: st, ExtStatus: ext, Slot: slot}
		fmt.Fprintf(os.Stderr,
			"[TxRxClose] FC CIP failure: svc=0x%02x status=0x%02x ext=0x%04x slot=%d serial=0x%04x (%s)\n",
			svc, st, ext, slot, connSerial, CIPStatusString(st, ext))
		return ce
	}
	return nil
}
