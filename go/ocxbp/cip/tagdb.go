// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// EncodeCreateTagDbHandle writes the path string at slot+0x78 (max
// 255 bytes, NUL-terminated) and flags=0 at slot+0x178.
func EncodeCreateTagDbHandle(slot []byte, path string) {
	// Zero the 256-byte path region.
	for i := 0; i < 256; i++ {
		slot[HdrPayloadStart+i] = 0
	}
	n := len(path)
	if n > 254 {
		n = 254
	}
	copy(slot[HdrPayloadStart:HdrPayloadStart+n], path)
	// Trailing NUL at offset HdrPayloadStart+n is already in place.
	binary.LittleEndian.PutUint16(slot[0x178:], 0) // flags
}

// DecodeCreateTagDbHandle reads the assigned db_handle at slot+0x17C.
func DecodeCreateTagDbHandle(slot []byte) uint32 {
	return binary.LittleEndian.Uint32(slot[0x17C:])
}

// EncodeHandle writes one uint32 db_handle at slot+0x78. Shared
// across DeleteTagDbHandle / BuildTagDb / TestTagDbVer.
func EncodeHandle(slot []byte, handle uint32) {
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart:], handle)
}

// DecodeBuildTagDb reads the enumerated symbol_count at slot+0x7C.
func DecodeBuildTagDb(slot []byte) uint16 {
	return binary.LittleEndian.Uint16(slot[HdrPayloadStart+4:])
}

// EncodeGetSymbolInfo writes db_handle at +0x78 and symbol index at +0x7C.
func EncodeGetSymbolInfo(slot []byte, handle uint32, index uint16) {
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart:], handle)
	binary.LittleEndian.PutUint16(slot[HdrPayloadStart+4:], index)
}

// SymbolInfo mirrors the 128-byte ocx_symbol_info_t at slot+0x80.
type SymbolInfo struct {
	Name         string // NUL-terminated, up to 99 chars
	DataType     uint16
	StructType   uint16
	ElemByteSize uint32
	Dim0         uint32
	Dim1         uint32
	Dim2         uint32
	Flags        uint16
}

// DecodeGetSymbolInfo parses the 128-byte struct at slot+0x80.
// Offsets within the struct itself per docs/protocol.md
// "OCXcip_GetSymbolInfo" → ocx_symbol_info_t.
func DecodeGetSymbolInfo(slot []byte) SymbolInfo {
	p := slot[HdrPayloadStart+8 : HdrPayloadStart+8+128]
	return SymbolInfo{
		Name:         readCString(p[:100], 99),
		DataType:     binary.LittleEndian.Uint16(p[0x64:]),
		StructType:   binary.LittleEndian.Uint16(p[0x68:]),
		ElemByteSize: binary.LittleEndian.Uint32(p[0x6C:]),
		Dim0:         binary.LittleEndian.Uint32(p[0x70:]),
		Dim1:         binary.LittleEndian.Uint32(p[0x74:]),
		Dim2:         binary.LittleEndian.Uint32(p[0x78:]),
		Flags:        binary.LittleEndian.Uint16(p[0x7C:]),
	}
}

// EncodeGetStructInfo: db_handle at +0x78, struct_id at +0x7C.
func EncodeGetStructInfo(slot []byte, handle uint32, structID uint16) {
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart:], handle)
	binary.LittleEndian.PutUint16(slot[HdrPayloadStart+4:], structID)
}

// StructInfo mirrors the 56-byte response at slot+0x80.
type StructInfo struct {
	Name      string // NUL-terminated, up to 39 chars
	DataType  uint32
	ByteSize  uint32
	NMembers  uint16
}

// DecodeGetStructInfo parses the StructInfo response.
// Layout (empirical, see c/src/tagdb.c::si_read):
//
//	+0x00 char[40]  name
//	+0x2C uint32    data_type
//	+0x30 uint32    byte_size
//	+0x36 uint16    n_members
func DecodeGetStructInfo(slot []byte) StructInfo {
	p := slot[HdrPayloadStart+8:]
	return StructInfo{
		Name:     readCString(p[:40], 39),
		DataType: binary.LittleEndian.Uint32(p[0x2C:]),
		ByteSize: binary.LittleEndian.Uint32(p[0x30:]),
		NMembers: binary.LittleEndian.Uint16(p[0x36:]),
	}
}

// EncodeGetStructMbrInfo: db_handle at +0x78, struct_id at +0x7C,
// member_index at +0x7E.
func EncodeGetStructMbrInfo(slot []byte, handle uint32, structID, memberIndex uint16) {
	binary.LittleEndian.PutUint32(slot[HdrPayloadStart:], handle)
	binary.LittleEndian.PutUint16(slot[HdrPayloadStart+4:], structID)
	binary.LittleEndian.PutUint16(slot[HdrPayloadStart+6:], memberIndex)
}

// StructMemberInfo mirrors the 76-byte response at slot+0x80.
type StructMemberInfo struct {
	Name       string // NUL-terminated, up to 43 chars
	DataType   uint16
	StructID   uint16 // non-zero if member is itself a UDT
	ByteSize   uint32
	Offset     uint32
	ArrayCount uint32
	Flags      uint8
}

// DecodeGetStructMbrInfo parses the StructMemberInfo response.
// Layout (empirical, see c/src/tagdb.c::smi_read):
//
//	+0x00 char[44]  name
//	+0x2C uint16    data_type
//	+0x30 uint16    struct_id
//	+0x34 uint32    byte_size
//	+0x38 uint32    offset
//	+0x40 uint32    array_count   (N for SINT[N]/DINT[N]/...; 0 scalar)
//	+0x44 uint8     flags
func DecodeGetStructMbrInfo(slot []byte) StructMemberInfo {
	p := slot[HdrPayloadStart+8:]
	return StructMemberInfo{
		Name:       readCString(p[:44], 43),
		DataType:   binary.LittleEndian.Uint16(p[0x2C:]),
		StructID:   binary.LittleEndian.Uint16(p[0x30:]),
		ByteSize:   binary.LittleEndian.Uint32(p[0x34:]),
		Offset:     binary.LittleEndian.Uint32(p[0x38:]),
		ArrayCount: binary.LittleEndian.Uint32(p[0x40:]),
		Flags:      p[0x44],
	}
}

// readCString returns the bytes up to (but not including) the first
// NUL, capped at maxLen. If no NUL is found within maxLen, returns
// the full maxLen bytes.
func readCString(buf []byte, maxLen int) string {
	n := 0
	for n < maxLen && n < len(buf) && buf[n] != 0 {
		n++
	}
	return string(buf[:n])
}
