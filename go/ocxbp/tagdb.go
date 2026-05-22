// SPDX-License-Identifier: MIT

package ocxbp

import (
	"errors"

	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/cip"
	"github.com/complacentsee/1756-cmee1y1-clients/go/ocxbp/shm"
)

// SymbolInfo is the public symbol descriptor returned by
// TagDB.SymbolAt. Mirrors cip.SymbolInfo / bp_symbol_info_t.
type SymbolInfo = cip.SymbolInfo

// StructInfo is the public UDT descriptor. Mirrors cip.StructInfo /
// bp_struct_info_t.
type StructInfo = cip.StructInfo

// StructMemberInfo is the public UDT member descriptor. Mirrors
// cip.StructMemberInfo / bp_struct_member_info_t.
type StructMemberInfo = cip.StructMemberInfo

// TagDB is a per-PLC tag database handle. Open with Client.OpenTagDB;
// Close releases it via OCXcip_DeleteTagDbHandle.
type TagDB struct {
	client *Client
	handle uint32
	path   string
}

// Handle returns the engine-assigned tag-DB handle. Diagnostic only.
func (db *TagDB) Handle() uint32 { return db.handle }

// Path returns the OldI CIP path string used to open this handle.
func (db *TagDB) Path() string { return db.path }

// OpenTagDB dispatches OCXcip_CreateTagDbHandle for an OldI CIP path
// (e.g. "P:1,S:2"). Path MUST follow the <letter>:<num> format
// joined with commas. Plain Rockwell "1,2" notation is rejected
// with ErrParamRange.
func (c *Client) OpenTagDB(path string) (*TagDB, error) {
	if c == nil || path == "" {
		return nil, ErrNullArg
	}
	if len(path) > 254 {
		return nil, ErrParamRange
	}
	var handle uint32
	err := c.shm.Call(shm.CallSpec{
		FnName:      cip.FnCreateTagDbHandle,
		PayloadSize: cip.SizeCreateTagDbHandle,
		Fill:        func(slot []byte) { cip.EncodeCreateTagDbHandle(slot, path) },
		Read:        func(slot []byte) { handle = cip.DecodeCreateTagDbHandle(slot) },
		TimeoutMs:   5000,
	})
	if err != nil {
		return nil, translateCallErr(err)
	}
	return &TagDB{client: c, handle: handle, path: path}, nil
}

// Close dispatches OCXcip_DeleteTagDbHandle and frees the local
// handle. Safe to call on nil.
func (db *TagDB) Close() {
	if db == nil || db.handle == 0 {
		return
	}
	h := db.handle
	_ = db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnDeleteTagDbHandle,
		PayloadSize: cip.SizeDeleteTagDbHandle,
		Fill:        func(slot []byte) { cip.EncodeHandle(slot, h) },
		TimeoutMs:   5000,
	})
	db.handle = 0
}

// Build walks the PLC's symbol table and returns the enumerated
// symbol count. Typically ~200 ms for a few thousand symbols.
//
// On success, invalidates the per-PLC symbol cache for db.path and
// resizes it for the fresh table.  Lazy fill kicks in on the next
// LookupSymbol; or call PreloadSymbols for eager warm-up.
func (db *TagDB) Build() (uint16, error) {
	if db == nil {
		return 0, ErrNullArg
	}
	var n uint16
	err := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnBuildTagDb,
		PayloadSize: cip.SizeBuildTagDb,
		Fill:        func(slot []byte) { cip.EncodeHandle(slot, db.handle) },
		Read:        func(slot []byte) { n = cip.DecodeBuildTagDb(slot) },
		TimeoutMs:   30000,
	})
	if err == nil {
		db.client.resetCacheAfterBuild(db.path, n)
	}
	return n, translateCallErr(err)
}

// TestVersion asks the PLC whether the tag database has changed
// since the last Build on this handle. Cheap (~5 ms) compared to
// a full Build. Returns true if the caller should rebuild — covers
// both "versions differ" (engine rc 0x14) and "no captured
// version yet" (engine rc 0x15).
func (db *TagDB) TestVersion() (changed bool, err error) {
	if db == nil {
		return false, ErrNullArg
	}
	callErr := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnTestTagDbVer,
		PayloadSize: cip.SizeTestTagDbVer,
		Fill:        func(slot []byte) { cip.EncodeHandle(slot, db.handle) },
		TimeoutMs:   5000,
	})
	if callErr == nil {
		return false, nil
	}
	var ee shm.EngineError
	if errors.As(callErr, &ee) {
		switch int(ee) {
		case 0x14, 0x15:
			return true, nil
		}
	}
	return false, translateCallErr(callErr)
}

// SymbolAt fetches one symbol's descriptor by zero-based index.
// Valid indices: 0 to (Build's count - 1) inclusive.
func (db *TagDB) SymbolAt(index uint16) (SymbolInfo, error) {
	if db == nil {
		return SymbolInfo{}, ErrNullArg
	}
	var out SymbolInfo
	err := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetSymbolInfo,
		PayloadSize: cip.SizeGetSymbolInfo,
		Fill:        func(slot []byte) { cip.EncodeGetSymbolInfo(slot, db.handle, index) },
		Read:        func(slot []byte) { out = cip.DecodeGetSymbolInfo(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

// GetStructInfo fetches a UDT template descriptor by id.
func (db *TagDB) GetStructInfo(structID uint16) (StructInfo, error) {
	if db == nil {
		return StructInfo{}, ErrNullArg
	}
	var out StructInfo
	err := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetStructInfo,
		PayloadSize: cip.SizeGetStructInfo,
		Fill:        func(slot []byte) { cip.EncodeGetStructInfo(slot, db.handle, structID) },
		Read:        func(slot []byte) { out = cip.DecodeGetStructInfo(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

// GetStructMember fetches one member descriptor by (struct_id, member_index).
func (db *TagDB) GetStructMember(structID, memberIndex uint16) (StructMemberInfo, error) {
	if db == nil {
		return StructMemberInfo{}, ErrNullArg
	}
	var out StructMemberInfo
	err := db.client.shm.Call(shm.CallSpec{
		FnName:      cip.FnGetStructMbrInfo,
		PayloadSize: cip.SizeGetStructMbrInfo,
		Fill:        func(slot []byte) { cip.EncodeGetStructMbrInfo(slot, db.handle, structID, memberIndex) },
		Read:        func(slot []byte) { out = cip.DecodeGetStructMbrInfo(slot) },
		TimeoutMs:   5000,
	})
	return out, translateCallErr(err)
}

// IsMemberArray reports whether the struct member is an array (flags bit 0x08).
func IsMemberArray(m StructMemberInfo) bool { return (m.Flags & 0x08) != 0 }

// IsMemberStruct reports whether the struct member is itself a UDT.
func IsMemberStruct(m StructMemberInfo) bool { return m.StructID != 0 }
