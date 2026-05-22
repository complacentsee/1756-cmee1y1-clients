// Package shm wires the host process into the bpServer's POSIX
// shared-memory transport at /dev/shm/bpShmem.
//
// Constants mirror c/src/proto.h byte-for-byte; see docs/protocol.md
// for the spec.
//
// SPDX-License-Identifier: MIT
package shm

// Shared-memory geometry.
const (
	ShmPath      = "/dev/shm/bpShmem"
	ShmTotalSize = 0x4B0000 // 16 slots × 0x4B000
	SlotCount    = 16
	SlotStride   = 0x4B000
)

// Named-semaphore names. The 16 req/resp pairs are formatted with %02d.
const (
	SemShmLock     = "/bpShm"
	SemReqPrefix   = "/bpReq"
	SemRespPrefix  = "/bpResp"
)

// Slot header offsets (from slot start).
const (
	HdrOpcode       = 0x00
	HdrPayloadSize  = 0x04
	HdrFnName       = 0x08 // 63 bytes + NUL at +0x47
	HdrClientPID    = 0x48
	HdrIsDocker     = 0x4C
	HdrErrorcode    = 0x50
	HdrSlotOwner    = 0x58
	HdrSlotNumber   = 0x60
	HdrPayloadStart = 0x78
)

// Header constants.
const (
	OpcodeCIP        = 0x00CA
	PendingErrorBits = 0xFFFFFED3 // int32(-301) as uint32 bit pattern
)
