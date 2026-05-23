// Package cip holds per-opcode wire encoders/decoders for the
// bpServer slot transport. Every function takes a slot byte slice
// (full SlotStride bytes) and writes or reads at fixed offsets that
// mirror c/src/proto.h byte-for-byte. See docs/protocol.md for the
// authoritative spec.
//
// SPDX-License-Identifier: MIT
package cip

// fn_name strings and payload_sizes per opcode. These pair with
// shm.CallSpec.FnName + .PayloadSize on every Call(). Values come
// from RE traces against libocxbpapi-w.so / libocxbpeng.so.2.3 —
// see the per-file source comments in c/src/*.c.
const (
	FnOpen                  = "OCXcip_Open"
	SizeOpen         uint32 = 0x80

	FnClose                  = "OCXcip_Close"
	SizeClose         uint32 = 0x78

	FnDummy                  = "OCXcip_Dummy"
	SizeDummy         uint32 = 0x78

	FnErrorString                  = "OCXcip_ErrorString"
	SizeErrorString         uint32 = 0xD0

	FnCreateTagDbHandle        = "OCXcip_CreateTagDbHandle"
	SizeCreateTagDbHandle uint32 = 0x180

	FnDeleteTagDbHandle        = "OCXcip_DeleteTagDbHandle"
	SizeDeleteTagDbHandle uint32 = 0x80

	FnBuildTagDb        = "OCXcip_BuildTagDb"
	SizeBuildTagDb uint32 = 0x80

	FnTestTagDbVer        = "OCXcip_TestTagDbVer"
	SizeTestTagDbVer uint32 = 0x80

	FnGetSymbolInfo        = "OCXcip_GetSymbolInfo"
	SizeGetSymbolInfo uint32 = 0x100

	FnGetStructInfo        = "OCXcip_GetStructInfo"
	SizeGetStructInfo uint32 = 0xB8

	FnGetStructMbrInfo        = "OCXcip_GetStructMbrInfo"
	SizeGetStructMbrInfo uint32 = 0xD0

	FnAccessTagData = "OCXcip_AccessTagData"
	// SizeAccessTagData is computed per-call:
	//   0x2A0 + (count-1)*0x120 + total_data_bytes

	FnAccessTagDataDb = "OCXcip_AccessTagDataDb"
	// SizeAccessTagDataDb is computed per-call:
	//   0x1B0 + (count-1)*0x128 + total_data_bytes
	// (v0.10.4+ peer of AccessTagData using cached db_handle —
	// saves the per-call path-string marshalling.)

	FnGetIdObject        = "OCXcip_GetIdObject"
	SizeGetIdObject uint32 = 0xA8

	FnGetDeviceIdObject        = "OCXcip_GetDeviceIdObject"
	SizeGetDeviceIdObject uint32 = 0x1B0

	FnGetDeviceIdStatus        = "OCXcip_GetDeviceIdStatus"
	SizeGetDeviceIdStatus uint32 = 0x180

	FnGetWCTime           = "OCXcip_GetWCTime"
	FnGetWCTimeUTC        = "OCXcip_GetWCTimeUTC"
	FnSetWCTime           = "OCXcip_SetWCTime"
	FnSetWCTimeUTC        = "OCXcip_SetWCTimeUTC"
	SizeWCTime     uint32 = 0x1B0

	FnGetExDevObject              = "OCXcip_GetExDevObject"
	SizeGetExDevObject     uint32 = 0x260
	ExDevObjectBytes              = 226

	FnGetDeviceICPObject          = "OCXcip_GetDeviceICPObject"
	SizeGetDeviceICPObject uint32 = 0x190
	DeviceICPObjectBytes          = 20

	FnGetActiveNodeTable        = "OCXcip_GetActiveNodeTable"
	SizeGetActiveNodeTable uint32 = 0x80

	FnGetSwitchPosition        = "OCXcip_GetSwitchPosition"
	SizeGetSwitchPosition uint32 = 0x80

	FnGetLED        = "OCXcip_GetLED"
	SizeGetLED uint32 = 0x80

	FnSetLED        = "OCXcip_SetLED"
	SizeSetLED uint32 = 0x80

	FnGetDisplay        = "OCXcip_GetDisplay"
	SizeGetDisplay uint32 = 0x80

	FnSetDisplay        = "OCXcip_SetDisplay"
	SizeSetDisplay uint32 = 0x80

	FnMessageSend        = "OCXcip_MessageSend"
	SizeMessageSend uint32 = 0x32088

	// OCXcip_TxRx* opcodes are NOT dispatched by the v0.7.0+ SDK —
	// they resolve to OCXCN_* in a library missing from cm1756.
	// The SDK builds Large Forward Open / Forward_Close bodies in
	// cip/conn.go and sends them via OCXcip_MessageSend.

	// ParsePath is a debug/RE opcode (not in the C public API).
	// Surfaced for cmd/pathprobe; see c/examples/pathprobe.c.
	FnParsePath        = "OCXcip_ParsePath"
	SizeParsePath uint32 = 0x288
)

// HdrPayloadStart is duplicated here so cip callers don't need a
// reverse dep on shm/. Stays in sync with shm.HdrPayloadStart.
const HdrPayloadStart = 0x78
