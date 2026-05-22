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
    if (rc == BP_OK) {
        /* Invalidate any prior cache for this path and resize for the
         * fresh table size.  Lazy fill kicks in on the next
         * bp_tagdb_lookup_symbol; or call bp_tagdb_preload_symbols for
         * eager warm-up.  Best-effort: a cache-reset failure here
         * doesn't fail the build (caller can still iterate
         * symbol_at). */
        (void)bp_tag_cache_reset_after_build(db->client, db->path, ctx.status);
        if (out_symbol_count) *out_symbol_count = ctx.status;
    }
    return rc;
}

/* ============================================================
 * TestTagDbVer
 *
 * Engine semantics (RE'd from libocxbpapi.so.2.3 OCXcip_TestTagDbVer
 * at 0x10ab3c): server memcmp's 12 bytes of a freshly-computed
 * PLC tag-DB version against the value captured during the last
 * BuildTagDb on this handle.  Engine writes one of these to
 * slot + 0x50 (errorcode):
 *   0x00 — versions match (caller's cache is current)
 *   0x14 — versions differ (caller should rebuild)
 *   0x15 — tagdb handle has no captured version yet (Build never run
 *          on this handle, or the version slot is uninitialised)
 *
 * Our bp_client_call surfaces these positive values directly because
 * any non-zero slot errorcode is returned as the rc.  We translate
 * 0x14 and 0x15 to *out_changed = 1 (the caller's recovery is the
 * same in both cases: call bp_tagdb_build).
 * ============================================================ */
int bp_tagdb_test_version(bp_tagdb_t *db, int *out_changed) {
    if (!db || !out_changed) return BP_ERR_NULL_ARG;
    uint32_t h = db->handle;
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_TestTagDbVer",
        .payload_size = 0x80,
        .fill_payload = simple_handle_fill,
        .read_reply   = NULL,
        .timeout_ms   = 5000,
        .user         = &h,
    };
    int rc = bp_client_call(db->client, &spec);
    if (rc == 0)    { *out_changed = 0; return BP_OK; }
    if (rc == 0x14) { *out_changed = 1; return BP_OK; }
    if (rc == 0x15) { *out_changed = 1; return BP_OK; }
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

int bp_symbol_is_array(const bp_symbol_info_t *info) {
    /* The Logix symbol enumeration encodes array dim0 in
     * bp_symbol_info_t.dim0.  Non-zero means "this is an array".
     * (The data_type field does NOT have the 0x2000 array bit set on
     * tested cm1756-via-L85E enumerations; dim0 is the only reliable
     * signal.) */
    return info ? (int)(info->dim0 != 0) : 0;
}
int bp_symbol_is_struct(const bp_symbol_info_t *info) {
    return info ? (int)(info->struct_type != 0) : 0;
}
uint16_t bp_symbol_type_code(const bp_symbol_info_t *info) {
    return info ? (uint16_t)(info->data_type & 0x1FFFu) : 0;
}

/* ============================================================
 * OCXcip_GetStructInfo + OCXcip_GetStructMbrInfo
 *
 * Wire-protocol layouts derived empirically against L85E firmware
 * ~36.11 via the cm1756.  See docs/protocol.md for the spec; see
 * the examples/structprobe.c output captured during development
 * for the raw-byte verification.
 * ============================================================ */

typedef struct {
    uint32_t handle;
    uint16_t struct_id;
    bp_struct_info_t *out;
} si_ctx_t;

static void si_fill(uint8_t *slot, void *user) {
    si_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->handle);
    bp_st_u16(slot + 0x7C, c->struct_id);
}

static void si_read(uint8_t *slot, void *user) {
    si_ctx_t *c = user;
    /* StructInfo response lives at slot+0x80, 56 bytes total.
     * Empirically determined layout (see examples/structprobe.c
     * runtime captures during RE):
     *   +0x00 char[40] name (NUL-terminated; trailing bytes may contain
     *                        leftover slot data — read only up to NUL)
     *   +0x28 (4 bytes) (zero in observed runs; likely padding)
     *   +0x2C uint32    data_type   (wire-level, e.g. 0x4527)
     *   +0x30 uint32    byte_size   (struct's total size)
     *   +0x34 uint16    (template handle / CRC; we don't surface)
     *   +0x36 uint16    n_members
     */
    const uint8_t *p = slot + 0x80;
    memset(c->out, 0, sizeof(*c->out));
    size_t nl = 0;
    while (nl < 39 && p[nl] != 0) nl++;
    memcpy(c->out->name, p, nl);
    c->out->name[nl] = 0;
    c->out->data_type = bp_ld_u32(p + 0x2C);
    c->out->byte_size = bp_ld_u32(p + 0x30);
    c->out->n_members = bp_ld_u16(p + 0x36);
}

int bp_tagdb_get_struct_info(bp_tagdb_t *db, uint16_t struct_id,
                              bp_struct_info_t *out_info) {
    if (!db || !out_info) return BP_ERR_NULL_ARG;
    si_ctx_t ctx = { .handle = db->handle, .struct_id = struct_id, .out = out_info };
    bp_call_spec_t spec = {
        .fn_name = "OCXcip_GetStructInfo",
        .payload_size = 0xB8,
        .fill_payload = si_fill,
        .read_reply   = si_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(db->client, &spec);
}

typedef struct {
    uint32_t handle;
    uint16_t struct_id;
    uint16_t member_index;
    bp_struct_member_info_t *out;
} smi_ctx_t;

static void smi_fill(uint8_t *slot, void *user) {
    smi_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->handle);
    bp_st_u16(slot + 0x7C, c->struct_id);
    bp_st_u16(slot + 0x7E, c->member_index);
}

static void smi_read(uint8_t *slot, void *user) {
    smi_ctx_t *c = user;
    /* StructMemberInfo lives at slot+0x80, 76 bytes:
     *  +0x00 char[44] name (NUL-terminated)
     *  +0x2C uint16   data_type
     *  +0x30 uint16   struct_id (non-zero if member is a UDT)
     *  +0x34 uint32   byte_size
     *  +0x38 uint32   offset within parent
     *  +0x3C uint32   dim0
     *  +0x40 uint32   dim1
     *  +0x44 uint32   flags
     */
    const uint8_t *p = slot + 0x80;
    memset(c->out, 0, sizeof(*c->out));
    size_t nl = 0;
    while (nl < 43 && p[nl] != 0) nl++;
    memcpy(c->out->name, p, nl);
    c->out->name[nl] = 0;
    c->out->data_type   = bp_ld_u16(p + 0x2C);
    c->out->struct_id   = bp_ld_u16(p + 0x30);
    c->out->byte_size   = bp_ld_u32(p + 0x34);
    c->out->offset      = bp_ld_u32(p + 0x38);
    /* p+0x3C is zero in all observed cases — reserved or 2nd dim */
    c->out->array_count = bp_ld_u32(p + 0x40);  /* N for SINT[N] / DINT[N] etc.; 0 if scalar */
    c->out->flags       = p[0x44];              /* only low byte is meaningful */
}

int bp_tagdb_get_struct_member(bp_tagdb_t *db, uint16_t struct_id,
                                uint16_t member_index,
                                bp_struct_member_info_t *out_member) {
    if (!db || !out_member) return BP_ERR_NULL_ARG;
    smi_ctx_t ctx = {
        .handle = db->handle, .struct_id = struct_id,
        .member_index = member_index, .out = out_member,
    };
    bp_call_spec_t spec = {
        .fn_name = "OCXcip_GetStructMbrInfo",
        .payload_size = 0xD0,
        .fill_payload = smi_fill,
        .read_reply   = smi_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(db->client, &spec);
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

    out_info->data_type      = bp_ld_u16(ctx.raw + BP_SYM_DATATYPE_OFF);
    out_info->struct_type    = bp_ld_u16(ctx.raw + BP_SYM_STRUCTTYPE_OFF);
    out_info->elem_byte_size = bp_ld_u32(ctx.raw + BP_SYM_ELEMSIZE_OFF);
    out_info->dim0           = bp_ld_u32(ctx.raw + BP_SYM_DIM0_OFF);
    out_info->dim1           = bp_ld_u32(ctx.raw + BP_SYM_DIM1_OFF);
    out_info->dim2           = bp_ld_u32(ctx.raw + BP_SYM_DIM2_OFF);
    out_info->flags          = bp_ld_u16(ctx.raw + BP_SYM_FLAGS_OFF);
    return BP_OK;
}
