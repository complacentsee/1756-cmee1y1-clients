/*
 * struct.c — structured (whole-UDT) tag access via CIP Read Tag (0x4C) /
 *            Write Tag (0x4D) over the raw MessageSend (UCMM) path.
 *            Public API: bp_client_read_struct / bp_client_write_struct.
 *
 * Reads / writes a whole Logix UDT ("structure") instance as ONE CIP
 * transaction — atomic on the controller — using the 2-byte
 * abbreviated-structure template handle (type code 0x02A0).  Mirrors
 * the Go SDK's (*Client).ReadStruct / WriteStruct in go/ocxbp/struct.go
 * byte-for-byte; both were validated against a live L85 (handle
 * 0x0A2C round-trip).
 *
 * Wire shapes (CIP request body — service, path_size_words, path, body):
 *
 *   Read Tag (0x4C):  [0x4C][words][0x91 len name pad][elem_count u16=1]
 *     reply data:     [type u16 = 0x02A0][handle u16][payload...]
 *
 *   Write Tag (0x4D): [0x4D][words][0x91 len name pad]
 *                     [0xA0 0x02][handle u16][elem_count u16=1][payload...]
 *
 * The symbolic path is an ANSI Extended Symbol Segment (0x91): name
 * length, name bytes, NUL pad to even length; size in 16-bit words.
 *
 * Routed through bp_client_message_send (UCMM), so the assembled
 * request must fit BP_MSG_MAX_REQ (500); over-cap writes return
 * BP_ERR_PARAM_RANGE rather than truncate.  CIP reply parse + error
 * reporting follow the conn.c idiom (svc=resp[0], status=resp[2],
 * ext_size=resp[3], ext at resp[4..]).
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

/* Build an ANSI Extended Symbol Segment (0x91) request path for `tag`
 * into `out` (must hold at least 2 + strlen(tag) + 1 bytes).  Returns
 * the byte count written; *out_words gets the path size in 16-bit
 * words (the value the CIP request header carries).  The name is
 * padded to an even length with a trailing NUL per CIP path encoding. */
static size_t symbolic_ioi_path(const char *tag, uint8_t *out, uint8_t *out_words) {
    size_t n = strlen(tag);
    size_t off = 0;
    out[off++] = 0x91;
    out[off++] = (uint8_t)n;
    memcpy(out + off, tag, n);
    off += n;
    if (off % 2 != 0) {
        out[off++] = 0x00;
    }
    *out_words = (uint8_t)(off / 2);
    return off;
}

int bp_client_read_struct(bp_client_t *c, uint8_t slot, const char *tag,
                          void *out_data, uint16_t out_cap,
                          uint16_t *out_len, uint16_t *out_handle) {
    if (!c || !tag || !out_data)            return BP_ERR_NULL_ARG;
    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len > 250)      return BP_ERR_PARAM_RANGE;
    if (out_cap < 16)                       return BP_ERR_PARAM_RANGE;
    if (out_len)    *out_len = 0;
    if (out_handle) *out_handle = 0;

    /* Assemble: [0x4C][words][0x91 len name pad][elem_count u16=1] */
    uint8_t req[2 + 2 + 250 + 1 + 2];
    uint8_t ioi[2 + 250 + 1];
    uint8_t words = 0;
    size_t ioi_len = symbolic_ioi_path(tag, ioi, &words);

    size_t off = 0;
    req[off++] = 0x4C;
    req[off++] = words;
    memcpy(req + off, ioi, ioi_len);
    off += ioi_len;
    bp_st_u16(req + off, 1);  /* elem_count = 1 */
    off += 2;

    /* Response buffer bounded by out_cap (the caller's struct headroom). */
    uint8_t resp[512];
    uint16_t resp_cap = out_cap;
    if (resp_cap > sizeof(resp)) resp_cap = sizeof(resp);

    bp_message_t m = {
        .slot          = slot,
        .cip_request   = req,
        .req_size      = (uint16_t)off,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = resp_cap,
    };
    int rc = bp_client_message_send(c, &m);
    if (rc != BP_OK) return rc;

    if (m.resp_len < 4) return BP_ERR_GENERIC;
    uint8_t  svc      = resp[0];
    uint8_t  status   = resp[2];
    int      ext_words = resp[3];
    if (status != 0) {
        uint16_t ext = 0;
        if (ext_words >= 1 && m.resp_len >= (uint16_t)(4 + ext_words * 2)) {
            ext = bp_ld_u16(resp + 4);
        }
        bp_record_cip_error(c, svc, status, ext, slot);
        fprintf(stderr, "[bp_client_read_struct] CIP failure: "
                        "svc=0x%02x status=0x%02x ext=0x%04x slot=%u (%s)\n",
                svc, status, ext, slot, bp_cip_status_string(status, ext));
        return BP_ERR_CIP_STATUS;
    }

    /* body = [type u16][handle u16][payload...] starting after the
     * CIP header + any extended-status words. */
    uint16_t body_off = (uint16_t)(4 + ext_words * 2);
    if (m.resp_len < body_off + 4) return BP_ERR_GENERIC;
    const uint8_t *body = resp + body_off;
    uint16_t body_len = (uint16_t)(m.resp_len - body_off);

    uint16_t handle = bp_ld_u16(body + 2);
    uint16_t payload_len = (uint16_t)(body_len - 4);
    if (payload_len > out_cap) return BP_ERR_PARAM_RANGE;
    if (payload_len > 0) {
        memcpy(out_data, body + 4, payload_len);
    }
    if (out_len)    *out_len = payload_len;
    if (out_handle) *out_handle = handle;
    return BP_OK;
}

int bp_client_write_struct(bp_client_t *c, uint8_t slot, const char *tag,
                           uint16_t handle, const void *data, uint16_t data_len) {
    if (!c || !tag || !data)                return BP_ERR_NULL_ARG;
    size_t tag_len = strlen(tag);
    if (tag_len == 0 || tag_len > 250)      return BP_ERR_PARAM_RANGE;
    if (data_len == 0)                      return BP_ERR_PARAM_RANGE;

    /* Assemble: [0x4D][words][0x91 len name pad]
     *           [0xA0 0x02][handle u16][elem_count u16=1][payload...] */
    uint8_t ioi[2 + 250 + 1];
    uint8_t words = 0;
    size_t ioi_len = symbolic_ioi_path(tag, ioi, &words);

    /* 2 (svc+words) + ioi + 2 (0xA0 0x02) + 2 (handle) + 2 (elem) + data */
    size_t req_cap = 2 + ioi_len + 6 + (size_t)data_len;
    uint8_t req[2 + 2 + 250 + 1 + 6 + BP_MSG_MAX_REQ];
    size_t off = 0;
    req[off++] = 0x4D;
    req[off++] = words;
    memcpy(req + off, ioi, ioi_len);
    off += ioi_len;
    /* abbreviated-structure type + handle */
    req[off++] = 0xA0;
    req[off++] = 0x02;
    bp_st_u16(req + off, handle);
    off += 2;
    bp_st_u16(req + off, 1);  /* elem_count = 1 */
    off += 2;
    memcpy(req + off, data, data_len);
    off += data_len;
    (void)req_cap;

    if (off > BP_MSG_MAX_REQ) {
        /* UCMM request cap — larger structs need a connected/fragmented
         * path; surface a clear range error rather than a truncated write. */
        return BP_ERR_PARAM_RANGE;
    }

    uint8_t resp[64];
    bp_message_t m = {
        .slot          = slot,
        .cip_request   = req,
        .req_size      = (uint16_t)off,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = sizeof(resp),
    };
    int rc = bp_client_message_send(c, &m);
    if (rc != BP_OK) return rc;

    if (m.resp_len < 3) return BP_ERR_GENERIC;
    uint8_t  svc       = resp[0];
    uint8_t  status    = resp[2];
    int      ext_words = m.resp_len >= 4 ? resp[3] : 0;
    if (status != 0) {
        uint16_t ext = 0;
        if (ext_words >= 1 && m.resp_len >= (uint16_t)(4 + ext_words * 2)) {
            ext = bp_ld_u16(resp + 4);
        }
        bp_record_cip_error(c, svc, status, ext, slot);
        fprintf(stderr, "[bp_client_write_struct] CIP failure: "
                        "svc=0x%02x status=0x%02x ext=0x%04x slot=%u (%s)\n",
                svc, status, ext, slot, bp_cip_status_string(status, ext));
        return BP_ERR_CIP_STATUS;
    }
    return BP_OK;
}
