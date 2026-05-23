/*
 * path.c — bp_client_parse_path: OCXcip_ParsePath dispatch.
 *
 * Wire format RE'd from libocxbpapi-w.so:0x1094f0 — see
 * docs/protocol.md "OCXcip_ParsePath".  Same decode that the
 * diagnostic `pathprobe` example uses (c/examples/pathprobe.c).
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

#define PP_PATH_OFF      0x078u
#define PP_CLASS_OFF     0x178u
#define PP_SEGFLAGS_OFF  0x17au
#define PP_INSTANCE_OFF  0x17cu
#define PP_ENCODED_OFF   0x180u
#define PP_SIZE_OFF      0x280u
#define PP_ATTRFLAGS_OFF 0x282u
#define PP_PAYLOAD_SIZE  0x288u

typedef struct {
    const char       *text;
    uint16_t          cap;
    bp_parsed_path_t *out;
} pp_ctx_t;

static void pp_fill(uint8_t *slot, void *user) {
    pp_ctx_t *c = user;
    size_t n = strlen(c->text);
    if (n > 254) n = 254;
    memcpy(slot + PP_PATH_OFF, c->text, n);
    *(slot + PP_PATH_OFF + n) = 0;
    /* tell the engine the caller buffer capacity */
    bp_st_u16(slot + PP_SIZE_OFF, c->cap);
}

static void pp_read(uint8_t *slot, void *user) {
    pp_ctx_t *c = user;
    bp_parsed_path_t *out = c->out;
    out->encoded_size  = bp_ld_u16(slot + PP_SIZE_OFF);
    if (out->encoded_size > sizeof(out->encoded)) out->encoded_size = sizeof(out->encoded);
    memcpy(out->encoded, slot + PP_ENCODED_OFF, out->encoded_size);
    out->cip_class     = bp_ld_u16(slot + PP_CLASS_OFF);
    out->segment_flags = *(slot + PP_SEGFLAGS_OFF);
    out->instance      = bp_ld_u32(slot + PP_INSTANCE_OFF);
    out->attr_flags    = *(slot + PP_ATTRFLAGS_OFF);
}

int bp_client_parse_path(bp_client_t *cl, const char *text,
                          bp_parsed_path_t *out) {
    if (!cl || !text || !out) return BP_ERR_NULL_ARG;
    if (strlen(text) > 254)   return BP_ERR_PARAM_RANGE;
    memset(out, 0, sizeof(*out));
    pp_ctx_t ctx = { .text = text, .cap = sizeof(out->encoded), .out = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_ParsePath",
        .payload_size = PP_PAYLOAD_SIZE,
        .fill_payload = pp_fill,
        .read_reply   = pp_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}
