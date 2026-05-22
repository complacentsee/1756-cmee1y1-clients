/*
 * errors.c — bp_strerror() — error code → string mapping.
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
    case BP_ERR_CLIENT_OPEN:   return "shm_open/ftruncate/mmap failed (is bpServer running? --ipc=host set?)";
    case BP_ERR_NO_FREE_SLOT:  return "all 16 slots in use (other clients holding slots)";
    default:                   return "unknown error";
    }
}
