/*
 * message.c — UCMM CIP messaging via OCXcip_MessageSend.
 *
 * The OEM signature is misleading: what it calls "service" is the
 * backplane SLOT NUMBER, and what it calls "encoded_path" is the
 * FULL CIP REQUEST BODY (service + path_size_words + path + body).
 * What it calls "class_or_misc" is the timeout in milliseconds.
 *
 * Wire format RE'd from:
 *   - libocxbpapi-w.so OCXcip_MessageSend at 0x109ec0  (IPC wrapper)
 *   - libocxbpeng.so.2.3 OC_bpMessageSend at 0x19bf84  (engine queue)
 *   - libocxbpeng.so.2.3 um_ProcessClientRequest at 0x18c518
 *       which builds an APex CB with:
 *         CB+0x1C = slot,  CB+0x1D = 0x0D (UCMM opcode),
 *         CB+0x22 = 0x1C (cb_flags),  CB+0x24 = 0x82 (PENDING),
 *         CB+0x10 = timeout_ms * 1000  (microseconds)
 *       and posts to MBOX_LOOPBACK (0x200) for the chip to send.
 * Corroborated by the sibling apex2d daemon's apex2_asic_send_ucmm.
 *
 * IPC slot layout (payload_size = 0x32088, unchanged from prior versions):
 *   slot + 0x00078  req_bytes (req_size bytes, max 500) — copied into the
 *                   chip's UCMM TX buffer verbatim
 *   slot + 0x19078  uint16  req_size (byte count)
 *   slot + 0x1907a  response data buffer (out, length at +0x3207a)
 *   slot + 0x3207a  uint16  resp_capacity (in) / resp_len (out)
 *   slot + 0x3207c  uint32  status (out)
 *   slot + 0x32080  uint8   slot number (0..0x13, NOT a CIP service)
 *   slot + 0x32082  uint16  timeout_ms  (engine clamps min to 26)
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

#define MSGSEND_REQ_OFF         0x00078u
#define MSGSEND_REQ_SIZE_OFF    0x19078u
#define MSGSEND_RESPDATA_OFF    0x1907au
#define MSGSEND_RESPLEN_OFF     0x3207au
#define MSGSEND_STATUS_OFF      0x3207cu
#define MSGSEND_SLOT_OFF        0x32080u
#define MSGSEND_TIMEOUT_OFF     0x32082u
#define MSGSEND_PAYLOAD_SIZE    0x32088u

static void msg_fill(uint8_t *slot, void *user) {
    bp_message_t *m = user;
    memcpy(slot + MSGSEND_REQ_OFF, m->cip_request, m->req_size);
    bp_st_u16(slot + MSGSEND_REQ_SIZE_OFF, m->req_size);
    bp_st_u16(slot + MSGSEND_RESPLEN_OFF,  m->resp_capacity);
    *(slot + MSGSEND_SLOT_OFF) = m->slot;
    bp_st_u16(slot + MSGSEND_TIMEOUT_OFF, m->timeout_ms);
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
    if (!m->cip_request || m->req_size == 0
        || m->req_size > BP_MSG_MAX_REQ)     return BP_ERR_PARAM_RANGE;
    if (!m->resp_data || m->resp_capacity == 0) return BP_ERR_NULL_ARG;
    if (m->slot > BP_MSG_MAX_SLOT)           return BP_ERR_SLOT_TOO_LARGE;

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
