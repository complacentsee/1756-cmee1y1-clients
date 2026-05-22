/*
 * pathprobe.c — invoke OCXcip_ParsePath and dump the encoded EPATH
 *               bytes it produces.  Useful when hand-building the
 *               route_path inside a routed Unconnected_Send (svc 0x52)
 *               carried in bp_client_message_send().cip_request.
 *
 * Usage:
 *   pathprobe "P:1,S:2"
 *   pathprobe "P:1,S:1"
 *   pathprobe "1,2,3"      (Rockwell-style — likely rejected)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"
#include "../src/proto.h"

/* OCXcip_ParsePath wire format (from libocxbpapi-w.so 0x1094f0):
 *   payload_size = 0x288
 *   request:
 *     slot + 0x78  text_path string (NUL-terminated, max 255 bytes)
 *     slot + 0x280 caller's path buffer capacity (u16)
 *   response:
 *     slot + 0x178 out_class (u16)
 *     slot + 0x17a out_segment_flags (u8)
 *     slot + 0x17c out_instance (u32)
 *     slot + 0x180 out_encoded_path bytes (up to 256)
 *     slot + 0x280 out_path_size (u16)
 *     slot + 0x282 out_attr_flags (u8)
 */
#define PP_PATH_OFF        0x078u
#define PP_CLASS_OFF       0x178u
#define PP_SEGFLAGS_OFF    0x17au
#define PP_INSTANCE_OFF    0x17cu
#define PP_ENCODED_OFF     0x180u
#define PP_SIZE_OFF        0x280u
#define PP_ATTRFLAGS_OFF   0x282u
#define PP_PAYLOAD_SIZE    0x288u

typedef struct {
    const char *text_path;
    uint16_t    cap;
    uint8_t     encoded[256];
    uint16_t    encoded_len;
    uint16_t    out_class;
    uint8_t     out_seg_flags;
    uint32_t    out_instance;
    uint8_t     out_attr_flags;
} pp_ctx_t;

static void pp_fill(uint8_t *slot, void *user) {
    pp_ctx_t *c = user;
    size_t n = strlen(c->text_path);
    if (n > 254) n = 254;
    memcpy(slot + PP_PATH_OFF, c->text_path, n);
    *(slot + PP_PATH_OFF + n) = 0;
    bp_st_u16(slot + PP_SIZE_OFF, c->cap);
}

static void pp_read(uint8_t *slot, void *user) {
    pp_ctx_t *c = user;
    c->encoded_len   = bp_ld_u16(slot + PP_SIZE_OFF);
    if (c->encoded_len > sizeof(c->encoded)) c->encoded_len = sizeof(c->encoded);
    memcpy(c->encoded, slot + PP_ENCODED_OFF, c->encoded_len);
    c->out_class      = bp_ld_u16(slot + PP_CLASS_OFF);
    c->out_seg_flags  = *(slot + PP_SEGFLAGS_OFF);
    c->out_instance   = bp_ld_u32(slot + PP_INSTANCE_OFF);
    c->out_attr_flags = *(slot + PP_ATTRFLAGS_OFF);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <text-path>\n", argv[0]);
        return 2;
    }
    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "client open failed\n"); return 2;
    }
    bp_client_open_session(cl, NULL);

    pp_ctx_t ctx = { .text_path = argv[1], .cap = sizeof(ctx.encoded) };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_ParsePath",
        .payload_size = PP_PAYLOAD_SIZE,
        .fill_payload = pp_fill,
        .read_reply   = pp_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    int rc = bp_client_call(cl, &spec);
    bp_client_close(cl);

    printf("[pathprobe] text='%s'  rc=%d  encoded_len=%u\n",
           argv[1], rc, ctx.encoded_len);
    if (rc != BP_OK) return 1;

    printf("  out_class      = 0x%04x\n", ctx.out_class);
    printf("  out_seg_flags  = 0x%02x\n", ctx.out_seg_flags);
    printf("  out_instance   = 0x%08x  (%u)\n", ctx.out_instance, ctx.out_instance);
    printf("  out_attr_flags = 0x%02x\n", ctx.out_attr_flags);
    printf("  encoded path   = ");
    for (uint16_t i = 0; i < ctx.encoded_len; i++) printf("%02x ", ctx.encoded[i]);
    printf("\n");
    return 0;
}
