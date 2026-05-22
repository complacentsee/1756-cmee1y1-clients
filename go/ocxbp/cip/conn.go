// SPDX-License-Identifier: MIT

package cip

import "encoding/binary"

// Class-3 connected messaging (TxRxOpenConn / TxRxMsg / TxRxCloseConn)
// slot layout. Mirrors c/src/conn.c offsets.
//
// STATUS: NOT FUNCTIONAL on cm1756 — OCXcip_TxRxOpenConn always
// returns engine code 0x1001 because the OCXCN_OpenClass3Connection
// PLT thunk in libocxbpapi.so.2.3 points to an OCXCN_* library that
// is not present anywhere on the cm1756 image. See docs/protocol.md
// "Connected messaging — open issues". Workaround for callers that
// need a real connection: build a Large Forward Open (CIP svc 0x5B)
// and send it via the regular MessageSend path.
const (
	TxRxAppHandleOff   = 0x00078
	TxRxOptionsOff     = 0x0007C
	TxRxPathOff        = 0x00080
	TxRxPathSizeOff    = 0x00180
	TxRxConnParamsOff  = 0x00182 // OpenConn / Msg only
	TxRxConnIDOff      = 0x00184 // OpenConn response
	TxRxConnSerialOff  = 0x00186 // OpenConn response

	TxRxMsgReqBufOff   = 0x00184
	TxRxMsgReqSizeOff  = 0x19084
	TxRxMsgRespBufOff  = 0x19086
	TxRxMsgRespLenOff  = 0x32186

	TxRxMaxPath = 0xFF
)

// ConnSpec is the caller-managed connection descriptor; the OEM API
// duplicates the same fields across Open / Msg / Close calls.
type ConnSpec struct {
	AppHandle   uint16
	Options     uint32
	EncodedPath []byte
	PathSize    uint16
	ConnParams  uint16
}

// EncodeTxRxOpenConn lays the OpenConn request into the slot.
func EncodeTxRxOpenConn(slot []byte, spec *ConnSpec) {
	binary.LittleEndian.PutUint16(slot[TxRxAppHandleOff:], spec.AppHandle)
	binary.LittleEndian.PutUint32(slot[TxRxOptionsOff:], spec.Options)
	copy(slot[TxRxPathOff:TxRxPathOff+int(spec.PathSize)], spec.EncodedPath[:spec.PathSize])
	binary.LittleEndian.PutUint16(slot[TxRxPathSizeOff:], spec.PathSize)
	binary.LittleEndian.PutUint16(slot[TxRxConnParamsOff:], spec.ConnParams)
}

// DecodeTxRxOpenConn reads (conn_id, conn_serial) from the response.
func DecodeTxRxOpenConn(slot []byte) (connID, connSerial uint16) {
	connID = binary.LittleEndian.Uint16(slot[TxRxConnIDOff:])
	connSerial = binary.LittleEndian.Uint16(slot[TxRxConnSerialOff:])
	return
}

// EncodeTxRxMsg lays the per-call request into the slot.
func EncodeTxRxMsg(slot []byte, spec *ConnSpec, req []byte, reqSize, respCap uint16) {
	binary.LittleEndian.PutUint16(slot[TxRxAppHandleOff:], spec.AppHandle)
	binary.LittleEndian.PutUint32(slot[TxRxOptionsOff:], spec.Options)
	copy(slot[TxRxPathOff:TxRxPathOff+int(spec.PathSize)], spec.EncodedPath[:spec.PathSize])
	binary.LittleEndian.PutUint16(slot[TxRxPathSizeOff:], spec.PathSize)
	binary.LittleEndian.PutUint16(slot[TxRxConnParamsOff:], spec.ConnParams)
	if reqSize > 0 {
		copy(slot[TxRxMsgReqBufOff:TxRxMsgReqBufOff+int(reqSize)], req[:reqSize])
	}
	binary.LittleEndian.PutUint16(slot[TxRxMsgReqSizeOff:], reqSize)
	binary.LittleEndian.PutUint16(slot[TxRxMsgRespLenOff:], respCap)
}

// DecodeTxRxMsg copies the response into resp[:] and returns the
// actual byte count the engine wrote.
func DecodeTxRxMsg(slot []byte, resp []byte, respCap uint16) uint16 {
	got := binary.LittleEndian.Uint16(slot[TxRxMsgRespLenOff:])
	if got > respCap {
		got = respCap
	}
	if len(resp) > 0 && got > 0 {
		n := int(got)
		if n > len(resp) {
			n = len(resp)
		}
		copy(resp[:n], slot[TxRxMsgRespBufOff:TxRxMsgRespBufOff+n])
	}
	return got
}

// EncodeTxRxCloseConn lays the Close request (no conn_params byte).
func EncodeTxRxCloseConn(slot []byte, spec *ConnSpec) {
	binary.LittleEndian.PutUint16(slot[TxRxAppHandleOff:], spec.AppHandle)
	binary.LittleEndian.PutUint32(slot[TxRxOptionsOff:], spec.Options)
	copy(slot[TxRxPathOff:TxRxPathOff+int(spec.PathSize)], spec.EncodedPath[:spec.PathSize])
	binary.LittleEndian.PutUint16(slot[TxRxPathSizeOff:], spec.PathSize)
}
