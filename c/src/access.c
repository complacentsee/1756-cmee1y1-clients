/*
 * access.c — OCXcip_AccessTagData (batched R/W) + scalar helpers.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

typedef struct {
    bp_tagdb_t       *db;
    bp_tag_request_t *requests;
    size_t            count;
    uint32_t          data_area_start;  /* slot offset where data area begins */
} access_ctx_t;

static void access_fill(uint8_t *slot, void *user) {
    access_ctx_t *ctx = user;

    /* Path at +0x78, NUL-terminated up to 255 bytes */
    memset(slot + BP_HDR_PAYLOAD_START, 0, 256);
    size_t plen = strlen(ctx->db->path);
    if (plen > 254) plen = 254;
    memcpy(slot + BP_HDR_PAYLOAD_START, ctx->db->path, plen);

    /* service code (unused) at +0x178; count at +0x17A */
    bp_st_u16(slot + BP_TAGDATA_SERVICE_OFF, 0);
    bp_st_u16(slot + BP_TAGDATA_COUNT_OFF,   (uint16_t)ctx->count);

    /* Per-request descriptors + data */
    uint32_t data_off = ctx->data_area_start;
    for (size_t i = 0; i < ctx->count; i++) {
        bp_tag_request_t *r = &ctx->requests[i];
        uint32_t req_start =
            BP_TAGDATA_REQ0_START + (uint32_t)i * BP_TAGDATA_REQ_STRIDE;

        /* Clear the descriptor + write fields */
        memset(slot + req_start, 0, BP_TAGDATA_REQ_STRIDE);

        size_t tlen = strlen(r->tag_name);
        if (tlen > 254) tlen = 254;
        memcpy(slot + req_start + BP_REQ_TAGNAME_OFF, r->tag_name, tlen);

        bp_st_u16(slot + req_start + BP_REQ_DATATYPE_OFF,       r->data_type);
        bp_st_u16(slot + req_start + BP_REQ_ELEM_BYTE_SIZE_OFF, r->elem_byte_size);
        bp_st_u16(slot + req_start + BP_REQ_ACTION_OFF,         r->action);
        bp_st_u16(slot + req_start + BP_REQ_ELEM_COUNT_OFF,     r->elem_count);
        slot[req_start + BP_REQ_HAS_EXTRA_OFF] = 0;
        bp_st_u64(slot + req_start + BP_REQ_DATA_PTR_OFF, 0);

        /* For writes, copy caller's data into the slot's data area */
        uint32_t nbytes = (uint32_t)r->elem_byte_size * (uint32_t)r->elem_count;
        if (r->action == BP_ACTION_WRITE && r->data && nbytes > 0) {
            memcpy(slot + data_off, r->data, nbytes);
        }
        data_off += nbytes;
    }
}

static void access_read(uint8_t *slot, void *user) {
    access_ctx_t *ctx = user;
    uint32_t data_off = ctx->data_area_start;
    for (size_t i = 0; i < ctx->count; i++) {
        bp_tag_request_t *r = &ctx->requests[i];
        uint32_t req_start =
            BP_TAGDATA_REQ0_START + (uint32_t)i * BP_TAGDATA_REQ_STRIDE;
        r->result = bp_ld_u32(slot + req_start + BP_REQ_RESULT_OFF);
        uint32_t nbytes = (uint32_t)r->elem_byte_size * (uint32_t)r->elem_count;
        if (r->action == BP_ACTION_READ && r->data && nbytes > 0) {
            memcpy(r->data, slot + data_off, nbytes);
        }
        data_off += nbytes;
    }
}

int bp_tagdb_access(bp_tagdb_t *db, bp_tag_request_t *requests, size_t count) {
    if (!db || !requests || count == 0) return BP_ERR_NULL_ARG;
    if (count > 16)                      return BP_ERR_PARAM_RANGE;

    /* Compute payload_size = data_area_start + total_data_bytes */
    uint32_t data_area_start = 0x2A0 + ((uint32_t)count - 1) * BP_TAGDATA_REQ_STRIDE;
    uint32_t total_data_bytes = 0;
    for (size_t i = 0; i < count; i++) {
        total_data_bytes += (uint32_t)requests[i].elem_byte_size
                          * (uint32_t)requests[i].elem_count;
    }
    uint32_t payload_size = data_area_start + total_data_bytes;
    if (payload_size > BP_SLOT_STRIDE - 0x80) return BP_ERR_SLOT_TOO_LARGE;

    access_ctx_t ctx = {
        .db = db, .requests = requests, .count = count,
        .data_area_start = data_area_start,
    };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_AccessTagData",
        .payload_size = payload_size,
        .fill_payload = access_fill,
        .read_reply   = access_read,
        .timeout_ms   = 10000,
        .user         = &ctx,
    };
    return bp_client_call(db->client, &spec);
}

/* ============================================================
 * Scalar convenience helpers
 * ============================================================ */

#define DEFINE_RW(NAME, TYPE, CIP_TYPE, BYTES)                                \
int bp_tagdb_read_##NAME(bp_tagdb_t *db, const char *tag, TYPE *out_value) {   \
    if (!out_value) return BP_ERR_NULL_ARG;                                    \
    TYPE buf = 0;                                                              \
    bp_tag_request_t r = {                                                     \
        .tag_name = tag, .data_type = CIP_TYPE, .elem_byte_size = BYTES,       \
        .action = BP_ACTION_READ, .elem_count = 1, .data = &buf, .result = 0,  \
    };                                                                         \
    int rc = bp_tagdb_access(db, &r, 1);                                       \
    if (rc != BP_OK) return rc;                                                \
    if (r.result != 0) return BP_ERR_GENERIC;                                  \
    *out_value = buf;                                                          \
    return BP_OK;                                                              \
}                                                                              \
int bp_tagdb_write_##NAME(bp_tagdb_t *db, const char *tag, TYPE value) {       \
    bp_tag_request_t r = {                                                     \
        .tag_name = tag, .data_type = CIP_TYPE, .elem_byte_size = BYTES,       \
        .action = BP_ACTION_WRITE, .elem_count = 1, .data = &value, .result = 0,\
    };                                                                         \
    int rc = bp_tagdb_access(db, &r, 1);                                       \
    if (rc != BP_OK) return rc;                                                \
    if (r.result != 0) return BP_ERR_GENERIC;                                  \
    return BP_OK;                                                              \
}

DEFINE_RW(int,  int16_t, BP_TYPE_INT,  2)
DEFINE_RW(dint, int32_t, BP_TYPE_DINT, 4)
DEFINE_RW(lint, int64_t, BP_TYPE_LINT, 8)
DEFINE_RW(real, float,   BP_TYPE_REAL, 4)
DEFINE_RW(lreal,double,  BP_TYPE_LREAL,8)

/* BOOL handled specially — the caller-facing type is `int` but on
 * the wire it's a single byte. */
int bp_tagdb_read_bool(bp_tagdb_t *db, const char *tag, int *out_value) {
    if (!out_value) return BP_ERR_NULL_ARG;
    uint8_t b = 0;
    bp_tag_request_t r = {
        .tag_name = tag, .data_type = BP_TYPE_BOOL, .elem_byte_size = 1,
        .action = BP_ACTION_READ, .elem_count = 1, .data = &b, .result = 0,
    };
    int rc = bp_tagdb_access(db, &r, 1);
    if (rc != BP_OK) return rc;
    if (r.result != 0) return BP_ERR_GENERIC;
    *out_value = (b != 0);
    return BP_OK;
}
int bp_tagdb_write_bool(bp_tagdb_t *db, const char *tag, int value) {
    uint8_t b = value ? 1 : 0;
    bp_tag_request_t r = {
        .tag_name = tag, .data_type = BP_TYPE_BOOL, .elem_byte_size = 1,
        .action = BP_ACTION_WRITE, .elem_count = 1, .data = &b, .result = 0,
    };
    int rc = bp_tagdb_access(db, &r, 1);
    if (rc != BP_OK) return rc;
    if (r.result != 0) return BP_ERR_GENERIC;
    return BP_OK;
}
