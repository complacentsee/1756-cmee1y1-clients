/*
 * message.c — OCXcip_MessageSend wrapper (unconnected CIP UCMM).
 *
 * Wire format RE'd from:
 *   - libocxbpapi-w.so OCXcip_MessageSend at 0x109ec0  (wrapper side)
 *   - libocxbpeng.so.2.3 OC_bpMessageSend at 0x19bf84  (engine side)
 *
 * Slot layout (payload_size = 0x32088):
 *   slot + 0x00078  encoded_path bytes (path_size bytes, max 500)
 *   slot + 0x19078  uint16  path_size (byte count)
 *   slot + 0x1907a  response data buffer (out, length at +0x3207a)
 *   slot + 0x3207a  uint16  resp_capacity (in) / resp_len (out)
 *   slot + 0x3207c  uint32  status (out)
 *   slot + 0x32080  uint8   service code
 *   slot + 0x32082  uint16  class_or_misc
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

#define MSGSEND_PATH_OFF        0x00078u
#define MSGSEND_PATH_SIZE_OFF   0x19078u
#define MSGSEND_RESPDATA_OFF    0x1907au
#define MSGSEND_RESPLEN_OFF     0x3207au
#define MSGSEND_STATUS_OFF      0x3207cu
#define MSGSEND_SERVICE_OFF     0x32080u
#define MSGSEND_CLASSMISC_OFF   0x32082u
#define MSGSEND_PAYLOAD_SIZE    0x32088u
#define MSGSEND_MAX_PATH        500u   /* engine validates 1..500 inclusive */

static void msg_fill(uint8_t *slot, void *user) {
    bp_message_t *m = user;
    memcpy(slot + MSGSEND_PATH_OFF, m->encoded_path, m->path_size);
    bp_st_u16(slot + MSGSEND_PATH_SIZE_OFF, m->path_size);
    bp_st_u16(slot + MSGSEND_RESPLEN_OFF,   m->resp_capacity);
    *(slot + MSGSEND_SERVICE_OFF) = m->service;
    bp_st_u16(slot + MSGSEND_CLASSMISC_OFF, m->class_or_misc);
}

static void msg_read(uint8_t *slot, void *user) {
    bp_message_t *m = user;
    uint16_t got = bp_ld_u16(slot + MSGSEND_RESPLEN_OFF);
    m->status = bp_ld_u32(slot + MSGSEND_STATUS_OFF);
    if (got > m->resp_capacity) got = m->resp_capacity;
    m->resp_len = got;
    if (m->resp_data && got > 0) {
        memcpy(m->resp_data, slot + MSGSEND_RESPDATA_OFF, got);
    }
}

int bp_client_message_send(bp_client_t *c, bp_message_t *m) {
    if (!c || !m)                            return BP_ERR_NULL_ARG;
    if (!m->encoded_path || m->path_size == 0
        || m->path_size > MSGSEND_MAX_PATH)  return BP_ERR_PARAM_RANGE;
    if (!m->resp_data || m->resp_capacity == 0) return BP_ERR_NULL_ARG;

    m->resp_len = 0;
    m->status   = 0;

    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_MessageSend",
        .payload_size = MSGSEND_PAYLOAD_SIZE,
        .fill_payload = msg_fill,
        .read_reply   = msg_read,
        .timeout_ms   = 5000,
        .user         = m,
    };
    return bp_client_call(c, &spec);
}
