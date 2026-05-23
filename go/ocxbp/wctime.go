// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// WCTime is the raw 6-qword wall-clock struct exchanged with the
// engine (mirrors C bp_wctime_t / Python bpclient.WCTime).
//
// Sec is Unix epoch seconds (UTC for the *UTC variants).  Aux0..3
// carry TZ / DST / leap-second metadata in a layout we haven't fully
// characterized — exposed as opaque uint64s.
type WCTime struct {
	Sec  uint64
	Nsec uint64
	Aux0 uint64
	Aux1 uint64
	Aux2 uint64
	Aux3 uint64
}

func encodeWCTimeReq(slot []byte, path string, instance uint16, in *WCTime) {
	n := len(path)
	if n > 254 {
		n = 254
	}
	copy(slot[shm.HdrPayloadStart:shm.HdrPayloadStart+n], path)
	binary.LittleEndian.PutUint16(slot[0x178:], instance)
	if in != nil {
		slot[0x17A] = 1
		binary.LittleEndian.PutUint64(slot[0x180:], in.Sec)
		binary.LittleEndian.PutUint64(slot[0x188:], in.Nsec)
		binary.LittleEndian.PutUint64(slot[0x190:], in.Aux0)
		binary.LittleEndian.PutUint64(slot[0x198:], in.Aux1)
		binary.LittleEndian.PutUint64(slot[0x1A0:], in.Aux2)
		binary.LittleEndian.PutUint64(slot[0x1A8:], in.Aux3)
	} else {
		slot[0x17A] = 0
	}
}

func decodeWCTimeResp(slot []byte) WCTime {
	return WCTime{
		Sec:  binary.LittleEndian.Uint64(slot[0x180:]),
		Nsec: binary.LittleEndian.Uint64(slot[0x188:]),
		Aux0: binary.LittleEndian.Uint64(slot[0x190:]),
		Aux1: binary.LittleEndian.Uint64(slot[0x198:]),
		Aux2: binary.LittleEndian.Uint64(slot[0x1A0:]),
		Aux3: binary.LittleEndian.Uint64(slot[0x1A8:]),
	}
}

func (c *Client) wctimeGet(fn, path string, instance uint16) (WCTime, error) {
	if c == nil || path == "" {
		return WCTime{}, ErrNullArg
	}
	if len(path) > 254 {
		return WCTime{}, ErrParamRange
	}
	var out WCTime
	have := WCTime{} // dummy; encodeWCTimeReq treats &have as "want response"
	err := c.shm.Call(shm.CallSpec{
		FnName:      fn,
		PayloadSize: cip.SizeWCTime,
		Fill:        func(slot []byte) { encodeWCTimeReq(slot, path, instance, &have) },
		Read:        func(slot []byte) { out = decodeWCTimeResp(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

func (c *Client) wctimeSet(fn, path string, instance uint16, in WCTime) error {
	if c == nil || path == "" {
		return ErrNullArg
	}
	if len(path) > 254 {
		return ErrParamRange
	}
	err := c.shm.Call(shm.CallSpec{
		FnName:      fn,
		PayloadSize: cip.SizeWCTime,
		Fill:        func(slot []byte) { encodeWCTimeReq(slot, path, instance, &in) },
		TimeoutMs:   5000,
	})
	return translateCallErr(err)
}

// GetWCTime reads the local-time wall clock from the device named by
// textPath.  See WCTime for the field semantics.
func (c *Client) GetWCTime(textPath string, instance uint16) (WCTime, error) {
	return c.wctimeGet(cip.FnGetWCTime, textPath, instance)
}

// GetWCTimeUTC reads the UTC wall clock from the device named by
// textPath.  Preferred over GetWCTime when callers need absolute
// time without worrying about the device's TZ config.
func (c *Client) GetWCTimeUTC(textPath string, instance uint16) (WCTime, error) {
	return c.wctimeGet(cip.FnGetWCTimeUTC, textPath, instance)
}

// SetWCTime writes the local-time wall clock on the device.
func (c *Client) SetWCTime(textPath string, instance uint16, in WCTime) error {
	return c.wctimeSet(cip.FnSetWCTime, textPath, instance, in)
}

// SetWCTimeUTC writes the UTC wall clock on the device.
func (c *Client) SetWCTimeUTC(textPath string, instance uint16, in WCTime) error {
	return c.wctimeSet(cip.FnSetWCTimeUTC, textPath, instance, in)
}
