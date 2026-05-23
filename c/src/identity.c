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
 * GetDeviceIdStatus — v0.10.0+ lightweight status-word probe
 *
 * Request mirrors GetDeviceIdObject: text_path at +0x78, instance
 * at +0x178.  Response is just a uint16 status at +0x178 (overlapping
 * the consumed input).  Wire address: libocxbpapi-w.so:0x108740
 * (RE'd from sibling tools/phase2_plc_mode.py ctypes signature).
 *
 * Payload size set to 0x180 — covers path+instance input region;
 * engine writes 2-byte response within bounds.
 * ============================================================ */
typedef struct {
    const char *path;
    uint16_t    instance;
    uint16_t    out_status;
} gids_ctx_t;

static void gids_fill(uint8_t *slot, void *user) {
    gids_ctx_t *c = user;
    size_t n = strlen(c->path);
    if (n > 254) n = 254;
    memcpy(slot + 0x78, c->path, n);
    *(slot + 0x78 + n) = 0;
    /* OEM wrapper at libocxbpapi-w.so:0x108620 explicitly clears
     * +0x178 (the output slot) before dispatch and then writes the
     * instance at +0x17A.  Without the clear, the engine seems to
     * leave +0x178 containing residual data — our earlier attempts
     * read junk because the slot wasn't reset. */
    bp_st_u16(slot + 0x178, 0);
    bp_st_u16(slot + 0x17A, c->instance);
}

static void gids_read(uint8_t *slot, void *user) {
    gids_ctx_t *c = user;
    /* Response status word at +0x178; instance was at +0x17A so
     * +0x178 is dedicated output (decompile of OCXcip_GetDeviceIdStatus
     * @ 0x108620 confirms: `*param_3 = puStack_8[0xbc]` where
     * puStack_8 is undefined2* so index 0xbc = byte offset 0x178). */
    c->out_status = bp_ld_u16(slot + 0x178);
}

int bp_client_get_device_id_status(bp_client_t *cl, const char *path,
                                    uint16_t instance, uint16_t *out_status) {
    if (!cl || !path || !out_status) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254)           return BP_ERR_PARAM_RANGE;
    gids_ctx_t ctx = { .path = path, .instance = instance };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetDeviceIdStatus",
        .payload_size = 0x180,
        .fill_payload = gids_fill,
        .read_reply   = gids_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    int rc = bp_client_call(cl, &spec);
    if (rc == BP_OK) *out_status = ctx.out_status;
    return rc;
}

/* ============================================================
 * GetExDevObject — extended device info (v0.10.0+)
 *
 * Wire format (Ghidra: 0x001087e4):
 *   payload_size = 0x260
 *   request: path at +0x78, instance at +0x25A (uint16)
 *   response: 28 qwords + uint16 trailer at +0x178..+0x25A (226 bytes)
 * ============================================================ */
typedef struct {
    const char *path;
    uint16_t    instance;
    uint8_t    *out;       /* 226 bytes */
} exd_ctx_t;

static void exd_fill(uint8_t *slot, void *user) {
    exd_ctx_t *c = user;
    size_t n = strlen(c->path);
    if (n > 254) n = 254;
    memcpy(slot + 0x78, c->path, n);
    *(slot + 0x78 + n) = 0;
    bp_st_u16(slot + 0x25A, c->instance);
}

static void exd_read(uint8_t *slot, void *user) {
    exd_ctx_t *c = user;
    memcpy(c->out, slot + 0x178, BP_EX_DEV_OBJECT_BYTES);
}

int bp_client_get_ex_dev_object(bp_client_t *cl, const char *path,
                                 uint16_t instance,
                                 uint8_t out[BP_EX_DEV_OBJECT_BYTES]) {
    if (!cl || !path || !out) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254)   return BP_ERR_PARAM_RANGE;
    memset(out, 0, BP_EX_DEV_OBJECT_BYTES);
    exd_ctx_t ctx = { .path = path, .instance = instance, .out = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetExDevObject",
        .payload_size = 0x260,
        .fill_payload = exd_fill,
        .read_reply   = exd_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

/* ============================================================
 * GetDeviceICPObject — EtherNet/IP IP-config (v0.10.0+)
 *
 * Wire format (Ghidra: 0x00108d00):
 *   payload_size = 0x190
 *   request: path at +0x78, instance at +0x18C (uint16)
 *   response: 2 qwords + 1 uint32 at +0x178..+0x18C (20 bytes)
 * ============================================================ */
typedef struct {
    const char *path;
    uint16_t    instance;
    uint8_t    *out;       /* 20 bytes */
} icp_ctx_t;

static void icp_fill(uint8_t *slot, void *user) {
    icp_ctx_t *c = user;
    size_t n = strlen(c->path);
    if (n > 254) n = 254;
    memcpy(slot + 0x78, c->path, n);
    *(slot + 0x78 + n) = 0;
    bp_st_u16(slot + 0x18C, c->instance);
}

static void icp_read(uint8_t *slot, void *user) {
    icp_ctx_t *c = user;
    memcpy(c->out, slot + 0x178, BP_DEVICE_ICP_OBJECT_BYTES);
}

int bp_client_get_device_icp_object(bp_client_t *cl, const char *path,
                                     uint16_t instance,
                                     uint8_t out[BP_DEVICE_ICP_OBJECT_BYTES]) {
    if (!cl || !path || !out) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254)   return BP_ERR_PARAM_RANGE;
    memset(out, 0, BP_DEVICE_ICP_OBJECT_BYTES);
    icp_ctx_t ctx = { .path = path, .instance = instance, .out = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetDeviceICPObject",
        .payload_size = 0x190,
        .fill_payload = icp_fill,
        .read_reply   = icp_read,
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
