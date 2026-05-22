// SPDX-License-Identifier: MIT

package ocxbp

import (
	"encoding/binary"
	"fmt"
	"math"
	"strings"
)

// readTagsBatchCap is the per-AccessTagData chunk size.  The engine's
// hard cap on requests-per-call hasn't been characterized; 16 is the
// conservative value the existing Access() helper enforces, so
// ReadTags chunks at the same boundary.
const readTagsBatchCap = 16

// scalarKind is the typed-decode variant produced by ReadTags for one
// symbol.  Mirrors C bp_value_kind_t.
type scalarKind int

const (
	kindNone scalarKind = iota
	kindBool
	kindSint
	kindInt
	kindDint
	kindLint
	kindUsint
	kindUint
	kindUdint
	kindUlint
	kindReal
	kindLreal
)

func kindForAtomic(dataType uint16, elemByteSize uint32) scalarKind {
	switch dataType & 0x1FFF {
	case uint16(TypeBool):
		if elemByteSize == 1 {
			return kindBool
		}
	case uint16(TypeSint):
		if elemByteSize == 1 {
			return kindSint
		}
	case uint16(TypeInt):
		if elemByteSize == 2 {
			return kindInt
		}
	case uint16(TypeDint):
		if elemByteSize == 4 {
			return kindDint
		}
	case uint16(TypeLint):
		if elemByteSize == 8 {
			return kindLint
		}
	case uint16(TypeUsint):
		if elemByteSize == 1 {
			return kindUsint
		}
	case uint16(TypeUint):
		if elemByteSize == 2 {
			return kindUint
		}
	case uint16(TypeUdint):
		if elemByteSize == 4 {
			return kindUdint
		}
	case uint16(TypeUlint):
		if elemByteSize == 8 {
			return kindUlint
		}
	case uint16(TypeReal):
		if elemByteSize == 4 {
			return kindReal
		}
	case uint16(TypeLreal):
		if elemByteSize == 8 {
			return kindLreal
		}
	}
	return kindNone
}

func decodeScalar(kind scalarKind, b []byte) interface{} {
	switch kind {
	case kindBool:
		return b[0] != 0
	case kindSint:
		return int8(b[0])
	case kindUsint:
		return b[0]
	case kindInt:
		return int16(binary.LittleEndian.Uint16(b))
	case kindUint:
		return binary.LittleEndian.Uint16(b)
	case kindDint:
		return int32(binary.LittleEndian.Uint32(b))
	case kindUdint:
		return binary.LittleEndian.Uint32(b)
	case kindReal:
		return math.Float32frombits(binary.LittleEndian.Uint32(b))
	case kindLint:
		return int64(binary.LittleEndian.Uint64(b))
	case kindUlint:
		return binary.LittleEndian.Uint64(b)
	case kindLreal:
		return math.Float64frombits(binary.LittleEndian.Uint64(b))
	}
	return nil
}

// TagReadResult is one entry's outcome in a ReadTags call.  On success
// the Value field holds the decoded scalar (bool / int8..int64 /
// uint8..uint64 / float32 / float64) and CIPStatus is 0.  On per-tag
// failure CIPStatus is the CIP General Status and Value is nil.
type TagReadResult struct {
	Value     interface{}
	CIPStatus uint32
}

// ReadTags resolves each name via the per-client symbol cache, batches
// requests into chunks of `readTagsBatchCap` (one OCXcip_AccessTagData
// call per chunk), and returns a map of name → TagReadResult.
//
// Scope (v0.9.0): scalars only.  Arrays or UDTs (incl. STRING family)
// return ErrParamRange before any IPC.
//
// Whole-batch failure: if any per-tag CIP General Status is non-zero,
// ReadTags returns an error wrapping the first failing name.  The
// returned map is still populated — callers inspect each entry's
// CIPStatus to find the failures.
func (db *TagDB) ReadTags(names []string) (map[string]TagReadResult, error) {
	if db == nil {
		return nil, ErrNullArg
	}
	if len(names) == 0 {
		return map[string]TagReadResult{}, nil
	}

	// Resolve every name + classify before issuing any IPC.  Rejects
	// arrays / UDTs / unknown names with a single round-trip cost.
	kinds := make([]scalarKind, len(names))
	infos := make([]SymbolInfo, len(names))
	for i, name := range names {
		info, err := db.LookupSymbol(name)
		if err != nil {
			return nil, fmt.Errorf("ocxbp: ReadTags: %q: %w", name, err)
		}
		if info.Dim0 != 0 || info.StructType != 0 {
			return nil, fmt.Errorf("ocxbp: ReadTags: %q: arrays/UDTs not supported in v0.9.0 (use ReadDINTArray / ReadString): %w", name, ErrParamRange)
		}
		kind := kindForAtomic(info.DataType, info.ElemByteSize)
		if kind == kindNone {
			return nil, fmt.Errorf("ocxbp: ReadTags: %q: unsupported data_type 0x%04x: %w", name, info.DataType, ErrParamRange)
		}
		kinds[i] = kind
		infos[i] = info
	}

	out := make(map[string]TagReadResult, len(names))
	var failedNames []string

	for start := 0; start < len(names); start += readTagsBatchCap {
		end := start + readTagsBatchCap
		if end > len(names) {
			end = len(names)
		}
		reqs := make([]TagRequest, end-start)
		bufs := make([][]byte, end-start)
		for j := range reqs {
			i := start + j
			bufs[j] = make([]byte, infos[i].ElemByteSize)
			reqs[j] = TagRequest{
				TagName:      names[i],
				DataType:     infos[i].DataType,
				ElemByteSize: uint16(infos[i].ElemByteSize),
				Action:       ActionRead,
				ElemCount:    1,
				Data:         bufs[j],
			}
		}
		if err := db.Access(reqs); err != nil {
			return out, fmt.Errorf("ocxbp: ReadTags: AccessTagData chunk failed: %w", err)
		}
		for j := range reqs {
			i := start + j
			res := TagReadResult{CIPStatus: reqs[j].Result}
			if reqs[j].Result == 0 {
				res.Value = decodeScalar(kinds[i], bufs[j])
			} else {
				failedNames = append(failedNames, names[i])
			}
			out[names[i]] = res
		}
	}

	if len(failedNames) > 0 {
		return out, fmt.Errorf("ocxbp: ReadTags: %d tag(s) returned non-zero CIP status (first: %q): %s",
			len(failedNames), failedNames[0], strings.Join(failedNames, ", "))
	}
	return out, nil
}

// TagWriteResult is one entry's outcome in a WriteTags call.
type TagWriteResult struct {
	CIPStatus uint32
}

// encodeForKind encodes `v` into a small byte buffer matching the
// given scalar kind.  Returns the bytes + true on success, nil +
// false if `v`'s Go type doesn't match the expected kind or is out
// of range for the destination type.
func encodeForKind(kind scalarKind, v interface{}) ([]byte, bool) {
	switch kind {
	case kindBool:
		b, ok := v.(bool)
		if !ok {
			return nil, false
		}
		if b {
			return []byte{1}, true
		}
		return []byte{0}, true
	case kindSint:
		switch x := v.(type) {
		case int8:
			return []byte{byte(x)}, true
		}
	case kindUsint:
		switch x := v.(type) {
		case uint8:
			return []byte{x}, true
		}
	case kindInt:
		if x, ok := v.(int16); ok {
			b := make([]byte, 2)
			binary.LittleEndian.PutUint16(b, uint16(x))
			return b, true
		}
	case kindUint:
		if x, ok := v.(uint16); ok {
			b := make([]byte, 2)
			binary.LittleEndian.PutUint16(b, x)
			return b, true
		}
	case kindDint:
		if x, ok := v.(int32); ok {
			b := make([]byte, 4)
			binary.LittleEndian.PutUint32(b, uint32(x))
			return b, true
		}
	case kindUdint:
		if x, ok := v.(uint32); ok {
			b := make([]byte, 4)
			binary.LittleEndian.PutUint32(b, x)
			return b, true
		}
	case kindReal:
		if x, ok := v.(float32); ok {
			b := make([]byte, 4)
			binary.LittleEndian.PutUint32(b, math.Float32bits(x))
			return b, true
		}
		// Accept float64 → float32 if it fits cleanly.
		if x, ok := v.(float64); ok {
			f := float32(x)
			b := make([]byte, 4)
			binary.LittleEndian.PutUint32(b, math.Float32bits(f))
			return b, true
		}
	case kindLint:
		if x, ok := v.(int64); ok {
			b := make([]byte, 8)
			binary.LittleEndian.PutUint64(b, uint64(x))
			return b, true
		}
	case kindUlint:
		if x, ok := v.(uint64); ok {
			b := make([]byte, 8)
			binary.LittleEndian.PutUint64(b, x)
			return b, true
		}
	case kindLreal:
		if x, ok := v.(float64); ok {
			b := make([]byte, 8)
			binary.LittleEndian.PutUint64(b, math.Float64bits(x))
			return b, true
		}
		if x, ok := v.(float32); ok {
			b := make([]byte, 8)
			binary.LittleEndian.PutUint64(b, math.Float64bits(float64(x)))
			return b, true
		}
	}
	return nil, false
}

// WriteTags writes `values` (a map of tag name → typed Go value) to
// the PLC.  Each value's Go type must match the symbol's data_type
// (int32 for DINT, float32 for REAL, bool for BOOL, etc.); mismatches
// are rejected pre-IPC with ErrParamRange and a stderr line naming
// the failing tag.
//
// Returns nil if all writes succeeded; an error naming the first
// failing tag if any per-tag CIP General Status is non-zero.
//
// Scope (v0.9.0): scalars only.  Arrays, UDTs, STRING family raise
// ErrParamRange before any IPC.
func (db *TagDB) WriteTags(values map[string]interface{}) error {
	if db == nil {
		return ErrNullArg
	}
	if len(values) == 0 {
		return nil
	}
	// Order matters for chunking — collect keys in a stable order.
	names := make([]string, 0, len(values))
	for k := range values {
		names = append(names, k)
	}

	// Pre-IPC validation + encoding.
	encoded := make([][]byte, len(names))
	infos := make([]SymbolInfo, len(names))
	for i, name := range names {
		info, err := db.LookupSymbol(name)
		if err != nil {
			return fmt.Errorf("ocxbp: WriteTags: %q: %w", name, err)
		}
		if info.Dim0 != 0 || info.StructType != 0 {
			return fmt.Errorf("ocxbp: WriteTags: %q: arrays/UDTs not supported in v0.9.0: %w", name, ErrParamRange)
		}
		kind := kindForAtomic(info.DataType, info.ElemByteSize)
		if kind == kindNone {
			return fmt.Errorf("ocxbp: WriteTags: %q: unsupported data_type 0x%04x: %w", name, info.DataType, ErrParamRange)
		}
		b, ok := encodeForKind(kind, values[name])
		if !ok {
			return fmt.Errorf("ocxbp: WriteTags: %q: Go value type %T doesn't match symbol kind %d (data_type 0x%04x): %w",
				name, values[name], int(kind), info.DataType, ErrParamRange)
		}
		encoded[i] = b
		infos[i] = info
	}

	var failedNames []string
	for start := 0; start < len(names); start += readTagsBatchCap {
		end := start + readTagsBatchCap
		if end > len(names) {
			end = len(names)
		}
		reqs := make([]TagRequest, end-start)
		for j := range reqs {
			i := start + j
			reqs[j] = TagRequest{
				TagName:      names[i],
				DataType:     infos[i].DataType,
				ElemByteSize: uint16(infos[i].ElemByteSize),
				Action:       ActionWrite,
				ElemCount:    1,
				Data:         encoded[i],
			}
		}
		if err := db.Access(reqs); err != nil {
			return fmt.Errorf("ocxbp: WriteTags: AccessTagData chunk failed: %w", err)
		}
		for j := range reqs {
			if reqs[j].Result != 0 {
				failedNames = append(failedNames, reqs[j].TagName)
			}
		}
	}
	if len(failedNames) > 0 {
		return fmt.Errorf("ocxbp: WriteTags: %d tag(s) returned non-zero CIP status (first: %q): %s",
			len(failedNames), failedNames[0], strings.Join(failedNames, ", "))
	}
	return nil
}
