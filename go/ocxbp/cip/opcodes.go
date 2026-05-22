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

	FnGetIdObject        = "OCXcip_GetIdObject"
	SizeGetIdObject uint32 = 0xA8

	FnGetDeviceIdObject        = "OCXcip_GetDeviceIdObject"
	SizeGetDeviceIdObject uint32 = 0x1B0

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

	FnTxRxOpenConn        = "OCXcip_TxRxOpenConn"
	SizeTxRxOpenConn uint32 = 0x188

	FnTxRxMsg        = "OCXcip_TxRxMsg"
	SizeTxRxMsg uint32 = 0x32190

	FnTxRxCloseConn        = "OCXcip_TxRxCloseConn"
	SizeTxRxCloseConn uint32 = 0x188
)

// HdrPayloadStart is duplicated here so cip callers don't need a
// reverse dep on shm/. Stays in sync with shm.HdrPayloadStart.
const HdrPayloadStart = 0x78
