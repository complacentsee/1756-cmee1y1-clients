// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// IDObject is the public CIP Identity-object struct. Mirrors
// bp_id_object_t / cip.IDObject.
type IDObject = cip.IDObject

// GetIDLocal returns the LOCAL cm1756 module's Identity object via
// OCXcip_GetIdObject (no path, no class word — engine knows it's
// self-querying).
func (c *Client) GetIDLocal() (IDObject, error) {
	if c == nil {
		return IDObject{}, ErrNullArg
	}
	var out IDObject
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetIdObject,
		PayloadSize: cip.SizeGetIdObject,
		Read:        func(slot []byte) { out = cip.DecodeGetIdLocal(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

// GetDeviceID returns the Identity of the device named by textPath
// ("P:1,S:2", etc.) and instance (normally 1). Wraps
// OCXcip_GetDeviceIdObject — the OEM library does path parsing
// internally, so this is the safest way to address a remote device
// when you're unsure about CIP EPATH encoding edge cases.
func (c *Client) GetDeviceID(textPath string, instance uint16) (IDObject, error) {
	if c == nil || textPath == "" {
		return IDObject{}, ErrNullArg
	}
	if len(textPath) > 254 {
		return IDObject{}, ErrParamRange
	}
	var out IDObject
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetDeviceIdObject,
		PayloadSize: cip.SizeGetDeviceIdObject,
		Fill:        func(slot []byte) { cip.EncodeGetDeviceId(slot, textPath, instance) },
		Read:        func(slot []byte) { out = cip.DecodeGetDeviceId(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

// GetDeviceIDStatus returns just the 16-bit Identity status word
// for the device named by textPath / instance.  Faster than
// GetDeviceID when callers only need the heartbeat / Logix-mode
// nibble (bits 4..7).
func (c *Client) GetDeviceIDStatus(textPath string, instance uint16) (uint16, error) {
	if c == nil || textPath == "" {
		return 0, ErrNullArg
	}
	if len(textPath) > 254 {
		return 0, ErrParamRange
	}
	var status uint16
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetDeviceIdStatus,
		PayloadSize: cip.SizeGetDeviceIdStatus,
		Fill:        func(slot []byte) { cip.EncodeGetDeviceIdStatus(slot, textPath, instance) },
		Read:        func(slot []byte) { status = cip.DecodeGetDeviceIdStatus(slot) },
		TimeoutMs:   5000,
	})
	return status, translateCallErr(err)
}

// GetExDevObject returns the 226-byte extended device info for the
// device named by textPath.  Raw bytes — caller decodes the
// vendor-specific layout (28 qwords + uint16 trailer).
func (c *Client) GetExDevObject(textPath string, instance uint16) ([]byte, error) {
	if c == nil || textPath == "" {
		return nil, ErrNullArg
	}
	if len(textPath) > 254 {
		return nil, ErrParamRange
	}
	out := make([]byte, cip.ExDevObjectBytes)
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetExDevObject,
		PayloadSize: cip.SizeGetExDevObject,
		Fill: func(slot []byte) {
			n := len(textPath)
			if n > 254 {
				n = 254
			}
			copy(slot[shm.HdrPayloadStart:shm.HdrPayloadStart+n], textPath)
			binary.LittleEndian.PutUint16(slot[0x25A:], instance)
		},
		Read:      func(slot []byte) { copy(out, slot[0x178:0x178+cip.ExDevObjectBytes]) },
		TimeoutMs: 5000,
	})
	return out, translateCallErr(err)
}

// GetDeviceICPObject returns the 20-byte EtherNet/IP IP-config
// object (2 qwords + 1 uint32; likely IP/netmask/gateway).
func (c *Client) GetDeviceICPObject(textPath string, instance uint16) ([]byte, error) {
	if c == nil || textPath == "" {
		return nil, ErrNullArg
	}
	if len(textPath) > 254 {
		return nil, ErrParamRange
	}
	out := make([]byte, cip.DeviceICPObjectBytes)
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetDeviceICPObject,
		PayloadSize: cip.SizeGetDeviceICPObject,
		Fill: func(slot []byte) {
			n := len(textPath)
			if n > 254 {
				n = 254
			}
			copy(slot[shm.HdrPayloadStart:shm.HdrPayloadStart+n], textPath)
			binary.LittleEndian.PutUint16(slot[0x18C:], instance)
		},
		Read:      func(slot []byte) { copy(out, slot[0x178:0x178+cip.DeviceICPObjectBytes]) },
		TimeoutMs: 5000,
	})
	return out, translateCallErr(err)
}

// GetActiveNodes returns the 64-bit active-node bitmap as (lo, hi)
// 32-bit halves. Bit N in (lo | hi<<32) is set when node N is
// responsive on the backplane.
func (c *Client) GetActiveNodes() (lo, hi uint32, err error) {
	if c == nil {
		return 0, 0, ErrNullArg
	}
	callErr := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetActiveNodeTable,
		PayloadSize: cip.SizeGetActiveNodeTable,
		Read:        func(slot []byte) { lo, hi = cip.DecodeGetActiveNodes(slot) },
		TimeoutMs:   5000,
	})
	return lo, hi, translateCallErr(callErr)
}
