/*
 * actnodes.c — call OCXcip_GetActiveNodeTable and print the
 *              bitmap of backplane slots that have responsive
 *              devices.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>

#include "bpclient.h"
#include "../src/proto.h"

/* OCXcip_GetActiveNodeTable wire format (from libocxbpapi-w.so 0x1082b0):
 *   payload_size = 0x80
 *   no input
 *   slot + 0x78  uint32 active_lo   (bitmap for slots 0..31)
 *   slot + 0x7c  uint32 active_hi   (bitmap for slots 32..63)
 */
typedef struct { uint32_t lo, hi; } an_ctx_t;

static void an_read(uint8_t *slot, void *user) {
    an_ctx_t *c = user;
    c->lo = bp_ld_u32(slot + 0x78);
    c->hi = bp_ld_u32(slot + 0x7c);
}

int main(void) {
    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);

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
    bp_client_close(cl);

    if (rc != BP_OK) {
        printf("[actnodes] rc=%d (%s)\n", rc, bp_strerror(rc));
        return 1;
    }
    printf("[actnodes] active_lo = 0x%08x  active_hi = 0x%08x\n", ctx.lo, ctx.hi);
    printf("active slots:");
    int count = 0;
    for (int i = 0; i < 32; i++) {
        if (ctx.lo & (1u << i)) { printf(" %d", i); count++; }
    }
    for (int i = 0; i < 32; i++) {
        if (ctx.hi & (1u << i)) { printf(" %d", i + 32); count++; }
    }
    printf("  (%d total)\n", count);
    return 0;
}
