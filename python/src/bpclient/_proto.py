"""Wire-protocol constants for the bpServer slot transport.

Mirrors c/src/proto.h and docs/protocol.md byte-for-byte.  Used by
the IPC layer (_ipc.py) and every opcode module.

SPDX-License-Identifier: MIT
"""

# Shared-memory geometry
SHM_PATH = "/dev/shm/bpShmem"
SHM_TOTAL_SIZE = 0x4B0000          # 16 slots × 0x4B000
SLOT_COUNT = 16
SLOT_STRIDE = 0x4B000

# Named semaphores
SEM_SHMLOCK = "/bpShm"
SEM_REQ_FMT = "/bpReq{:02d}"
SEM_RESP_FMT = "/bpResp{:02d}"

# Slot header offsets (from slot start)
HDR_OPCODE = 0x00
HDR_PAYLOAD_SIZE = 0x04
HDR_FN_NAME = 0x08                 # 63 bytes + NUL at +0x47
HDR_CLIENT_PID = 0x48
HDR_IS_DOCKER = 0x4C
HDR_ERRORCODE = 0x50
HDR_SLOT_OWNER = 0x58
HDR_SLOT_NUMBER = 0x60
HDR_PAYLOAD_START = 0x78

# Header constants
OPCODE_CIP = 0x00CA
PENDING_ERROR_BITS = 0xFFFFFED3    # int32(-301) as uint32

# AccessTagData layout (within a slot, after the header)
TAGDATA_SERVICE_OFF = 0x178        # uint16 service (unused by engine)
TAGDATA_COUNT_OFF = 0x17A          # uint16 request count
TAGDATA_REQ0_START = 0x180
TAGDATA_REQ_STRIDE = 0x120

# Within a request descriptor (relative to descriptor start)
REQ_TAGNAME_OFF = 0x000
REQ_DATATYPE_OFF = 0x100
REQ_ELEM_BYTE_SIZE_OFF = 0x102     # vendor header called this "count" — wrong
REQ_ACTION_OFF = 0x104             # 1=read, 2=write
REQ_ELEM_COUNT_OFF = 0x106         # vendor header called this "elem_size" — wrong
REQ_HAS_EXTRA_OFF = 0x108
REQ_DATA_PTR_OFF = 0x110
REQ_RESULT_OFF = 0x118

# MessageSend layout
MSGSEND_REQ_OFF = 0x00078
MSGSEND_REQ_SIZE_OFF = 0x19078
MSGSEND_RESPDATA_OFF = 0x1907A
MSGSEND_RESPLEN_OFF = 0x3207A
MSGSEND_STATUS_OFF = 0x3207C
MSGSEND_SLOT_OFF = 0x32080
MSGSEND_TIMEOUT_OFF = 0x32082
MSGSEND_PAYLOAD_SIZE = 0x32088

MSG_MAX_SLOT = 0x13                # engine validates < 0x14
MSG_MAX_REQ = 500                  # engine validates path_size <= 500
MSG_MIN_TIMEOUT = 26               # engine clamps below this

# OCXcip_TxRx* slot layout (v0.6 and earlier).  v0.7.0+ bypasses
# these opcodes entirely — txrx_* on Client routes through
# message_send instead.  Constants kept only for TXRX_MAX_PATH
# (still used as the path-size validation cap).
TXRX_MAX_PATH = 0xFF

# Large Forward Open / Forward_Close defaults (v0.7.0+).
# See docs/protocol.md "Connected messaging — wire format".
LFO_DEFAULT_OT_SIZE = 4000          # SDK default O→T/T→O size
LFO_MAX_OT_SIZE = 4002              # absolute hardware ceiling
LFO_VENDOR_ID = 0x0001              # Rockwell
LFO_RPI_US = 10_000_000             # 10 s, matches sibling FO_OT_RPI_US
_LFO_PARAMS_HI = 0x42000000         # P2P + variable
_LFO_OT_HINT = 0x80010000
_LFO_TO_HINT = 0x80000001
TXRX_MAX_CONNS = 16                 # per Client

# CIP atomic type codes (low 13 bits of data_type)
TYPE_BOOL = 0xC1
TYPE_SINT = 0xC2
TYPE_INT = 0xC3
TYPE_DINT = 0xC4
TYPE_LINT = 0xC5
TYPE_USINT = 0xC6
TYPE_UINT = 0xC7
TYPE_UDINT = 0xC8
TYPE_ULINT = 0xC9
TYPE_REAL = 0xCA
TYPE_LREAL = 0xCB
TYPE_BIT_ARRAY = 0xD3              # Logix BOOL[] packed 32 bits per DWORD

# AccessTagData actions
ACTION_READ = 1
ACTION_WRITE = 2
