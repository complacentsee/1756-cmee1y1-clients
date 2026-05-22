/*
 * errors.c — bp_strerror() + bp_cip_status_string().
 *
 * SPDX-License-Identifier: MIT
 */
#include "../include/bpclient.h"

const char *bp_strerror(int rc) {
    switch (rc) {
    case BP_OK:                return "ok";
    case BP_ERR_GENERIC:       return "generic failure";
    case BP_ERR_SEND_REQUEST:  return "sem_post on bpReq failed";
    case BP_ERR_RECV_ANSWER:   return "sem_wait on bpResp failed (server crashed?)";
    case BP_ERR_NULL_ARG:      return "null argument";
    case BP_ERR_PENDING:       return "still pending (server hasn't replied — server crash?)";
    case BP_ERR_NOT_OPEN:      return "not open / Open() not called or IPC lost";
    case BP_ERR_PARAM_RANGE:   return "parameter range error (check path string format: P:1,S:2 not 1,2)";
    case BP_ERR_SLOT_TOO_LARGE:return "response too large for slot (reduce batch size)";
    case BP_ERR_CIP_STATUS:    return "CIP-layer rejection (call bp_client_last_cip_error for details)";
    case BP_ERR_CLIENT_OPEN:   return "shm_open/ftruncate/mmap failed (is bpServer running? --ipc=host set?)";
    case BP_ERR_NO_FREE_SLOT:  return "all 16 slots in use (other clients holding slots)";
    default:                   return "unknown error";
    }
}

const char *bp_cip_status_string(uint8_t status, uint16_t ext_status) {
    /* Common CIP General Status codes + the extended-status variants
     * we observe on Logix-family controllers.  Table mirrors
     * docs/error-codes.md "Forward_Open / Forward_Close failure modes"
     * — keep in sync.  Codes the SDK doesn't surface today (most of
     * the Identity / MR / Vendor-specific) fall through to the
     * default. */
    switch (status) {
    case 0x00: return "success";
    case 0x01:
        switch (ext_status) {
        case 0x0100: return "connection in use (stale conn from prior session — let PLC idle-time-out ~40s or restart bpServer)";
        case 0x0103: return "transport class unsupported (controller firmware rejected class 0xA3)";
        case 0x0107: return "connection ID not found in Forward_Close (PLC already cleaned up; safe to ignore on close)";
        case 0x0113: return "no more connections available on target";
        case 0x0114: return "vendor id or product code mismatch in Forward_Close";
        case 0x0115: return "device type mismatch in Forward_Close";
        case 0x0116: return "revision mismatch in Forward_Close";
        case 0x0117: return "non-listen-only connection not opened";
        case 0x0119: return "Forward_Close conn ID mismatch";
        case 0x011A: return "target application out of connections";
        case 0x0203: return "connection timeout";
        case 0x0204: return "Unconnected_Send timeout";
        case 0x0205: return "parameter error in Unconnected_Send";
        case 0x0206: return "message too large for Unconnected_Send";
        case 0x0311: return "port not available";
        case 0x0312: return "link address not available";
        case 0x0315: return "invalid segment type or value in path";
        case 0x0316: return "invalid attribute (connection path malformed)";
        case 0x0317: return "key segment not preceded by port segment";
        case 0x0318: return "link address to self invalid";
        default:     return "connection failure";
        }
    case 0x02: return "resource unavailable (most often: conn_params requesting oversized buffer — try conn_params=0)";
    case 0x03: return "invalid parameter value";
    case 0x04: return "path segment error (bad tag name or EPATH)";
    case 0x05: return "path destination unknown (slot empty, or object doesn't accept this service)";
    case 0x06: return "partial transfer";
    case 0x07: return "connection lost";
    case 0x08: return "service not supported by target object";
    case 0x09: return "invalid attribute value";
    case 0x0A: return "attribute list error";
    case 0x0B: return "already in requested state";
    case 0x0C: return "object state conflict";
    case 0x0D: return "object already exists";
    case 0x0E: return "attribute not settable (write to read-only)";
    case 0x0F: return "privilege violation";
    case 0x10: return "device state conflict";
    case 0x11: return "reply data too large";
    case 0x12: return "fragmentation of primitive value";
    case 0x13: return "not enough data";
    case 0x14: return "attribute not supported";
    case 0x15: return "too much data";
    case 0x16: return "object does not exist";
    case 0x17: return "service fragmentation sequence not in progress";
    case 0x18: return "no stored attribute data";
    case 0x19: return "store operation failure";
    case 0x1A: return "routing failure: request packet too large";
    case 0x1B: return "routing failure: response packet too large";
    case 0x1C: return "missing attribute list entry data";
    case 0x1D: return "invalid attribute value list";
    case 0x1E: return "embedded service error";
    case 0x1F: return "vendor-specific error";
    case 0x20: return "invalid parameter";
    case 0x21: return "write-once value or medium already written";
    case 0x22: return "invalid reply received";
    case 0x25: return "key failure in path";
    case 0x26: return "path size invalid";
    case 0x27: return "unexpected attribute in list";
    case 0x28: return "invalid member id";
    case 0x29: return "member not settable";
    default:   return "unknown CIP status";
    }
}
