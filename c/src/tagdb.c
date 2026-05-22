/*
 * tagdb.c — OCXcip_CreateTagDbHandle, BuildTagDb, GetSymbolInfo,
 *           DeleteTagDbHandle.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

/* ============================================================
 * CreateTagDbHandle
 * ============================================================ */
typedef struct {
    const char *path;
    uint32_t    handle;
} ctdh_ctx_t;

static void ctdh_fill(uint8_t *slot, void *user) {
    ctdh_ctx_t *ctx = user;
    /* Path string at +0x78, NUL-terminated, max 255 bytes */
    size_t path_len = strlen(ctx->path);
    if (path_len > 254) path_len = 254;
    memset(slot + BP_HDR_PAYLOAD_START, 0, 256);
    memcpy(slot + BP_HDR_PAYLOAD_START, ctx->path, path_len);
    slot[BP_HDR_PAYLOAD_START + path_len] = 0;
    /* flags at +0x178 */
    bp_st_u16(slot + 0x178, 0);
}

static void ctdh_read(uint8_t *slot, void *user) {
    ctdh_ctx_t *ctx = user;
    ctx->handle = bp_ld_u32(slot + 0x17C);
}

int bp_tagdb_open(bp_client_t *client, const char *path, bp_tagdb_t **out_db) {
    if (!client || !path || !out_db) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254)         return BP_ERR_PARAM_RANGE;

    ctdh_ctx_t ctx = { .path = path, .handle = 0 };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_CreateTagDbHandle",
        .payload_size = 0x180,
        .fill_payload = ctdh_fill,
        .read_reply   = ctdh_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    int rc = bp_client_call(client, &spec);
    if (rc != BP_OK) return rc;

    bp_tagdb_t *db = calloc(1, sizeof(*db));
    if (!db) return BP_ERR_CLIENT_OPEN;
    db->client = client;
    db->handle = ctx.handle;
    size_t plen = strlen(path);
    if (plen > 254) plen = 254;
    memcpy(db->path, path, plen);
    db->path[plen] = 0;
    *out_db = db;
    return BP_OK;
}

/* ============================================================
 * DeleteTagDbHandle
 * ============================================================ */
static void simple_handle_fill(uint8_t *slot, void *user) {
    uint32_t *h = user;
    bp_st_u32(slot + BP_HDR_PAYLOAD_START, *h);
}

void bp_tagdb_close(bp_tagdb_t *db) {
    if (!db) return;
    if (db->handle != 0) {
        uint32_t h = db->handle;
        bp_call_spec_t spec = {
            .fn_name      = "OCXcip_DeleteTagDbHandle",
            .payload_size = 0x80,
            .fill_payload = simple_handle_fill,
            .read_reply   = NULL,
            .timeout_ms   = 5000,
            .user         = &h,
        };
        (void)bp_client_call(db->client, &spec);
    }
    free(db);
}

/* ============================================================
 * BuildTagDb
 * ============================================================ */
typedef struct {
    uint32_t handle;
    uint16_t status;
} build_ctx_t;

static void build_fill(uint8_t *slot, void *user) {
    build_ctx_t *ctx = user;
    bp_st_u32(slot + BP_HDR_PAYLOAD_START, ctx->handle);
}

static void build_read(uint8_t *slot, void *user) {
    build_ctx_t *ctx = user;
    ctx->status = bp_ld_u16(slot + BP_HDR_PAYLOAD_START + 4);
}

int bp_tagdb_build(bp_tagdb_t *db, uint16_t *out_symbol_count) {
    if (!db) return BP_ERR_NULL_ARG;
    build_ctx_t ctx = { .handle = db->handle, .status = 0 };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_BuildTagDb",
        .payload_size = 0x80,
        .fill_payload = build_fill,
        .read_reply   = build_read,
        .timeout_ms   = 30000,
        .user         = &ctx,
    };
    int rc = bp_client_call(db->client, &spec);
    if (rc == BP_OK && out_symbol_count) *out_symbol_count = ctx.status;
    return rc;
}

/* ============================================================
 * GetSymbolInfo
 * ============================================================ */
typedef struct {
    uint32_t handle;
    uint16_t index;
    uint8_t  raw[128];
} sym_ctx_t;

static void sym_fill(uint8_t *slot, void *user) {
    sym_ctx_t *ctx = user;
    bp_st_u32(slot + BP_HDR_PAYLOAD_START,     ctx->handle);
    bp_st_u16(slot + BP_HDR_PAYLOAD_START + 4, ctx->index);
}

static void sym_read(uint8_t *slot, void *user) {
    sym_ctx_t *ctx = user;
    /* 128 bytes at slot + 0x80 (i.e. payload_start + 8) */
    memcpy(ctx->raw, slot + BP_HDR_PAYLOAD_START + 8, 128);
}

int bp_tagdb_symbol_at(bp_tagdb_t *db, uint16_t index,
                       bp_symbol_info_t *out_info) {
    if (!db || !out_info) return BP_ERR_NULL_ARG;

    sym_ctx_t ctx = { .handle = db->handle, .index = index };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetSymbolInfo",
        .payload_size = 0x100,
        .fill_payload = sym_fill,
        .read_reply   = sym_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    int rc = bp_client_call(db->client, &spec);
    if (rc != BP_OK) return rc;

    /* Parse the 128-byte symbol info struct */
    memset(out_info, 0, sizeof(*out_info));
    /* Name: NUL-terminated, max 99 bytes (leave room for NUL) */
    size_t name_len = 0;
    while (name_len < 99 && ctx.raw[name_len] != 0) name_len++;
    memcpy(out_info->name, ctx.raw, name_len);
    out_info->name[name_len] = 0;

    out_info->data_type   = bp_ld_u16(ctx.raw + BP_SYM_DATATYPE_OFF);
    out_info->struct_type = bp_ld_u16(ctx.raw + BP_SYM_STRUCTTYPE_OFF);
    out_info->field1      = bp_ld_u32(ctx.raw + BP_SYM_FIELD1_OFF);
    out_info->field2      = bp_ld_u32(ctx.raw + BP_SYM_FIELD2_OFF);
    out_info->field3      = bp_ld_u32(ctx.raw + BP_SYM_FIELD3_OFF);
    out_info->instance_id = bp_ld_u32(ctx.raw + BP_SYM_INSTID_OFF);
    out_info->flags       = bp_ld_u16(ctx.raw + BP_SYM_FLAGS_OFF);
    return BP_OK;
}
