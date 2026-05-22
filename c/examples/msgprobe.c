/*
 * msgprobe.c — manual OCXcip_MessageSend invocation + raw response
 *              hex dump.  RE/diagnostic tool — bypasses the public
 *              bp_message_t struct and writes the IPC slot directly
 *              so we can verify what the wrapper does with each
 *              field independently.
 *
 * Usage examples:
 *   # Get_Attributes_All on Identity, slot 1 (L73):
 *   msgprobe --slot 1 --req "01 02 20 01 24 01"
 *
 *   # Get_Attribute_Single (svc 0x0e), attribute 1 = vendor, on slot 2 (L85):
 *   msgprobe --slot 2 --req "0e 03 20 01 24 01 30 01"
 *
 *   # Hand-crafted Unconnected_Send (svc 0x52) — chip just forwards the
 *   # bytes, target slot 1 routes to the named device.
 *   msgprobe --slot 1 --req "52 02 20 06 24 01 ..."
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

/* Wire-format offsets — see src/message.c for the full RE notes. */
#define MS_REQ_OFF         0x00078u
#define MS_REQ_SIZE_OFF    0x19078u
#define MS_RESPDATA_OFF    0x1907au
#define MS_RESPLEN_OFF     0x3207au
#define MS_STATUS_OFF      0x3207cu
#define MS_SLOT_OFF        0x32080u
#define MS_TIMEOUT_OFF     0x32082u

typedef struct {
    const uint8_t *req;
    uint16_t       req_size;
    uint16_t       resp_capacity;
    uint8_t        slot;
    uint16_t       timeout_ms;

    uint8_t        resp_buf[4096];
    uint16_t       resp_len;       /* server-written */
    uint32_t       status;         /* server-written */
} mp_ctx_t;

static void mp_fill(uint8_t *slot, void *user) {
    mp_ctx_t *c = user;
    memcpy(slot + MS_REQ_OFF, c->req, c->req_size);
    bp_st_u16(slot + MS_REQ_SIZE_OFF, c->req_size);
    bp_st_u16(slot + MS_RESPLEN_OFF,  c->resp_capacity);
    *(slot + MS_SLOT_OFF) = c->slot;
    bp_st_u16(slot + MS_TIMEOUT_OFF, c->timeout_ms);
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
    int slot = -1;
    int timeout_ms = 0;
    const char *req_hex = NULL;

    static struct option opts[] = {
        {"slot",       required_argument, 0, 's'},
        {"req",        required_argument, 0, 'r'},
        {"timeout-ms", required_argument, 0, 't'},
        /* legacy aliases (deprecated) */
        {"service",    required_argument, 0, 's'},
        {"path",       required_argument, 0, 'r'},
        {"class",      required_argument, 0, 't'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "s:r:t:", opts, &idx)) != -1) {
        if      (oc == 's') slot       = (int)strtol(optarg, NULL, 0);
        else if (oc == 'r') req_hex    = optarg;
        else if (oc == 't') timeout_ms = (int)strtol(optarg, NULL, 0);
    }
    if (slot < 0 || !req_hex) {
        fprintf(stderr, "Use --slot 1 --req \"01 02 20 01 24 01\" [--timeout-ms 1000]\n");
        return 2;
    }

    uint8_t req_bytes[256];
    size_t req_len = parse_hex(req_hex, req_bytes, sizeof(req_bytes));
    if (req_len == 0) { fprintf(stderr, "empty req\n"); return 2; }

    printf("[msgprobe] slot=%d timeout_ms=%d req=", slot, timeout_ms);
    for (size_t i = 0; i < req_len; i++) printf("%02x ", req_bytes[i]);
    printf("(%zu bytes)\n\n", req_len);

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) { fprintf(stderr, "Open failed\n"); return 2; }
    bp_client_open_session(cl, NULL);

    mp_ctx_t ctx = {
        .req           = req_bytes,
        .req_size      = (uint16_t)req_len,
        .resp_capacity = sizeof(ctx.resp_buf),
        .slot          = (uint8_t)slot,
        .timeout_ms    = (uint16_t)timeout_ms,
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
