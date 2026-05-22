/*
 * msgprobe.c — manual OCXcip_MessageSend invocation + raw response
 *              hex dump.  Used to RE the wire format empirically
 *              (specifically: what does `encoded_path` contain —
 *              EPATH only, or EPATH + request body?).
 *
 * Usage examples:
 *   # Identity vendor ID of LOCAL cm1756 (no backplane routing):
 *   msgprobe --service 0x0e --class 1 \
 *            --path "20 01 24 01 30 01"
 *
 *   # Identity vendor ID of L85E (slot 2):
 *   msgprobe --service 0x0e --class 1 \
 *            --path "01 02 20 01 24 01 30 01"
 *
 *   # Get_Attribute_All (svc 0x01) on Identity:
 *   msgprobe --service 0x01 --class 1 \
 *            --path "01 02 20 01 24 01"
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"
#include "../src/proto.h"

/* Wire-format offsets (RE'd from libocxbpapi-w.so OCXcip_MessageSend
 * at 0x109ec0).  All offsets are from the slot's base; multiply by 2
 * when comparing against the Ghidra decompile of puStack_8[N]. */
#define MS_PATH_OFF        0x00078u  /* encoded_path bytes (length = MS_PATH_SIZE) */
#define MS_PATH_SIZE_OFF   0x19078u  /* uint16 byte count of MS_PATH */
#define MS_RESPDATA_OFF    0x1907au  /* response data buffer (and possibly req body) */
#define MS_RESPLEN_OFF     0x3207au  /* uint16 in: capacity; out: actual response length */
#define MS_STATUS_OFF      0x3207cu  /* uint32 out: status (CIP general + extended?) */
#define MS_SERVICE_OFF     0x32080u  /* uint8 service code */
#define MS_CLASSMISC_OFF   0x32082u  /* uint16 class word / misc */

typedef struct {
    const uint8_t *path;
    uint16_t       path_size;
    uint16_t       resp_capacity;
    uint8_t        service;
    uint16_t       class_or_misc;

    uint8_t        resp_buf[4096];
    uint16_t       resp_len;       /* server-written */
    uint32_t       status;         /* server-written */
} mp_ctx_t;

static void mp_fill(uint8_t *slot, void *user) {
    mp_ctx_t *c = user;
    memcpy(slot + MS_PATH_OFF, c->path, c->path_size);
    bp_st_u16(slot + MS_PATH_SIZE_OFF, c->path_size);
    bp_st_u16(slot + MS_RESPLEN_OFF,   c->resp_capacity);
    *(slot + MS_SERVICE_OFF) = c->service;
    bp_st_u16(slot + MS_CLASSMISC_OFF, c->class_or_misc);
}

static void mp_read(uint8_t *slot, void *user) {
    mp_ctx_t *c = user;
    c->resp_len = bp_ld_u16(slot + MS_RESPLEN_OFF);
    c->status   = bp_ld_u32(slot + MS_STATUS_OFF);
    if (c->resp_len > sizeof(c->resp_buf)) c->resp_len = sizeof(c->resp_buf);
    memcpy(c->resp_buf, slot + MS_RESPDATA_OFF, c->resp_len);
}

static void hexdump(const void *buf, size_t len) {
    const uint8_t *b = buf;
    for (size_t i = 0; i < len; i += 16) {
        printf("    +%03zx ", i);
        size_t line = (len - i) > 16 ? 16 : (len - i);
        for (size_t j = 0; j < line; j++) printf("%02x ", b[i+j]);
        for (size_t j = line; j < 16; j++) printf("   ");
        printf(" ");
        for (size_t j = 0; j < line; j++) {
            unsigned char ch = b[i+j];
            putchar((ch >= 0x20 && ch < 0x7f) ? (char)ch : '.');
        }
        putchar('\n');
    }
}

/* Parse a hex byte sequence like "20 01 24 01 30 01" (spaces optional). */
static size_t parse_hex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*s && n < cap) {
        while (*s && !isxdigit((unsigned char)*s)) s++;
        if (!*s) break;
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

int main(int argc, char *argv[]) {
    int service = -1;
    int class_or_misc = 0;
    const char *path_hex = NULL;

    static struct option opts[] = {
        {"service", required_argument, 0, 's'},
        {"class",   required_argument, 0, 'c'},
        {"path",    required_argument, 0, 'p'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "s:c:p:", opts, &idx)) != -1) {
        if      (oc == 's') service = (int)strtol(optarg, NULL, 0);
        else if (oc == 'c') class_or_misc = (int)strtol(optarg, NULL, 0);
        else if (oc == 'p') path_hex = optarg;
    }
    if (service < 0 || !path_hex) {
        fprintf(stderr, "Use --service 0x0e --class 1 --path \"20 01 24 01 30 01\"\n");
        return 2;
    }

    uint8_t path_bytes[256];
    size_t path_len = parse_hex(path_hex, path_bytes, sizeof(path_bytes));
    if (path_len == 0) { fprintf(stderr, "empty path\n"); return 2; }

    printf("[msgprobe] service=0x%02x class_or_misc=0x%04x path=", service, class_or_misc);
    for (size_t i = 0; i < path_len; i++) printf("%02x ", path_bytes[i]);
    printf("(%zu bytes)\n\n", path_len);

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) { fprintf(stderr, "Open failed\n"); return 2; }
    bp_client_open_session(cl, NULL);

    mp_ctx_t ctx = {
        .path          = path_bytes,
        .path_size     = (uint16_t)path_len,
        .resp_capacity = sizeof(ctx.resp_buf),
        .service       = (uint8_t)service,
        .class_or_misc = (uint16_t)class_or_misc,
    };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_MessageSend",
        .payload_size = 0x32088,
        .fill_payload = mp_fill,
        .read_reply   = mp_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    int rc = bp_client_call(cl, &spec);

    printf("rc            = %d (0x%x)\n", rc, (unsigned)rc);
    printf("response_len  = %u\n", ctx.resp_len);
    printf("status field  = 0x%08x\n", ctx.status);
    if (ctx.resp_len) {
        printf("response bytes:\n");
        hexdump(ctx.resp_buf, ctx.resp_len > 256 ? 256 : ctx.resp_len);
    }

    bp_client_close(cl);
    return rc == 0 ? 0 : 1;
}
