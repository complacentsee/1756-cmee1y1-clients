// SPDX-License-Identifier: MIT

package ocxbp

import (
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// Client is the public outbound-CIP handle. Wraps the IPC shm.Client
// and the OCXcip_Open session handle (which is bookkeeping — the
// wrapper tracks state via slot ownership, not the handle).
type Client struct {
	shm *shm.Client
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
	return &Client{shm: s}, nil
}

// Close releases the IPC. Safe to call on nil.
func (c *Client) Close() {
	if c == nil {
		return
	}
	c.shm.Close()
	c.shm = nil
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
