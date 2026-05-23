/*
 * wctime.c — OCXcip_GetWCTime / SetWCTime + UTC variants (v0.10.0).
 *
 * Wire format RE'd via Ghidra (libocxbpapi-w.so:0x10e2e0,
 * 0x10e4c0, 0x10e6a0, 0x10e894).  All four opcodes share payload
 * size 0x1B0 and slot layout:
 *
 *   REQUEST:
 *     +0x78    path string (NUL-terminated, max 254 bytes)
 *     +0x178   uint16 instance
 *     +0x17A   uint8  have_buffer flag (1 if input/output present)
 *     +0x180   uint64 sec        (input for Set; output for Get)
 *     +0x188   uint64 nsec
 *     +0x190   uint64 aux0
 *     +0x198   uint64 aux1
 *     +0x1A0   uint64 aux2
 *     +0x1A8   uint64 aux3
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

#define WC_PAYLOAD_SIZE 0x1B0u

static void wc_st_u64(uint8_t *p, uint64_t v) {
    bp_st_u32(p,     (uint32_t)v);
    bp_st_u32(p + 4, (uint32_t)(v >> 32));
}
static uint64_t wc_ld_u64(const uint8_t *p) {
    return (uint64_t)bp_ld_u32(p) | ((uint64_t)bp_ld_u32(p + 4) << 32);
}

typedef struct {
    const char     *path;
    uint16_t        instance;
    bp_wctime_t    *out;
} wc_get_ctx_t;

static void wc_get_fill(uint8_t *slot, void *user) {
    wc_get_ctx_t *c = user;
    size_t n = strlen(c->path);
    if (n > 254) n = 254;
    memcpy(slot + 0x78, c->path, n);
    *(slot + 0x78 + n) = 0;
    bp_st_u16(slot + 0x178, c->instance);
    /* have_buffer = 1: we want a response */
    *(slot + 0x17A) = (c->out != NULL) ? 1 : 0;
}

static void wc_get_read(uint8_t *slot, void *user) {
    wc_get_ctx_t *c = user;
    if (!c->out) return;
    c->out->sec  = wc_ld_u64(slot + 0x180);
    c->out->nsec = wc_ld_u64(slot + 0x188);
    c->out->aux0 = wc_ld_u64(slot + 0x190);
    c->out->aux1 = wc_ld_u64(slot + 0x198);
    c->out->aux2 = wc_ld_u64(slot + 0x1A0);
    c->out->aux3 = wc_ld_u64(slot + 0x1A8);
}

static int wc_get_dispatch(bp_client_t *cl, const char *fn,
                            const char *path, uint16_t instance,
                            bp_wctime_t *out) {
    if (!cl || !path || !out) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254)   return BP_ERR_PARAM_RANGE;
    memset(out, 0, sizeof(*out));
    wc_get_ctx_t ctx = { .path = path, .instance = instance, .out = out };
    bp_call_spec_t spec = {
        .fn_name      = fn,
        .payload_size = WC_PAYLOAD_SIZE,
        .fill_payload = wc_get_fill,
        .read_reply   = wc_get_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

int bp_client_get_wctime(bp_client_t *cl, const char *path,
                          uint16_t instance, bp_wctime_t *out) {
    return wc_get_dispatch(cl, "OCXcip_GetWCTime", path, instance, out);
}
int bp_client_get_wctime_utc(bp_client_t *cl, const char *path,
                              uint16_t instance, bp_wctime_t *out) {
    return wc_get_dispatch(cl, "OCXcip_GetWCTimeUTC", path, instance, out);
}

typedef struct {
    const char         *path;
    uint16_t            instance;
    const bp_wctime_t  *in;
} wc_set_ctx_t;

static void wc_set_fill(uint8_t *slot, void *user) {
    wc_set_ctx_t *c = user;
    size_t n = strlen(c->path);
    if (n > 254) n = 254;
    memcpy(slot + 0x78, c->path, n);
    *(slot + 0x78 + n) = 0;
    bp_st_u16(slot + 0x178, c->instance);
    *(slot + 0x17A) = (c->in != NULL) ? 1 : 0;
    if (c->in) {
        wc_st_u64(slot + 0x180, c->in->sec);
        wc_st_u64(slot + 0x188, c->in->nsec);
        wc_st_u64(slot + 0x190, c->in->aux0);
        wc_st_u64(slot + 0x198, c->in->aux1);
        wc_st_u64(slot + 0x1A0, c->in->aux2);
        wc_st_u64(slot + 0x1A8, c->in->aux3);
    }
}

static int wc_set_dispatch(bp_client_t *cl, const char *fn,
                            const char *path, uint16_t instance,
                            const bp_wctime_t *in) {
    if (!cl || !path) return BP_ERR_NULL_ARG;
    if (strlen(path) > 254) return BP_ERR_PARAM_RANGE;
    wc_set_ctx_t ctx = { .path = path, .instance = instance, .in = in };
    bp_call_spec_t spec = {
        .fn_name      = fn,
        .payload_size = WC_PAYLOAD_SIZE,
        .fill_payload = wc_set_fill,
        .read_reply   = NULL,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

int bp_client_set_wctime(bp_client_t *cl, const char *path,
                          uint16_t instance, const bp_wctime_t *in) {
    return wc_set_dispatch(cl, "OCXcip_SetWCTime", path, instance, in);
}
int bp_client_set_wctime_utc(bp_client_t *cl, const char *path,
                              uint16_t instance, const bp_wctime_t *in) {
    return wc_set_dispatch(cl, "OCXcip_SetWCTimeUTC", path, instance, in);
}

/* ────────── decoders (v0.10.2+) ─────────────────────────────── */

/* Unix epoch seconds for each per-device epoch.  Verified by hand
 * (e.g. 2000-01-01 UTC = 946684800 unix). */
static int64_t epoch_unix_seconds(bp_wctime_epoch_t e) {
    switch (e) {
    case BP_WCTIME_EPOCH_UNIX: return 0;
    case BP_WCTIME_EPOCH_1972: return  63072000LL;   /* 2 years past Unix */
    case BP_WCTIME_EPOCH_1998: return 883612800LL;
    case BP_WCTIME_EPOCH_2000: return 946684800LL;
    }
    return 0;
}

int64_t bp_wctime_to_unix_us(const bp_wctime_t *wc, bp_wctime_epoch_t epoch) {
    if (!wc) return 0;
    return (int64_t)wc->sec + epoch_unix_seconds(epoch) * 1000000LL;
}

size_t bp_wctime_tz_name(const bp_wctime_t *wc, char *out, size_t out_size) {
    if (!wc || !out || out_size == 0) return 0;
    /* Read 32 bytes from aux0..aux3 in little-endian byte order
     * (i.e. memcpy from the qwords as they appear in memory on a
     * LE platform — both AArch64 and x86 qualify).  Stop at the
     * first NUL byte or buffer limit. */
    uint64_t qwords[4] = { wc->aux0, wc->aux1, wc->aux2, wc->aux3 };
    const uint8_t *bytes = (const uint8_t *)qwords;
    size_t n = 0;
    size_t max_copy = out_size - 1;
    if (max_copy > 32) max_copy = 32;
    for (; n < max_copy; n++) {
        if (bytes[n] == 0) break;
        out[n] = (char)bytes[n];
    }
    out[n] = '\0';
    return n;
}
