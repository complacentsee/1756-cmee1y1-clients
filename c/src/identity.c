/*
 * identity.c — CIP Identity-object wrappers.
 *
 * Wire formats RE'd from libocxbpapi-w.so:
 *   - OCXcip_GetIdObject        @ 0x1089e0  (local cm1756)
 *   - OCXcip_GetDeviceIdObject  @ 0x108450  (remote, by text path)
 *   - OCXcip_GetActiveNodeTable @ 0x1082b0  (backplane node bitmap)
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

/* Decode the on-wire 48-byte Identity into the public struct.  Both
 * GetIdObject and GetDeviceIdObject return the same struct layout —
 * just at different slot offsets. */
static void decode_id(const uint8_t *p, bp_id_object_t *out) {
    out->vendor_id     = bp_ld_u16(p + 0x00);
    out->device_type   = bp_ld_u16(p + 0x02);
    out->product_code  = bp_ld_u16(p + 0x04);
    out->major_rev     = p[0x06];
    out->minor_rev     = p[0x07];
    out->status        = bp_ld_u16(p + 0x08);
    out->serial_number = bp_ld_u32(p + 0x0a);
    memcpy(out->product_name, p + 0x0e, 32);
}

/* ============================================================
 * GetIdObject — local cm1756 Identity (no path)
 * ============================================================ */
typedef struct { bp_id_object_t *out; } gid_local_ctx_t;

static void gid_local_read(uint8_t *slot, void *user) {
    gid_local_ctx_t *c = user;
    decode_id(slot + 0x78, c->out);
}

int bp_client_get_id_local(bp_client_t *cl, bp_id_object_t *out) {
    if (!cl || !out) return BP_ERR_NULL_ARG;
    memset(out, 0, sizeof(*out));
    gid_local_ctx_t ctx = { .out = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetIdObject",
        .payload_size = 0xa8,
        .fill_payload = NULL,
        .read_reply   = gid_local_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

/* ============================================================
 * GetDeviceIdObject — Identity of a device named by text path
 *
 * Request:
 *   slot + 0x78   text_path (NUL-terminated, max 255 bytes)
 *   slot + 0x178  uint16 instance
 * Response:
 *   slot + 0x178  48 bytes (ocx_id_object_t)
 * ============================================================ */
typedef struct {
    const char *path;
    uint16_t    instance;
    bp_id_object_t *out;
} gid_dev_ctx_t;

static void gid_dev_fill(uint8_t *slot, void *user) {
    gid_dev_ctx_t *c = user;
    size_t n = strlen(c->path);
    if (n > 254) n = 254;
    memcpy(slot + 0x78, c->path, n);
    *(slot + 0x78 + n) = 0;
    bp_st_u16(slot + 0x178, c->instance);
}

static void gid_dev_read(uint8_t *slot, void *user) {
    gid_dev_ctx_t *c = user;
    decode_id(slot + 0x178, c->out);
}

int bp_client_get_device_id(bp_client_t *cl, const char *path,
                             uint16_t instance, bp_id_object_t *out) {
    if (!cl || !path || !out) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254)   return BP_ERR_PARAM_RANGE;
    memset(out, 0, sizeof(*out));
    gid_dev_ctx_t ctx = { .path = path, .instance = instance, .out = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetDeviceIdObject",
        .payload_size = 0x1b0,
        .fill_payload = gid_dev_fill,
        .read_reply   = gid_dev_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

/* ============================================================
 * GetActiveNodeTable
 * ============================================================ */
typedef struct { uint32_t lo, hi; } an_ctx_t;

static void an_read(uint8_t *slot, void *user) {
    an_ctx_t *c = user;
    c->lo = bp_ld_u32(slot + 0x78);
    c->hi = bp_ld_u32(slot + 0x7c);
}

int bp_client_get_active_nodes(bp_client_t *cl, uint32_t *lo, uint32_t *hi) {
    if (!cl || !lo || !hi) return BP_ERR_NULL_ARG;
    an_ctx_t ctx = {0};
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetActiveNodeTable",
        .payload_size = 0x80,
        .fill_payload = NULL,
        .read_reply   = an_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    int rc = bp_client_call(cl, &spec);
    if (rc == BP_OK) { *lo = ctx.lo; *hi = ctx.hi; }
    return rc;
}
