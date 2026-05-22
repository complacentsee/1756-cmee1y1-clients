//go:build linux

// SPDX-License-Identifier: MIT

package shm

import (
	"encoding/binary"
	"errors"
	"fmt"
	"os"
	"sync"

	"golang.org/x/sys/unix"
)

// Errors surfaced by the shm layer. Engine-level errorcodes are
// returned as ErrEngine; transport-level failures use the named
// values below. These mirror the C SDK's BP_ERR_* numbering for
// the codes that originate above the wire (transport, slot mgmt).
var (
	ErrNullArg     = errors.New("shm: null argument")
	ErrClientOpen  = errors.New("shm: open/mmap/sem_open failed")
	ErrSendRequest = errors.New("shm: sem_post on bpReq failed")
	ErrRecvAnswer  = errors.New("shm: sem_wait on bpResp failed (server crashed?)")
	ErrNoFreeSlot  = errors.New("shm: all 16 slots in use")
)

// EngineError carries an int errorcode that the bpServer wrote into
// the slot header. Positive values are engine codes (1 = bad param,
// 3 = empty slot, 14 = retry budget exhausted, 0x14/0x15 from
// TestTagDbVer, etc.). Negative values are OCX_ERR_* / BP_ERR_*
// codes — see docs/error-codes.md and c/include/bpclient.h.
type EngineError int

func (e EngineError) Error() string {
	return fmt.Sprintf("shm: engine errorcode %d (0x%x)", int(e), uint32(e))
}

// Client is the IPC handle to bpServer. Safe for concurrent use:
// each Call() reserves its own slot across all 16 slots, gated by
// the cross-process /bpShm sem and a local scan mutex.
type Client struct {
	shmFd   int
	shm     []byte
	semLock *posixSem
	semReq  [SlotCount]*posixSem
	semResp [SlotCount]*posixSem
	pid     int
	scanMu  sync.Mutex
}

// Open maps /dev/shm/bpShmem and opens all 33 named semaphores
// (/bpShm + 16×/bpReqNN + 16×/bpRespNN). No wire request is sent.
//
// Container plumbing required: --ipc=host --pid=host -v /dev/shm:/dev/shm.
func Open() (*Client, error) {
	fd, err := unix.Open(ShmPath, unix.O_RDWR, 0)
	if err != nil {
		return nil, fmt.Errorf("%w: open(%s): %v", ErrClientOpen, ShmPath, err)
	}

	buf, err := unix.Mmap(fd, 0, ShmTotalSize, unix.PROT_READ|unix.PROT_WRITE, unix.MAP_SHARED)
	if err != nil {
		unix.Close(fd)
		return nil, fmt.Errorf("%w: mmap: %v", ErrClientOpen, err)
	}

	c := &Client{shmFd: fd, shm: buf, pid: os.Getpid()}

	c.semLock, err = semOpen(SemShmLock)
	if err != nil {
		c.Close()
		return nil, fmt.Errorf("%w: %v", ErrClientOpen, err)
	}

	for i := 0; i < SlotCount; i++ {
		c.semReq[i], err = semOpen(fmt.Sprintf("%s%02d", SemReqPrefix, i))
		if err != nil {
			c.Close()
			return nil, fmt.Errorf("%w: %v", ErrClientOpen, err)
		}
		c.semResp[i], err = semOpen(fmt.Sprintf("%s%02d", SemRespPrefix, i))
		if err != nil {
			c.Close()
			return nil, fmt.Errorf("%w: %v", ErrClientOpen, err)
		}
	}

	return c, nil
}

// Close releases all IPC resources. Safe to call on a nil receiver
// or partially-initialised Client (used as the failure-path unwind
// in Open).
func (c *Client) Close() {
	if c == nil {
		return
	}
	for i := 0; i < SlotCount; i++ {
		c.semReq[i].Close()
		c.semReq[i] = nil
		c.semResp[i].Close()
		c.semResp[i] = nil
	}
	c.semLock.Close()
	c.semLock = nil
	if c.shm != nil {
		unix.Munmap(c.shm)
		c.shm = nil
	}
	if c.shmFd > 0 {
		unix.Close(c.shmFd)
		c.shmFd = -1
	}
}

// PID returns the process PID written into slot headers
// (HdrClientPID). Captured at Open() time.
func (c *Client) PID() int { return c.pid }

// Slot returns the [SlotStride]byte slice at index i. Callers must
// not retain the slice past the duration of one Call()'s
// fill/read callbacks — the slot may be reused.
func (c *Client) Slot(i int) []byte {
	return c.shm[i*SlotStride : (i+1)*SlotStride]
}

// reserveSlot picks the first free slot, writes the owner field, and
// drains stale posts. Mirrors reserve_slot() in c/src/client.c.
func (c *Client) reserveSlot(tid int) (int, error) {
	if err := c.semLock.Wait(); err != nil {
		return -1, fmt.Errorf("shm: sem_wait /bpShm: %w", err)
	}
	c.scanMu.Lock()

	found := -1
	for i := 0; i < SlotCount; i++ {
		slot := c.Slot(i)
		owner := binary.LittleEndian.Uint64(slot[HdrSlotOwner:])
		if owner == 0 {
			newOwner := (uint64(uint32(tid)) << 32) | uint64(uint32(c.pid))
			binary.LittleEndian.PutUint64(slot[HdrSlotOwner:], newOwner)
			c.semReq[i].Drain()
			c.semResp[i].Drain()
			found = i
			break
		}
	}

	c.scanMu.Unlock()
	c.semLock.Post()

	if found < 0 {
		return -1, ErrNoFreeSlot
	}
	return found, nil
}

// releaseSlot zeroes the owner field and drains the slot's sems.
// Mirrors release_slot() in c/src/client.c.
func (c *Client) releaseSlot(idx int) {
	if err := c.semLock.Wait(); err != nil {
		return
	}
	slot := c.Slot(idx)
	binary.LittleEndian.PutUint64(slot[HdrSlotOwner:], 0)
	c.semReq[idx].Drain()
	c.semResp[idx].Drain()
	c.semLock.Post()
}
