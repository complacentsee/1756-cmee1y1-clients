// SPDX-License-Identifier: MIT

package ocxbp

import (
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// ConnSpec is the public class-3 connection descriptor. Mirrors
// cip.ConnSpec / bp_conn_spec_t.
type ConnSpec = cip.ConnSpec

// TxRxOpen establishes a class-3 connection.
//
// STATUS: NOT FUNCTIONAL on cm1756 — always returns engine code 0x1001
// (4097) because OCXCN_OpenClass3Connection's library is missing from
// the cm1756 image. RE trace: docs/protocol.md "Connected messaging —
// open issues".
//
// Workaround: send a manual Large Forward Open (CIP service 0x5B)
// via Client.MessageSend; that path works on the L85 in slot 2.
// See c/examples/connidentity.c for the technique.
func (c *Client) TxRxOpen(spec *ConnSpec) (connID, connSerial uint16, err error) {
	if c == nil || spec == nil || spec.EncodedPath == nil {
		return 0, 0, ErrNullArg
	}
	if spec.PathSize == 0 || spec.PathSize > cip.TxRxMaxPath {
		return 0, 0, ErrParamRange
	}
	callErr := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnTxRxOpenConn,
		PayloadSize: cip.SizeTxRxOpenConn,
		Fill:        func(slot []byte) { cip.EncodeTxRxOpenConn(slot, spec) },
		Read:        func(slot []byte) { connID, connSerial = cip.DecodeTxRxOpenConn(slot) },
		TimeoutMs:   30000,
	})
	return connID, connSerial, translateCallErr(callErr)
}

// TxRxMsg sends one CIP request over the connection set up by
// TxRxOpen. respCap is the buffer capacity; resp is the destination.
// Returns the actual number of bytes the engine wrote.
//
// STATUS: NOT FUNCTIONAL — see TxRxOpen note.
func (c *Client) TxRxMsg(spec *ConnSpec, req []byte, resp []byte, respCap uint16) (uint16, error) {
	if c == nil || spec == nil || spec.EncodedPath == nil {
		return 0, ErrNullArg
	}
	if resp == nil || respCap == 0 {
		return 0, ErrNullArg
	}
	if spec.PathSize == 0 || spec.PathSize > cip.TxRxMaxPath {
		return 0, ErrParamRange
	}
	var got uint16
	callErr := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnTxRxMsg,
		PayloadSize: cip.SizeTxRxMsg,
		Fill: func(slot []byte) {
			cip.EncodeTxRxMsg(slot, spec, req, uint16(len(req)), respCap)
		},
		Read:      func(slot []byte) { got = cip.DecodeTxRxMsg(slot, resp, respCap) },
		TimeoutMs: 30000,
	})
	return got, translateCallErr(callErr)
}

// TxRxClose releases the connection. Idempotent per spec.AppHandle.
//
// STATUS: NOT FUNCTIONAL — see TxRxOpen note.
func (c *Client) TxRxClose(spec *ConnSpec) error {
	if c == nil || spec == nil || spec.EncodedPath == nil {
		return ErrNullArg
	}
	if spec.PathSize == 0 || spec.PathSize > cip.TxRxMaxPath {
		return ErrParamRange
	}
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnTxRxCloseConn,
		PayloadSize: cip.SizeTxRxCloseConn,
		Fill:        func(slot []byte) { cip.EncodeTxRxCloseConn(slot, spec) },
		TimeoutMs:   5000,
	})
	return translateCallErr(err)
}
