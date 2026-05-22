/*
 * module.c — local cm1756 module utilities (LED / Display / Switch).
 *
 * Wire formats RE'd from:
 *   OCXcip_GetSwitchPosition @ 0x10a7b0   payload_size = 0x80
 *   OCXcip_GetLED            @ 0x09044    payload_size = 0x80
 *   OCXcip_SetLED            @ 0x08ed0    payload_size = 0x80
 *   OCXcip_GetDisplay        @ 0x09360    payload_size = 0x80
 *   OCXcip_SetDisplay        @ 0x091d0    payload_size = 0x80
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

/* ----- Switch position ----- */
typedef struct { uint32_t *out; } sw_ctx_t;
static void sw_read(uint8_t *slot, void *user) {
    sw_ctx_t *c = user;
    *c->out = bp_ld_u32(slot + 0x78);
}
int bp_client_get_switch_position(bp_client_t *cl, uint32_t *out) {
    if (!cl || !out) return BP_ERR_NULL_ARG;
    sw_ctx_t ctx = { .out = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetSwitchPosition",
        .payload_size = 0x80,
        .fill_payload = NULL,
        .read_reply   = sw_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

/* ----- LED get / set ----- */
typedef struct { uint32_t led_id; uint32_t *out_state; } led_get_ctx_t;
static void led_get_fill(uint8_t *slot, void *user) {
    led_get_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->led_id);
}
static void led_get_read(uint8_t *slot, void *user) {
    led_get_ctx_t *c = user;
    *c->out_state = bp_ld_u32(slot + 0x7c);
}
int bp_client_get_led(bp_client_t *cl, uint32_t led_id, uint32_t *out) {
    if (!cl || !out) return BP_ERR_NULL_ARG;
    led_get_ctx_t ctx = { .led_id = led_id, .out_state = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetLED",
        .payload_size = 0x80,
        .fill_payload = led_get_fill,
        .read_reply   = led_get_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

typedef struct { uint32_t led_id; uint32_t state; } led_set_ctx_t;
static void led_set_fill(uint8_t *slot, void *user) {
    led_set_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->led_id);
    bp_st_u32(slot + 0x7c, c->state);
}
int bp_client_set_led(bp_client_t *cl, uint32_t led_id, uint32_t state) {
    if (!cl) return BP_ERR_NULL_ARG;
    led_set_ctx_t ctx = { .led_id = led_id, .state = state };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_SetLED",
        .payload_size = 0x80,
        .fill_payload = led_set_fill,
        .read_reply   = NULL,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

/* ----- Display get / set ----- */
typedef struct { char *out_five_chars; } disp_get_ctx_t;
static void disp_get_read(uint8_t *slot, void *user) {
    disp_get_ctx_t *c = user;
    memcpy(c->out_five_chars, slot + 0x78, 4);
    c->out_five_chars[4] = 0;
}
int bp_client_get_display(bp_client_t *cl, char out[5]) {
    if (!cl || !out) return BP_ERR_NULL_ARG;
    memset(out, 0, 5);
    disp_get_ctx_t ctx = { .out_five_chars = out };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_GetDisplay",
        .payload_size = 0x80,
        .fill_payload = NULL,
        .read_reply   = disp_get_read,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}

typedef struct { const char *four; } disp_set_ctx_t;
static void disp_set_fill(uint8_t *slot, void *user) {
    disp_set_ctx_t *c = user;
    memcpy(slot + 0x78, c->four, 4);
    *(slot + 0x7c) = 0;
}
int bp_client_set_display(bp_client_t *cl, const char four[4]) {
    if (!cl || !four) return BP_ERR_NULL_ARG;
    disp_set_ctx_t ctx = { .four = four };
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_SetDisplay",
        .payload_size = 0x80,
        .fill_payload = disp_set_fill,
        .read_reply   = NULL,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec);
}
