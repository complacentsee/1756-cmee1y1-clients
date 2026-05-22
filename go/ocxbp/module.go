// SPDX-License-Identifier: MIT

package ocxbp

import (
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// GetSwitchPosition reads the front-panel rotary switch on the local
// cm1756. Returns 0 if no switch is present or firmware doesn't
// expose it.
func (c *Client) GetSwitchPosition() (uint32, error) {
	if c == nil {
		return 0, ErrNullArg
	}
	var v uint32
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetSwitchPosition,
		PayloadSize: cip.SizeGetSwitchPosition,
		Read:        func(slot []byte) { v = cip.DecodeGetSwitchPosition(slot) },
		TimeoutMs:   5000,
	})
	return v, translateCallErr(err)
}

// GetLED reads the state of an LED. ledID + state values are
// vendor-defined; consult ASEM/Rockwell EEC docs for the IDs
// available on your hardware revision.
func (c *Client) GetLED(ledID uint32) (uint32, error) {
	if c == nil {
		return 0, ErrNullArg
	}
	var state uint32
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetLED,
		PayloadSize: cip.SizeGetLED,
		Fill:        func(slot []byte) { cip.EncodeGetLED(slot, ledID) },
		Read:        func(slot []byte) { state = cip.DecodeGetLED(slot) },
		TimeoutMs:   5000,
	})
	return state, translateCallErr(err)
}

// SetLED writes an LED state.
func (c *Client) SetLED(ledID, state uint32) error {
	if c == nil {
		return ErrNullArg
	}
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnSetLED,
		PayloadSize: cip.SizeSetLED,
		Fill:        func(slot []byte) { cip.EncodeSetLED(slot, ledID, state) },
		TimeoutMs:   5000,
	})
	return translateCallErr(err)
}

// GetDisplay reads the 4-char module display, NUL-padded to 5 bytes.
func (c *Client) GetDisplay() ([5]byte, error) {
	if c == nil {
		return [5]byte{}, ErrNullArg
	}
	var out [5]byte
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetDisplay,
		PayloadSize: cip.SizeGetDisplay,
		Read:        func(slot []byte) { out = cip.DecodeGetDisplay(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

// SetDisplay writes the 4-char module display. On some firmware
// revisions Set is silently ignored — Get afterwards to verify if
// exactness matters.
func (c *Client) SetDisplay(fourChars [4]byte) error {
	if c == nil {
		return ErrNullArg
	}
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnSetDisplay,
		PayloadSize: cip.SizeSetDisplay,
		Fill:        func(slot []byte) { cip.EncodeSetDisplay(slot, fourChars) },
		TimeoutMs:   5000,
	})
	return translateCallErr(err)
}
