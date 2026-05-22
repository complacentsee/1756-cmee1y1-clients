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
