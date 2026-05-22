// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"
	"math"
)

// arrayRW dispatches one batched-of-one Access call with the given
// type / size / count. Used by the typed array helpers below.
//
// For ActionRead, data is the OUT buffer (must be sized to
// count*elemByteSize).  For ActionWrite, it's the IN bytes already
// laid out in CIP wire order.
func arrayRW(db *TagDB, tag string, dt, ebs, action, count uint16, data []byte) error {
	if db == nil {
		return ErrNullArg
	}
	if count == 0 {
		return ErrParamRange
	}
	r := []TagRequest{{
		TagName:      tag,
		DataType:     dt,
		ElemByteSize: ebs,
		Action:       action,
		ElemCount:    count,
		Data:         data,
	}}
	if err := db.Access(r); err != nil {
		return err
	}
	if r[0].Result != 0 {
		return ErrGeneric
	}
	return nil
}

// ----- Signed integer arrays -----

// ReadSINTArray reads an int8[] slice from the PLC.
func (db *TagDB) ReadSINTArray(tag string, count uint16) ([]int8, error) {
	buf := make([]byte, count)
	if err := arrayRW(db, tag, TypeSint, 1, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]int8, count)
	for i := range out {
		out[i] = int8(buf[i])
	}
	return out, nil
}

// WriteSINTArray writes an int8[] slice to the PLC.
func (db *TagDB) WriteSINTArray(tag string, vals []int8) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals))
	for i, v := range vals {
		buf[i] = byte(v)
	}
	return arrayRW(db, tag, TypeSint, 1, ActionWrite, uint16(len(vals)), buf)
}

// ReadINTArray reads an int16[] slice from the PLC.
func (db *TagDB) ReadINTArray(tag string, count uint16) ([]int16, error) {
	buf := make([]byte, int(count)*2)
	if err := arrayRW(db, tag, TypeInt, 2, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]int16, count)
	for i := range out {
		out[i] = int16(binary.LittleEndian.Uint16(buf[i*2:]))
	}
	return out, nil
}

// WriteINTArray writes an int16[] slice to the PLC.
func (db *TagDB) WriteINTArray(tag string, vals []int16) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*2)
	for i, v := range vals {
		binary.LittleEndian.PutUint16(buf[i*2:], uint16(v))
	}
	return arrayRW(db, tag, TypeInt, 2, ActionWrite, uint16(len(vals)), buf)
}

// ReadDINTArray reads an int32[] slice from the PLC.
func (db *TagDB) ReadDINTArray(tag string, count uint16) ([]int32, error) {
	buf := make([]byte, int(count)*4)
	if err := arrayRW(db, tag, TypeDint, 4, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]int32, count)
	for i := range out {
		out[i] = int32(binary.LittleEndian.Uint32(buf[i*4:]))
	}
	return out, nil
}

// WriteDINTArray writes an int32[] slice to the PLC.
func (db *TagDB) WriteDINTArray(tag string, vals []int32) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*4)
	for i, v := range vals {
		binary.LittleEndian.PutUint32(buf[i*4:], uint32(v))
	}
	return arrayRW(db, tag, TypeDint, 4, ActionWrite, uint16(len(vals)), buf)
}

// ReadLINTArray reads an int64[] slice from the PLC.
func (db *TagDB) ReadLINTArray(tag string, count uint16) ([]int64, error) {
	buf := make([]byte, int(count)*8)
	if err := arrayRW(db, tag, TypeLint, 8, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]int64, count)
	for i := range out {
		out[i] = int64(binary.LittleEndian.Uint64(buf[i*8:]))
	}
	return out, nil
}

// WriteLINTArray writes an int64[] slice to the PLC.
func (db *TagDB) WriteLINTArray(tag string, vals []int64) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*8)
	for i, v := range vals {
		binary.LittleEndian.PutUint64(buf[i*8:], uint64(v))
	}
	return arrayRW(db, tag, TypeLint, 8, ActionWrite, uint16(len(vals)), buf)
}

// ----- Unsigned integer arrays -----

// ReadUSINTArray reads a uint8[] slice from the PLC.
func (db *TagDB) ReadUSINTArray(tag string, count uint16) ([]uint8, error) {
	buf := make([]byte, count)
	if err := arrayRW(db, tag, TypeUsint, 1, ActionRead, count, buf); err != nil {
		return nil, err
	}
	return buf, nil
}

// WriteUSINTArray writes a uint8[] slice to the PLC.
func (db *TagDB) WriteUSINTArray(tag string, vals []uint8) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	return arrayRW(db, tag, TypeUsint, 1, ActionWrite, uint16(len(vals)), append([]byte(nil), vals...))
}

// ReadUINTArray reads a uint16[] slice from the PLC.
func (db *TagDB) ReadUINTArray(tag string, count uint16) ([]uint16, error) {
	buf := make([]byte, int(count)*2)
	if err := arrayRW(db, tag, TypeUint, 2, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]uint16, count)
	for i := range out {
		out[i] = binary.LittleEndian.Uint16(buf[i*2:])
	}
	return out, nil
}

// WriteUINTArray writes a uint16[] slice to the PLC.
func (db *TagDB) WriteUINTArray(tag string, vals []uint16) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*2)
	for i, v := range vals {
		binary.LittleEndian.PutUint16(buf[i*2:], v)
	}
	return arrayRW(db, tag, TypeUint, 2, ActionWrite, uint16(len(vals)), buf)
}

// ReadUDINTArray reads a uint32[] slice from the PLC.
func (db *TagDB) ReadUDINTArray(tag string, count uint16) ([]uint32, error) {
	buf := make([]byte, int(count)*4)
	if err := arrayRW(db, tag, TypeUdint, 4, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]uint32, count)
	for i := range out {
		out[i] = binary.LittleEndian.Uint32(buf[i*4:])
	}
	return out, nil
}

// WriteUDINTArray writes a uint32[] slice to the PLC.
func (db *TagDB) WriteUDINTArray(tag string, vals []uint32) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*4)
	for i, v := range vals {
		binary.LittleEndian.PutUint32(buf[i*4:], v)
	}
	return arrayRW(db, tag, TypeUdint, 4, ActionWrite, uint16(len(vals)), buf)
}

// ReadULINTArray reads a uint64[] slice from the PLC.
func (db *TagDB) ReadULINTArray(tag string, count uint16) ([]uint64, error) {
	buf := make([]byte, int(count)*8)
	if err := arrayRW(db, tag, TypeUlint, 8, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]uint64, count)
	for i := range out {
		out[i] = binary.LittleEndian.Uint64(buf[i*8:])
	}
	return out, nil
}

// WriteULINTArray writes a uint64[] slice to the PLC.
func (db *TagDB) WriteULINTArray(tag string, vals []uint64) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*8)
	for i, v := range vals {
		binary.LittleEndian.PutUint64(buf[i*8:], v)
	}
	return arrayRW(db, tag, TypeUlint, 8, ActionWrite, uint16(len(vals)), buf)
}

// ----- Float arrays -----

// ReadREALArray reads a float32[] slice from the PLC.
func (db *TagDB) ReadREALArray(tag string, count uint16) ([]float32, error) {
	buf := make([]byte, int(count)*4)
	if err := arrayRW(db, tag, TypeReal, 4, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]float32, count)
	for i := range out {
		out[i] = math.Float32frombits(binary.LittleEndian.Uint32(buf[i*4:]))
	}
	return out, nil
}

// WriteREALArray writes a float32[] slice to the PLC.
func (db *TagDB) WriteREALArray(tag string, vals []float32) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*4)
	for i, v := range vals {
		binary.LittleEndian.PutUint32(buf[i*4:], math.Float32bits(v))
	}
	return arrayRW(db, tag, TypeReal, 4, ActionWrite, uint16(len(vals)), buf)
}

// ReadLREALArray reads a float64[] slice from the PLC.
func (db *TagDB) ReadLREALArray(tag string, count uint16) ([]float64, error) {
	buf := make([]byte, int(count)*8)
	if err := arrayRW(db, tag, TypeLreal, 8, ActionRead, count, buf); err != nil {
		return nil, err
	}
	out := make([]float64, count)
	for i := range out {
		out[i] = math.Float64frombits(binary.LittleEndian.Uint64(buf[i*8:]))
	}
	return out, nil
}

// WriteLREALArray writes a float64[] slice to the PLC.
func (db *TagDB) WriteLREALArray(tag string, vals []float64) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	buf := make([]byte, len(vals)*8)
	for i, v := range vals {
		binary.LittleEndian.PutUint64(buf[i*8:], math.Float64bits(v))
	}
	return arrayRW(db, tag, TypeLreal, 8, ActionWrite, uint16(len(vals)), buf)
}

// ----- BOOL[] (Logix BIT_ARRAY 0xD3) -----
//
// Logix packs BOOL[N] as ceil(N/32) DWORDs on the wire (CIP type
// 0xD3 = BIT_ARRAY). These helpers convert between that wire form
// and a Go []bool slice.
//
// Write note: if count is not a multiple of 32, the trailing bits
// in the last DWORD are written as zeros. Read-modify-write if you
// want to preserve unrelated bits.

func dwordsForBits(count uint16) uint16 {
	return (count + 31) / 32
}

// ReadBOOLArray reads `count` BOOL[] elements as a Go []bool slice.
// Logix returns ceil(count/32) DWORDs internally; this helper
// unpacks the bits.
func (db *TagDB) ReadBOOLArray(tag string, count uint16) ([]bool, error) {
	if count == 0 {
		return nil, ErrParamRange
	}
	n := dwordsForBits(count)
	buf := make([]byte, int(n)*4)
	if err := arrayRW(db, tag, TypeBitArray, 4, ActionRead, n, buf); err != nil {
		return nil, err
	}
	out := make([]bool, count)
	for i := uint16(0); i < count; i++ {
		word := binary.LittleEndian.Uint32(buf[(i/32)*4:])
		out[i] = word&(1<<(i&31)) != 0
	}
	return out, nil
}

// WriteBOOLArray writes a []bool slice as Logix BIT_ARRAY DWORDs.
// `vals` must hold the array's declared dimension (e.g. 32 for
// BOOL[32]); the wire transfer is ceil(len(vals)/32) DWORDs.
func (db *TagDB) WriteBOOLArray(tag string, vals []bool) error {
	if len(vals) == 0 || len(vals) > 0xFFFF {
		return ErrParamRange
	}
	count := uint16(len(vals))
	n := dwordsForBits(count)
	buf := make([]byte, int(n)*4)
	for i, v := range vals {
		if v {
			off := (i / 32) * 4
			word := binary.LittleEndian.Uint32(buf[off:])
			word |= 1 << (uint(i) & 31)
			binary.LittleEndian.PutUint32(buf[off:], word)
		}
	}
	return arrayRW(db, tag, TypeBitArray, 4, ActionWrite, n, buf)
}

// ----- STRING (AB Logix STRING family) -----
//
// Works with the default STRING (LEN:DINT + DATA:SINT[82]),
// STRING_32, STRING_512, and any LEN+DATA-shaped UDT.  Does two
// IPC round-trips internally (LEN and DATA accessed separately).

// ReadString reads tag.LEN (DINT) then tag.DATA (SINT[LEN]) and
// returns the result as a Go string.  Caller-supplied capping is
// not necessary — the PLC LEN field is authoritative.
func (db *TagDB) ReadString(tag string) (string, error) {
	if len(tag) == 0 || len(tag) > 250 {
		return "", ErrParamRange
	}
	lenField, err := db.ReadDINT(tag + ".LEN")
	if err != nil {
		return "", err
	}
	if lenField <= 0 {
		return "", nil
	}
	n := int(lenField)
	if n > 0xFFFF {
		n = 0xFFFF
	}
	data, err := db.ReadSINTArray(tag+".DATA", uint16(n))
	if err != nil {
		return "", err
	}
	b := make([]byte, len(data))
	for i, v := range data {
		b[i] = byte(v)
	}
	return string(b), nil
}

// WriteString writes value's bytes into tag.DATA (as SINT[]) then
// updates tag.LEN.  If the destination's DATA[] capacity is smaller
// than len(value), the engine returns a CIP General Status error
// (typically 0x13 "Not enough data" or 0x15 "Too much data"),
// surfaced as ErrGeneric.
func (db *TagDB) WriteString(tag, value string) error {
	if len(tag) == 0 || len(tag) > 250 {
		return ErrParamRange
	}
	if len(value) > 0xFFFF {
		return ErrParamRange
	}
	if len(value) > 0 {
		data := make([]int8, len(value))
		for i := range value {
			data[i] = int8(value[i])
		}
		if err := db.WriteSINTArray(tag+".DATA", data); err != nil {
			return err
		}
	}
	return db.WriteDINT(tag+".LEN", int32(len(value)))
}
