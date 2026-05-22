// SPDX-License-Identifier: MIT

package ocxbp

import (
	"errors"
	"fmt"
	"os"
	"sync"
	"sync/atomic"
	"time"
)

// PoolSpec configures a per-slot connection pool.  Mirrors C's
// bp_pool_spec_t and Python's PoolSpec.
//
// Recommended defaults: Size=4, KeepaliveMs=10000, ConnParams=0.
type PoolSpec struct {
	Slot        uint8  // backplane slot 0..0x13
	Size        uint8  // connections to keep open (1..PoolMaxSize)
	KeepaliveMs uint16 // idle ping interval; 0 = disabled
	ConnParams  uint16 // O→T/T→O size in bytes; 0 = SDK default 4000
}

// pool is the internal per-slot pool state.  One per slot per Client;
// indexed by slot in Client.pools.
type pool struct {
	slot        uint8
	size        uint8
	keepaliveMs uint16
	connParams  uint16
	entries     []*poolEntry
	free        chan int          // buffered; values are entry indices
	stop        chan struct{}     // closed by PoolClose
	wg          sync.WaitGroup    // tracks keepalive goroutine
	inflight    sync.WaitGroup    // tracks in-flight PoolTxRx
	initialized atomic.Bool
}

type poolEntry struct {
	appHandle uint16
	lastUsed  atomic.Int64 // unix-nanoseconds; updated on every release
	dead      atomic.Bool
	// v0.9.0 Phase 4 — auto-reopen bookkeeping.  Keepalive retries a
	// dead entry with exponential backoff (1s → 2s → 4s → ... cap 30s).
	lastReopenNs   atomic.Int64
	reopenBackoffMs atomic.Int64
}

// poolIdentityReq is the keepalive ping body — Identity Object
// GetAttributeAll on class 0x01 instance 1.  Same bytes the sibling
// apex2d daemon uses (apex2_cip_connection.c:3301-3303).
var poolIdentityReq = []byte{0x01, 0x02, 0x20, 0x01, 0x24, 0x01}

// PoolOpen pre-opens spec.Size class-3 connections to spec.Slot and
// starts a keepalive goroutine if KeepaliveMs > 0.  Only one pool per
// slot per Client; reopening without PoolClose first returns an error.
//
// On partial failure (some opened, some not), the partial state is
// rolled back (Forward_Close on each opened entry) and the underlying
// error is returned.
func (c *Client) PoolOpen(spec *PoolSpec) error {
	if c == nil || spec == nil {
		return ErrNullArg
	}
	if spec.Slot > MsgMaxSlot {
		return ErrSlotTooLarge
	}
	if spec.Size < 1 || spec.Size > PoolMaxSize {
		return ErrParamRange
	}

	c.poolsMu.Lock()
	if c.pools[spec.Slot] != nil {
		c.poolsMu.Unlock()
		return fmt.Errorf("ocxbp: pool already open for slot %d (PoolClose first)", spec.Slot)
	}
	p := &pool{
		slot:        spec.Slot,
		size:        spec.Size,
		keepaliveMs: spec.KeepaliveMs,
		connParams:  spec.ConnParams,
		entries:     make([]*poolEntry, spec.Size),
		free:        make(chan int, int(spec.Size)),
		stop:        make(chan struct{}),
	}
	c.pools[spec.Slot] = p
	c.poolsMu.Unlock()

	// Open the underlying conns sequentially.  Parallelizing would
	// save a few ms at open but each LFO mutates shared PLC state; the
	// sibling apex2d daemon also opens its pool sequentially.
	now := time.Now().UnixNano()
	openedIdxs := make([]int, 0, int(spec.Size))
	var openErr error
	epath := []byte{0x01, spec.Slot}
	for i := 0; i < int(spec.Size); i++ {
		appHandle := poolAppHandleBase | (uint16(spec.Slot) << 8) | uint16(i)
		cs := &ConnSpec{
			AppHandle:   appHandle,
			EncodedPath: epath,
			PathSize:    2,
			ConnParams:  spec.ConnParams,
		}
		if _, _, err := c.TxRxOpen(cs); err != nil {
			fmt.Fprintf(os.Stderr,
				"[PoolOpen] slot=%d entry %d/%d open failed: %v\n",
				spec.Slot, i, spec.Size, err)
			openErr = err
			break
		}
		p.entries[i] = &poolEntry{appHandle: appHandle}
		p.entries[i].lastUsed.Store(now)
		openedIdxs = append(openedIdxs, i)
	}

	if openErr != nil {
		// Roll back partial open.
		for _, i := range openedIdxs {
			cs := &ConnSpec{
				AppHandle:   p.entries[i].appHandle,
				EncodedPath: epath,
				PathSize:    2,
				ConnParams:  spec.ConnParams,
			}
			_ = c.TxRxClose(cs)
		}
		c.poolsMu.Lock()
		c.pools[spec.Slot] = nil
		c.poolsMu.Unlock()
		return openErr
	}

	// Seed the free channel with all entry indices in order.
	for i := 0; i < int(spec.Size); i++ {
		p.free <- i
	}
	p.initialized.Store(true)

	if spec.KeepaliveMs > 0 {
		p.wg.Add(1)
		go p.keepaliveLoop(c)
	}
	return nil
}

// PoolTxRx sends one CIP request via a pool connection to slot.  The
// pool picks the next free entry and routes the request through it
// via the same UCMM transport as TxRxMsg.  Blocks if all entries are
// in flight; returns ErrNotOpen if no pool exists or it's closing.
func (c *Client) PoolTxRx(slot uint8, req []byte, resp []byte, respCap uint16) (uint16, error) {
	if c == nil || len(req) == 0 || resp == nil || respCap == 0 {
		return 0, ErrNullArg
	}
	if slot > MsgMaxSlot {
		return 0, ErrSlotTooLarge
	}
	c.poolsMu.Lock()
	p := c.pools[slot]
	c.poolsMu.Unlock()
	if p == nil || !p.initialized.Load() {
		return 0, ErrNotOpen
	}

	p.inflight.Add(1)
	defer p.inflight.Done()

	// Acquire a non-dead entry.  If we pull a dead entry from the
	// free channel, put it back (the keepalive goroutine will
	// auto-reopen it) and try again.  Cap retries at size to avoid
	// spinning when all entries are dead.
	var idx int
	for attempt := 0; ; attempt++ {
		select {
		case idx = <-p.free:
		case <-p.stop:
			return 0, ErrNotOpen
		}
		if !p.entries[idx].dead.Load() {
			break
		}
		// Return the dead entry to free for the keepalive thread.
		select {
		case p.free <- idx:
		case <-p.stop:
			return 0, ErrNotOpen
		}
		if attempt+1 >= int(p.size) {
			return 0, fmt.Errorf("ocxbp: PoolTxRx: all %d pool entries are dead (keepalive auto-reopen still in backoff)", p.size)
		}
	}
	defer func() {
		p.entries[idx].lastUsed.Store(time.Now().UnixNano())
		// Try to return; if pool is closing the receive on stop wins.
		select {
		case p.free <- idx:
		case <-p.stop:
		}
	}()

	epath := []byte{0x01, slot}
	cs := &ConnSpec{
		AppHandle:   p.entries[idx].appHandle,
		EncodedPath: epath,
		PathSize:    2,
		ConnParams:  p.connParams,
	}
	got, err := c.TxRxMsg(cs, req, resp, respCap)
	return got, err
}

// BatchItem is one request/response slot in a PoolBatch call.
// Caller fills Req; the pool fills Resp + Err on completion.  Order
// of dispatch is unspecified but each result lands back in its
// original slot.
type BatchItem struct {
	Req  []byte
	Resp []byte // populated on success; len() == response size
	Err  error
}

// PoolBatch concurrently dispatches len(items) requests through the
// pool for slot, blocking until all complete.  Spawns
// min(pool.size, len(items)) worker goroutines.  Returns nil if every
// item succeeded; otherwise returns an error wrapping the first
// non-nil item Err (caller can iterate items to find all failures).
//
// Each item's Resp buffer is allocated by PoolBatch with capacity
// respCap.  Pass a respCap large enough for the biggest expected
// reply.
func (c *Client) PoolBatch(slot uint8, items []BatchItem, respCap uint16) error {
	if c == nil || items == nil {
		return ErrNullArg
	}
	if slot > MsgMaxSlot {
		return ErrSlotTooLarge
	}
	if len(items) == 0 {
		return nil
	}
	c.poolsMu.Lock()
	p := c.pools[slot]
	c.poolsMu.Unlock()
	if p == nil || !p.initialized.Load() {
		return ErrNotOpen
	}

	workerCount := int(p.size)
	if workerCount > len(items) {
		workerCount = len(items)
	}

	var idx atomic.Int64
	var wg sync.WaitGroup
	wg.Add(workerCount)
	for w := 0; w < workerCount; w++ {
		go func() {
			defer wg.Done()
			for {
				i := int(idx.Add(1)) - 1
				if i >= len(items) {
					return
				}
				resp := make([]byte, respCap)
				got, err := c.PoolTxRx(slot, items[i].Req, resp, respCap)
				if err == nil {
					items[i].Resp = resp[:got]
				} else {
					items[i].Resp = nil
				}
				items[i].Err = err
			}
		}()
	}
	wg.Wait()

	for i := range items {
		if items[i].Err != nil {
			return fmt.Errorf("ocxbp: PoolBatch: item %d failed: %w", i, items[i].Err)
		}
	}
	return nil
}

// PoolClose stops the keepalive goroutine, sends Forward_Close on
// every pool entry, and frees the pool state.  In-flight calls
// complete first; subsequent PoolTxRx on this slot returns ErrNotOpen.
// Idempotent: closing a non-existent pool returns nil.
func (c *Client) PoolClose(slot uint8) error {
	if c == nil {
		return ErrNullArg
	}
	if slot >= PoolMaxSlots {
		return ErrSlotTooLarge
	}
	c.poolsMu.Lock()
	p := c.pools[slot]
	if p == nil {
		c.poolsMu.Unlock()
		return nil
	}
	c.pools[slot] = nil
	c.poolsMu.Unlock()

	p.initialized.Store(false)
	close(p.stop)
	p.wg.Wait()       // keepalive goroutine exits
	p.inflight.Wait() // in-flight PoolTxRx finishes

	epath := []byte{0x01, slot}
	for i := 0; i < int(p.size); i++ {
		e := p.entries[i]
		if e == nil || e.appHandle == 0 {
			continue
		}
		cs := &ConnSpec{
			AppHandle:   e.appHandle,
			EncodedPath: epath,
			PathSize:    2,
			ConnParams:  p.connParams,
		}
		// Ignore close errors — PLC may have idle-timed-out and the
		// SDK frees its own cached state regardless of FC outcome.
		// Treat the missing-handle error from TxRxClose as benign.
		if err := c.TxRxClose(cs); err != nil && !errors.Is(err, ErrNotOpen) {
			// Already logged inside TxRxClose for CIP-layer failures.
		}
	}
	return nil
}

// keepaliveLoop pings idle pool entries with Identity GAA on a fixed
// cadence.  Mirrors C pool_keepalive_loop and apex2d's
// slot_pool_keepalive_idle.  Exits on close(p.stop).
func (p *pool) keepaliveLoop(c *Client) {
	defer p.wg.Done()
	intervalMs := int64(p.keepaliveMs) / 2
	if intervalMs < 500 {
		intervalMs = 500
	}
	if intervalMs > 5000 {
		intervalMs = 5000
	}
	ticker := time.NewTicker(time.Duration(intervalMs) * time.Millisecond)
	defer ticker.Stop()

	for {
		select {
		case <-p.stop:
			return
		case <-ticker.C:
		}

		now := time.Now().UnixNano()
		idleNs := int64(p.keepaliveMs) * int64(time.Millisecond)
		for i := 0; i < int(p.size); i++ {
			e := p.entries[i]
			if e == nil || e.dead.Load() {
				continue
			}
			last := e.lastUsed.Load()
			if now-last < idleNs {
				continue
			}
			// Try to acquire this specific entry; if it's currently
			// borrowed by a PoolTxRx, skip it (real traffic does the
			// same job as a ping).  We claim by draining one slot from
			// the free channel; if we get a different index back we
			// put it back and skip.
			var claimed int = -1
			select {
			case got := <-p.free:
				if got == i {
					claimed = i
				} else {
					// Wrong entry — return it; another keepalive tick
					// will catch entry i.
					select {
					case p.free <- got:
					case <-p.stop:
						return
					}
				}
			default:
				// All entries in use; skip this round.
			}
			if claimed < 0 {
				continue
			}

			epath := []byte{0x01, p.slot}
			cs := &ConnSpec{
				AppHandle:   e.appHandle,
				EncodedPath: epath,
				PathSize:    2,
				ConnParams:  p.connParams,
			}
			respBuf := make([]byte, 64)
			_, err := c.TxRxMsg(cs, poolIdentityReq, respBuf, uint16(len(respBuf)))
			e.lastUsed.Store(time.Now().UnixNano())
			if err != nil {
				e.dead.Store(true)
				// Initial backoff = 1 s.  Auto-reopen pass (below) takes
				// over from here.
				e.lastReopenNs.Store(time.Now().UnixNano())
				if e.reopenBackoffMs.Load() == 0 {
					e.reopenBackoffMs.Store(1000)
				}
				fmt.Fprintf(os.Stderr,
					"[pool keepalive] slot=%d entry %d ping failed: %v — entry marked dead\n",
					p.slot, i, err)
			}
			// Return entry to free pool.
			select {
			case p.free <- claimed:
			case <-p.stop:
				return
			}
		}

		// v0.9.0 Phase 4: auto-reopen pass.  For each dead entry whose
		// backoff has elapsed, force-close the stale local conn state
		// and re-issue Forward_Open with the same app_handle.  On
		// success: dead=false, backoff resets, free-channel gets the
		// entry back so a waiting PoolTxRx picks it up.  On failure:
		// backoff doubles, capped at 30 s.
		for i := 0; i < int(p.size); i++ {
			e := p.entries[i]
			if e == nil || !e.dead.Load() {
				continue
			}
			now := time.Now().UnixNano()
			sinceMs := (now - e.lastReopenNs.Load()) / int64(time.Millisecond)
			if sinceMs < e.reopenBackoffMs.Load() {
				continue
			}
			// Claim the entry slot in free so a parallel PoolTxRx
			// doesn't grab it mid-reopen.  A dead entry is normally
			// already not in `free` (PoolTxRx returns it to free even
			// when dead, but takes the dead-skip path on next acquire);
			// be defensive and try a non-blocking receive — if we can't
			// claim it, skip this round.
			var claimed int = -1
			select {
			case got := <-p.free:
				if got == i {
					claimed = i
				} else {
					select {
					case p.free <- got:
					case <-p.stop:
						return
					}
				}
			default:
				// Entry not in free queue (race).  Defer to next tick.
			}
			if claimed < 0 {
				continue
			}
			e.lastReopenNs.Store(now)

			// Force-close the stale local conn-state slot; the PLC has
			// already dropped this conn so no Forward_Close on the wire.
			c.forceCloseLocal(e.appHandle)

			epath := []byte{0x01, p.slot}
			cs := &ConnSpec{
				AppHandle:   e.appHandle,
				EncodedPath: epath,
				PathSize:    2,
				ConnParams:  p.connParams,
			}
			_, _, orc := c.TxRxOpen(cs)
			if orc == nil {
				e.dead.Store(false)
				e.lastUsed.Store(time.Now().UnixNano())
				e.reopenBackoffMs.Store(1000)
				fmt.Fprintf(os.Stderr,
					"[pool keepalive] slot=%d entry %d auto-reopen OK\n",
					p.slot, i)
			} else {
				next := e.reopenBackoffMs.Load() * 2
				if next > 30000 {
					next = 30000
				}
				e.reopenBackoffMs.Store(next)
				fmt.Fprintf(os.Stderr,
					"[pool keepalive] slot=%d entry %d auto-reopen failed: %v — next attempt in %d ms\n",
					p.slot, i, orc, next)
			}
			// Return the entry to free regardless.  If dead, PoolTxRx
			// would skip via the dead check; but we don't have that
			// check today (the channel just hands out indices).  For
			// v0.9.0 simplicity we accept that PoolTxRx may briefly
			// hand back a still-dead entry and let TxRxMsg surface the
			// real error.  TODO if this becomes noisy: gate PoolTxRx
			// on entry.dead.Load().
			select {
			case p.free <- claimed:
			case <-p.stop:
				return
			}
		}
	}
}
