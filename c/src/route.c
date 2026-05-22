/*
 * route.c — Unconnected_Send (CIP svc 0x52) body assembly +
 *           port-segment helpers (v0.8.0 Phase 3).
 *
 * Wire format documented in docs/protocol.md "Multi-hop routes —
 * Unconnected_Send (service 0x52)".  Keep this file in sync with
 * go/ocxbp/cip/route.go and python/src/bpclient/_route.py.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <stdint.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

/* Tick = 5 → 32 ms units (priority 0).  Standard CIP encoding for
 * Unconnected_Send on Logix gateways. */
#define UCS_TICK_VAL  5
#define UCS_TICK_MS   32

int bp_build_unconnected_send(uint8_t *out, size_t out_cap,
                                const uint8_t *embedded_msg, size_t embedded_len,
                                const uint8_t *route_path,  size_t route_len,
                                uint16_t timeout_ms) {
    if (!out || !embedded_msg || !route_path) return 0;
    if ((route_len & 1) != 0) return -EINVAL;       /* must be word-aligned */
    if (embedded_len == 0 || embedded_len > 0xFFFF) return -EINVAL;
    if (route_len == 0 || route_len > 0x1FE) return -EINVAL;  /* size in words fits in 1 byte */

    /* timeout_ms → ticks (1..255).  At tick=5 (32 ms), max representable
     * is 255 * 32 = 8160 ms.  Anything larger gets clamped. */
    uint32_t ticks_u = (timeout_ms + UCS_TICK_MS - 1) / UCS_TICK_MS;
    if (ticks_u < 1)   ticks_u = 1;
    if (ticks_u > 255) ticks_u = 255;

    size_t pad = embedded_len & 1;
    size_t total = 10 + embedded_len + pad + 2 + route_len;
    if (total > out_cap) return -ENOSPC;

    size_t off = 0;
    out[off++] = 0x52;                  /* service: Unconnected_Send */
    out[off++] = 0x02;                  /* path_size in words */
    out[off++] = 0x20; out[off++] = 0x06;  /* class 0x06 = ConnMgr */
    out[off++] = 0x24; out[off++] = 0x01;  /* instance 1 */
    out[off++] = (UCS_TICK_VAL & 0x0F); /* priority 0, tick = 5 (32 ms) */
    out[off++] = (uint8_t)ticks_u;      /* timeout_ticks */
    out[off++] = (uint8_t)(embedded_len & 0xFF);
    out[off++] = (uint8_t)((embedded_len >> 8) & 0xFF);
    memcpy(out + off, embedded_msg, embedded_len);
    off += embedded_len;
    if (pad) out[off++] = 0x00;
    out[off++] = (uint8_t)(route_len / 2);  /* route_path size in words */
    out[off++] = 0x00;                  /* reserved */
    memcpy(out + off, route_path, route_len);
    off += route_len;
    return (int)off;
}

int bp_route_append_port(uint8_t *route, size_t cap, size_t *off,
                          uint8_t port, uint8_t link) {
    if (!route || !off) return BP_ERR_NULL_ARG;
    if (*off + 2 > cap) return BP_ERR_PARAM_RANGE;
    route[*off]     = port;
    route[*off + 1] = link;
    *off += 2;
    return BP_OK;
}
