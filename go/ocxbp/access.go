// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"
	"math"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// CIP atomic type codes (low 13 bits of data_type).
const (
	TypeBool     uint16 = 0xC1
	TypeSint     uint16 = 0xC2
	TypeInt      uint16 = 0xC3
	TypeDint     uint16 = 0xC4
	TypeLint     uint16 = 0xC5
	TypeUsint    uint16 = 0xC6
	TypeUint     uint16 = 0xC7
	TypeUdint    uint16 = 0xC8
	TypeUlint    uint16 = 0xC9
	TypeReal     uint16 = 0xCA
	TypeLreal    uint16 = 0xCB
	TypeBitArray uint16 = 0xD3 // Logix BOOL[] packed 32 bits per DWORD
)

// AccessTagData actions, re-exported.
const (
	ActionRead  = cip.ActionRead
	ActionWrite = cip.ActionWrite
)

// TagRequest is one entry in a batched Access call. Aliased to cip's
// definition so public callers can construct requests directly.
type TagRequest = cip.TagRequest

// Access performs one OCXcip_AccessTagData call with `reqs` requests.
// On non-error return, each request's Result field holds the CIP
// General Status (0 = ok). The slot-level errorcode is returned via
// the error.
func (db *TagDB) Access(reqs []TagRequest) error {
	if db == nil || len(reqs) == 0 {
		return ErrNullArg
	}
	if len(reqs) > 16 {
		return ErrParamRange
	}
	payloadSize := cip.AccessTagDataPayloadSize(reqs)
	if payloadSize > shm.SlotStride-0x80 {
		return ErrSlotTooLarge
	}
	err := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnAccessTagData,
		PayloadSize: payloadSize,
		Fill:        func(slot []byte) { cip.EncodeAccessTagData(slot, db.path, reqs) },
		Read:        func(slot []byte) { cip.DecodeAccessTagData(slot, reqs) },
		TimeoutMs:   10000,
	})
	return translateCallErr(err)
}

// scalarRW does one Read or Write of a single elem_byte_size-byte
// scalar by tag name. Used by the typed helpers below.
func scalarRW(db *TagDB, tag string, dt uint16, ebs uint16, action uint16, data []byte) error {
	r := []TagRequest{{
		TagName: tag, DataType: dt, ElemByteSize: ebs,
		Action: action, ElemCount: 1, Data: data,
	}}
	if err := db.Access(r); err != nil {
		return err
	}
	if r[0].Result != 0 {
		return ErrGeneric
	}
	return nil
}

// ----- Signed scalar helpers -----

// ReadSINT reads a single SINT (int8) tag.
func (db *TagDB) ReadSINT(tag string) (int8, error) {
	var buf [1]byte
	if err := scalarRW(db, tag, TypeSint, 1, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return int8(buf[0]), nil
}

// WriteSINT writes a single SINT.
func (db *TagDB) WriteSINT(tag string, v int8) error {
	buf := []byte{byte(v)}
	return scalarRW(db, tag, TypeSint, 1, ActionWrite, buf)
}

// ReadINT reads a single INT (int16).
func (db *TagDB) ReadINT(tag string) (int16, error) {
	var buf [2]byte
	if err := scalarRW(db, tag, TypeInt, 2, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return int16(binary.LittleEndian.Uint16(buf[:])), nil
}

// WriteINT writes a single INT.
func (db *TagDB) WriteINT(tag string, v int16) error {
	buf := make([]byte, 2)
	binary.LittleEndian.PutUint16(buf, uint16(v))
	return scalarRW(db, tag, TypeInt, 2, ActionWrite, buf)
}

// ReadDINT reads a single DINT (int32).
func (db *TagDB) ReadDINT(tag string) (int32, error) {
	var buf [4]byte
	if err := scalarRW(db, tag, TypeDint, 4, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return int32(binary.LittleEndian.Uint32(buf[:])), nil
}

// WriteDINT writes a single DINT.
func (db *TagDB) WriteDINT(tag string, v int32) error {
	buf := make([]byte, 4)
	binary.LittleEndian.PutUint32(buf, uint32(v))
	return scalarRW(db, tag, TypeDint, 4, ActionWrite, buf)
}

// ReadLINT reads a single LINT (int64).
func (db *TagDB) ReadLINT(tag string) (int64, error) {
	var buf [8]byte
	if err := scalarRW(db, tag, TypeLint, 8, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return int64(binary.LittleEndian.Uint64(buf[:])), nil
}

// WriteLINT writes a single LINT.
func (db *TagDB) WriteLINT(tag string, v int64) error {
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint64(buf, uint64(v))
	return scalarRW(db, tag, TypeLint, 8, ActionWrite, buf)
}

// ----- Unsigned scalar helpers -----

// ReadUSINT reads a single USINT.
func (db *TagDB) ReadUSINT(tag string) (uint8, error) {
	var buf [1]byte
	err := scalarRW(db, tag, TypeUsint, 1, ActionRead, buf[:])
	return buf[0], err
}

// WriteUSINT writes a single USINT.
func (db *TagDB) WriteUSINT(tag string, v uint8) error {
	return scalarRW(db, tag, TypeUsint, 1, ActionWrite, []byte{v})
}

// ReadUINT reads a single UINT.
func (db *TagDB) ReadUINT(tag string) (uint16, error) {
	var buf [2]byte
	if err := scalarRW(db, tag, TypeUint, 2, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint16(buf[:]), nil
}

// WriteUINT writes a single UINT.
func (db *TagDB) WriteUINT(tag string, v uint16) error {
	buf := make([]byte, 2)
	binary.LittleEndian.PutUint16(buf, v)
	return scalarRW(db, tag, TypeUint, 2, ActionWrite, buf)
}

// ReadUDINT reads a single UDINT.
func (db *TagDB) ReadUDINT(tag string) (uint32, error) {
	var buf [4]byte
	if err := scalarRW(db, tag, TypeUdint, 4, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint32(buf[:]), nil
}

// WriteUDINT writes a single UDINT.
func (db *TagDB) WriteUDINT(tag string, v uint32) error {
	buf := make([]byte, 4)
	binary.LittleEndian.PutUint32(buf, v)
	return scalarRW(db, tag, TypeUdint, 4, ActionWrite, buf)
}

// ReadULINT reads a single ULINT.
func (db *TagDB) ReadULINT(tag string) (uint64, error) {
	var buf [8]byte
	if err := scalarRW(db, tag, TypeUlint, 8, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return binary.LittleEndian.Uint64(buf[:]), nil
}

// WriteULINT writes a single ULINT.
func (db *TagDB) WriteULINT(tag string, v uint64) error {
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint64(buf, v)
	return scalarRW(db, tag, TypeUlint, 8, ActionWrite, buf)
}

// ----- Float helpers -----

// ReadREAL reads a single REAL (float32).
func (db *TagDB) ReadREAL(tag string) (float32, error) {
	var buf [4]byte
	if err := scalarRW(db, tag, TypeReal, 4, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return math.Float32frombits(binary.LittleEndian.Uint32(buf[:])), nil
}

// WriteREAL writes a single REAL.
func (db *TagDB) WriteREAL(tag string, v float32) error {
	buf := make([]byte, 4)
	binary.LittleEndian.PutUint32(buf, math.Float32bits(v))
	return scalarRW(db, tag, TypeReal, 4, ActionWrite, buf)
}

// ReadLREAL reads a single LREAL (float64).
func (db *TagDB) ReadLREAL(tag string) (float64, error) {
	var buf [8]byte
	if err := scalarRW(db, tag, TypeLreal, 8, ActionRead, buf[:]); err != nil {
		return 0, err
	}
	return math.Float64frombits(binary.LittleEndian.Uint64(buf[:])), nil
}

// WriteLREAL writes a single LREAL.
func (db *TagDB) WriteLREAL(tag string, v float64) error {
	buf := make([]byte, 8)
	binary.LittleEndian.PutUint64(buf, math.Float64bits(v))
	return scalarRW(db, tag, TypeLreal, 8, ActionWrite, buf)
}

// ----- BOOL -----

// ReadBOOL reads a single BOOL. On the wire it's a single byte (0 or 1).
func (db *TagDB) ReadBOOL(tag string) (bool, error) {
	var buf [1]byte
	if err := scalarRW(db, tag, TypeBool, 1, ActionRead, buf[:]); err != nil {
		return false, err
	}
	return buf[0] != 0, nil
}

// WriteBOOL writes a single BOOL.
func (db *TagDB) WriteBOOL(tag string, v bool) error {
	b := byte(0)
	if v {
		b = 1
	}
	return scalarRW(db, tag, TypeBool, 1, ActionWrite, []byte{b})
}
