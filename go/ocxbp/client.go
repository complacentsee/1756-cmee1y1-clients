// SPDX-License-Identifier: MIT

package ocxbp

import (
	"sync"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// txrxState tracks one open class-3 connection.  Lookup is by
// app_handle (the caller's key in cip.ConnSpec).  Lifecycle: created
// in TxRxOpen, mutated by TxRxMsg (sequence counter for diagnostics),
// removed in TxRxClose.
type txrxState struct {
	slot       uint8
	connSerial uint16
	vendorID   uint16
	origSerial uint32
	otConnID   uint32
	toConnID   uint32
	sequence   uint16 // diagnostic only — NOT on the wire
}

// Client is the public outbound-CIP handle. Wraps the IPC shm.Client
// and the OCXcip_Open session handle (which is bookkeeping — the
// wrapper tracks state via slot ownership, not the handle).
type Client struct {
	shm     *shm.Client
	txrxMu  sync.Mutex
	txrxMap map[uint16]*txrxState
	poolsMu sync.Mutex
	pools   [PoolMaxSlots]*pool
	// v0.9.0 per-PLC symbol cache, keyed on the OldI CIP path.  Shared
	// across all TagDB handles to the same path on this client.  See
	// tagcache.go for the cache shape and lookup semantics.
	tagCacheMu sync.Mutex
	tagCaches  map[string]*tagCache
}

// Open maps /dev/shm/bpShmem and opens the 33 POSIX named semaphores
// (/bpShm + 16×/bpReqNN + 16×/bpRespNN). Sends no wire request;
// call OpenSession to make the first OCXcip_* dispatch.
//
// Container plumbing required: --ipc=host --pid=host -v /dev/shm:/dev/shm.
func Open() (*Client, error) {
	s, err := shm.Open()
	if err != nil {
		return nil, translateCallErr(err)
	}
	return &Client{
		shm:       s,
		txrxMap:   make(map[uint16]*txrxState),
		tagCaches: make(map[string]*tagCache),
	}, nil
}

// Close releases the IPC. Safe to call on nil.  Any open pools are
// closed first (with their Forward_Close sends to the PLC and their
// keepalive goroutines stopped), then the engine session is released
// via OCXcip_Close (so bpServer's session table doesn't accumulate
// dead entries), then the IPC layer is torn down.
func (c *Client) Close() {
	if c == nil {
		return
	}
	for s := 0; s < PoolMaxSlots; s++ {
		c.poolsMu.Lock()
		p := c.pools[s]
		c.poolsMu.Unlock()
		if p != nil {
			_ = c.PoolClose(uint8(s))
		}
	}
	// Best-effort — if no session was open the engine returns
	// ErrNotOpen which we discard.
	_ = c.CloseSession()
	c.shm.Close()
	c.shm = nil
}

// SHM returns the underlying shm.Client. Exposed for diagnostic
// tools (cmd/pathprobe etc.) that need to issue opcodes not in the
// public ocxbp API. Application code should not need this.
func (c *Client) SHM() *shm.Client {
	if c == nil {
		return nil
	}
	return c.shm
}

// OpenSession dispatches OCXcip_Open and returns the server-assigned
// session handle. Must succeed before any other call works.
func (c *Client) OpenSession() (uint32, error) {
	if c == nil {
		return 0, ErrNullArg
	}
	var handle uint32
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnOpen,
		PayloadSize: cip.SizeOpen,
		Read:        func(slot []byte) { handle = cip.DecodeOpenSession(slot) },
		TimeoutMs:   5000,
	})
	return handle, translateCallErr(err)
}

// ParsedPath is the public result struct for Client.ParsePath.
// Mirrors C bp_parsed_path_t.
type ParsedPath struct {
	Encoded       []byte // binary EPATH the engine generated
	CIPClass      uint16
	SegmentFlags  uint8
	Instance      uint32
	AttrFlags     uint8
}

// ParsePath dispatches OCXcip_ParsePath against `text` (OldI format,
// e.g. "P:1,S:2,C:1,I:1,A:1") and returns the parsed result.  Use it
// to validate path syntax at the SDK boundary instead of as a PLC-side
// CIP rejection.  Returns an error wrapping engine code -101 on a
// malformed path.
func (c *Client) ParsePath(text string) (ParsedPath, error) {
	if c == nil || text == "" {
		return ParsedPath{}, ErrNullArg
	}
	if len(text) > 254 {
		return ParsedPath{}, ErrParamRange
	}
	var raw cip.ParsePathResult
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnParsePath,
		PayloadSize: cip.SizeParsePath,
		Fill:        func(slot []byte) { cip.EncodeParsePath(slot, text, 256) },
		Read:        func(slot []byte) { raw = cip.DecodeParsePath(slot) },
		TimeoutMs:   5000,
	})
	if err != nil {
		return ParsedPath{}, translateCallErr(err)
	}
	return ParsedPath{
		Encoded:      raw.Encoded,
		CIPClass:     raw.Class,
		SegmentFlags: raw.SegFlags,
		Instance:     raw.Instance,
		AttrFlags:    raw.AttrFlags,
	}, nil
}

// Reconnect re-establishes the IPC after a bpServer restart.
// INVALIDATES EVERYTHING the caller held: open pools, tagdbs, and
// symbol caches are all wiped before the IPC restart.  Callers must
// call OpenSession again and re-open any pools / tag databases after
// a successful Reconnect.
//
// Returns ErrClientOpen if any sem/shm reopen fails (typically
// bpServer not running yet).
func (c *Client) Reconnect() error {
	if c == nil {
		return ErrNullArg
	}
	// Tear down caller-held state.
	for s := 0; s < PoolMaxSlots; s++ {
		c.poolsMu.Lock()
		p := c.pools[s]
		c.poolsMu.Unlock()
		if p != nil {
			_ = c.PoolClose(uint8(s))
		}
	}
	c.tagCacheMu.Lock()
	c.tagCaches = make(map[string]*tagCache)
	c.tagCacheMu.Unlock()
	c.txrxMu.Lock()
	c.txrxMap = make(map[uint16]*txrxState)
	c.txrxMu.Unlock()

	return translateCallErr(c.shm.Reconnect())
}

// CloseSession dispatches OCXcip_Close to release the engine-side
// session opened by OpenSession.  Client.Close calls this
// automatically; explicit invocation is only needed if you want to
// keep the SDK's IPC handle alive across multiple sessions.
func (c *Client) CloseSession() error {
	if c == nil {
		return ErrNullArg
	}
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnClose,
		PayloadSize: cip.SizeClose,
		TimeoutMs:   5000,
	})
	return translateCallErr(err)
}
