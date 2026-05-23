// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"
	"time"

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

// WCTimeEpoch identifies the per-device epoch interpretation for
// WCTime.Sec.  See bp_wctime_epoch_t in the C SDK for the
// empirical L73/L85 observations.
type WCTimeEpoch int

const (
	WCTimeEpochUnix WCTimeEpoch = iota // 1970-01-01 UTC (L85 GetWCTimeUTC)
	WCTimeEpoch1972                    // 1972-01-01 UTC (L85 GetWCTime; ODVA standard)
	WCTimeEpoch1998                    // 1998-01-01 UTC (L73 GetWCTimeUTC)
	WCTimeEpoch2000                    // 2000-01-01 UTC (L73 GetWCTime)
)

func epochUnixSeconds(e WCTimeEpoch) int64 {
	switch e {
	case WCTimeEpoch1972:
		return 63072000
	case WCTimeEpoch1998:
		return 883612800
	case WCTimeEpoch2000:
		return 946684800
	}
	return 0
}

// ToUnixMicros converts the WCTime's Sec field to Unix-epoch
// microseconds, given the per-device epoch.
func (wc WCTime) ToUnixMicros(epoch WCTimeEpoch) int64 {
	return int64(wc.Sec) + epochUnixSeconds(epoch)*1_000_000
}

// ToTime converts the WCTime to a Go time.Time in UTC.
func (wc WCTime) ToTime(epoch WCTimeEpoch) time.Time {
	us := wc.ToUnixMicros(epoch)
	return time.Unix(us/1_000_000, (us%1_000_000)*1_000).UTC()
}

// WCTimeLocal is the broken-down LOCAL view of aux2 (v0.10.3+).
// aux2 packs four LE uint16s in (day, hour, minute, second) order.
// Confirmed across L73 + L85 — the decoded fields match the
// sec-derived UTC second-for-second.  See bp_wctime_local_t in
// the C SDK for the sibling-field caveats (aux0/aux1/aux3 are
// not decoded; their semantics aren't fully understood).
type WCTimeLocal struct {
	Day    uint16 // 1..31 — aux2 LE uint16 #0
	Hour   uint16 // 0..23 — aux2 LE uint16 #1
	Minute uint16 // 0..59 — aux2 LE uint16 #2
	Second uint16 // 0..59 — aux2 LE uint16 #3
}

// DecodeLocal extracts (day, hour, minute, second) from wc.Aux2.
// Returns zero-valued WCTimeLocal if wc is the zero value.
func (wc WCTime) DecodeLocal() WCTimeLocal {
	return WCTimeLocal{
		Day:    uint16(wc.Aux2 & 0xFFFF),
		Hour:   uint16((wc.Aux2 >> 16) & 0xFFFF),
		Minute: uint16((wc.Aux2 >> 32) & 0xFFFF),
		Second: uint16((wc.Aux2 >> 48) & 0xFFFF),
	}
}

// TZName extracts the NUL-terminated ASCII timezone-name string
// from the four aux qwords (32 bytes, little-endian).  Only
// meaningful when aux0..3 actually carry a TZ string (observed
// on the L85's GetWCTimeUTC response).
func (wc WCTime) TZName() string {
	var buf [32]byte
	binary.LittleEndian.PutUint64(buf[0:], wc.Aux0)
	binary.LittleEndian.PutUint64(buf[8:], wc.Aux1)
	binary.LittleEndian.PutUint64(buf[16:], wc.Aux2)
	binary.LittleEndian.PutUint64(buf[24:], wc.Aux3)
	n := 0
	for n < len(buf) && buf[n] != 0 {
		n++
	}
	return string(buf[:n])
}
