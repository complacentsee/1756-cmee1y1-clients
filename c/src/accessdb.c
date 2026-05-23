/*
 * accessdb.c — OCXcip_AccessTagDataDb (batched R/W via cached db_handle).
 *
 * SPDX-License-Identifier: MIT
 *
 * Peer of access.c (OCXcip_AccessTagData) that trades the 251-byte
 * per-call path string for a 4-byte db_handle lookup on the engine
 * side.  The handle is the value CreateTagDbHandle returned (cached
 * on bp_tagdb_t at open time).  See docs/access-tag-data-db.md for
 * the RE notes and the descriptor-layout diff vs AccessTagData.
 *
 * SDK policy on the new fields:
 *   has_extra = 0, opt_value  = 0      (matches OEM wrapper's
 *                                       param_5 == NULL branch)
 *   has_data  = 0, mask_seed  = 0      (matches OEM wrapper's
 *                                       request->data == NULL branch)
 *
 * The engine reads tag data from the slot's data area when has_data == 0,
 * which is the same path AccessTagData already drives successfully.
 */
#include <stdint.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

typedef struct {
    bp_tagdb_t       *db;
    bp_tag_request_t *requests;
    size_t            count;
    uint32_t          data_area_start;  /* slot offset where data area begins */
} accessdb_ctx_t;

static void accessdb_fill(uint8_t *slot, void *user) {
    accessdb_ctx_t *ctx = user;

    /* db_handle (u32) at +0x78; has_extra/opt_value/count immediately
     * after.  Zero the run from +0x78 through the end of the header
     * area so any reserved bytes start at 0. */
    memset(slot + BP_HDR_PAYLOAD_START, 0,
           BP_TAGDATA_DB_REQ0_START - BP_HDR_PAYLOAD_START);

    bp_st_u32(slot + BP_TAGDATA_DB_HANDLE_OFF, ctx->db->handle);
    /* has_extra/opt_value already 0 from the memset above. */
    bp_st_u16(slot + BP_TAGDATA_DB_COUNT_OFF, (uint16_t)ctx->count);

    /* Per-request descriptors + inline data. */
    uint32_t data_off = ctx->data_area_start;
    for (size_t i = 0; i < ctx->count; i++) {
        bp_tag_request_t *r = &ctx->requests[i];
        uint32_t req_start =
            BP_TAGDATA_DB_REQ0_START + (uint32_t)i * BP_TAGDATA_DB_REQ_STRIDE;

        /* Zero the whole descriptor, then populate fields.  Names up
         * to 254 bytes are copied; the NUL at +0xFF stays from the
         * memset (the OEM wrapper writes an explicit 0 there — same
         * effect since we memset first). */
        memset(slot + req_start, 0, BP_TAGDATA_DB_REQ_STRIDE);

        size_t tlen = strlen(r->tag_name);
        if (tlen > 254) tlen = 254;
        memcpy(slot + req_start + BP_REQDB_TAGNAME_OFF, r->tag_name, tlen);

        bp_st_u16(slot + req_start + BP_REQDB_ACTION_OFF,         r->action);
        bp_st_u32(slot + req_start + BP_REQDB_DATATYPE_OFF,       r->data_type);
        bp_st_u32(slot + req_start + BP_REQDB_ELEM_BYTE_SIZE_OFF, r->elem_byte_size);
        bp_st_u32(slot + req_start + BP_REQDB_ELEM_COUNT_OFF,     r->elem_count);
        /* has_data and mask_seed both stay at 0 from the memset. */

        /* For writes, copy caller's bytes into the slot's data area. */
        uint32_t nbytes = (uint32_t)r->elem_byte_size * (uint32_t)r->elem_count;
        if (r->action == BP_ACTION_WRITE && r->data && nbytes > 0) {
            memcpy(slot + data_off, r->data, nbytes);
        }
        data_off += nbytes;
    }
}

static void accessdb_read(uint8_t *slot, void *user) {
    accessdb_ctx_t *ctx = user;
    uint32_t data_off = ctx->data_area_start;
    for (size_t i = 0; i < ctx->count; i++) {
        bp_tag_request_t *r = &ctx->requests[i];
        uint32_t req_start =
            BP_TAGDATA_DB_REQ0_START + (uint32_t)i * BP_TAGDATA_DB_REQ_STRIDE;
        r->result = bp_ld_u32(slot + req_start + BP_REQDB_RESULT_OFF);
        uint32_t nbytes = (uint32_t)r->elem_byte_size * (uint32_t)r->elem_count;
        if (r->action == BP_ACTION_READ && r->data && nbytes > 0) {
            memcpy(r->data, slot + data_off, nbytes);
        }
        data_off += nbytes;
    }
}

int bp_tagdb_access_db(bp_tagdb_t *db, bp_tag_request_t *requests, size_t count) {
    if (!db || !requests || count == 0) return BP_ERR_NULL_ARG;
    if (count > 16)                      return BP_ERR_PARAM_RANGE;

    /* data_area_start = 0x1B0 + (count - 1) * 0x128, matching the
     * OEM wrapper's payload_size = 0x1B0 + (count-1)*0x128 +
     * total_data_bytes formula. */
    uint32_t data_area_start =
        BP_TAGDATA_DB_DATA_AREA0
        + ((uint32_t)count - 1u) * BP_TAGDATA_DB_REQ_STRIDE;
    uint32_t total_data_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        total_data_bytes += (uint32_t)requests[i].elem_byte_size
                          * (uint32_t)requests[i].elem_count;
    }
    uint32_t payload_size = data_area_start + total_data_bytes;
    if (payload_size > BP_SLOT_STRIDE - 0x80) return BP_ERR_SLOT_TOO_LARGE;

    accessdb_ctx_t ctx = {
        .db = db, .requests = requests, .count = count,
        .data_area_start = data_area_start,
    };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_AccessTagDataDb",
        .payload_size = payload_size,
        .fill_payload = accessdb_fill,
        .read_reply   = accessdb_read,
        .timeout_ms   = 10000,
        .user         = &ctx,
    };
    return bp_client_call(db->client, &spec);
}
