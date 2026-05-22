/*
 * conn.c — class-3 connected CIP messaging.
 *           Public API: bp_client_txrx_open / txrx_msg / txrx_close.
 *
 * v0.7.0 implementation: routes the connection lifecycle through
 * bp_client_message_send (chip mailbox 0x200, UCMM transport).
 * The OCXcip_TxRxOpenConn / OCXcip_TxRxMsg / OCXcip_TxRxCloseConn
 * OEM entry points are not used — they dispatch to OCXCN_* thunks
 * pointing to a library that is missing from the cm1756 image.
 *
 * See docs/protocol.md "Connected messaging — wire format" for the
 * byte layouts.  Sibling reference: historianupdate
 * driver/apex2/daemon/apex2_cip_connection.c.
 *
 * v0.7.0 known limitations: inherits the ~500 B envelope from
 * MessageSend.  4002-byte connected transport via mbox 0x204 is
 * v0.8 territory — see docs/v0.8-large-buffer-re.md.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/bpclient.h"
#include "proto.h"

/* ────────── wire helpers ─────────────────────────────────────── */
static void put_u16le(uint8_t *p, uint16_t v) {
    p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
}
static void put_u32le(uint8_t *p, uint32_t v) {
    p[0] =  v        & 0xFF; p[1] = (v >>  8) & 0xFF;
    p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF;
}

/* OT/TO sizing.  Spec field conn_params is uint16: low 16 bits go
 * into the LFO's 32-bit conn-params dword.  0 → use the sibling's
 * known-good default (4000 B = 0x0FA0 = FO_REQUEST_OT_SIZE).
 *
 * LFO_MAX_OT_SIZE = 4002 is the absolute hardware ceiling for
 * Large Forward Open on this PLC family.  4000 is what OCX
 * negotiates and what apex2_cip_connection.c uses (4002 trips
 * L73 path-destination-unknown errors on subsequent DT Buffer
 * Create — see sibling lines 767-770).  We cap at 4002 so a
 * mis-configured caller can't trip PLC "Resource unavailable"
 * by passing a stale 16-bit OEM conn_params value (e.g. 0x43E8). */
#define LFO_DEFAULT_OT_SIZE  4000U
#define LFO_MAX_OT_SIZE      4002U
#define LFO_PARAMS_HI        0x42000000U  /* P2P + variable */
#define LFO_RPI_US           10000000U    /* 10 s — matches sibling FO_OT_RPI_US */
#define LFO_OT_HINT          0x80010000U
#define LFO_TO_HINT          0x80000001U
#define LFO_VENDOR_ID        0x0001U

/* Build a Large Forward Open (CIP service 0x5B) request body.
 * Returns the byte length written into `buf` (must be ≥ 50 bytes).
 * Mirrors historianupdate apex2_cip_connection.c::build_forward_open
 * lines 682-820. */
static int build_lfo(uint8_t *buf,
                     uint16_t conn_serial, uint32_t orig_serial,
                     uint16_t ot_size_bytes) {
    int off = 0;
    buf[off++] = 0x5B;
    buf[off++] = 0x02;
    buf[off++] = 0x20; buf[off++] = 0x06;
    buf[off++] = 0x24; buf[off++] = 0x01;
    buf[off++] = 0x05;
    buf[off++] = 0xF7;
    put_u32le(buf + off, LFO_OT_HINT);     off += 4;
    put_u32le(buf + off, LFO_TO_HINT);     off += 4;
    put_u16le(buf + off, conn_serial);     off += 2;
    put_u16le(buf + off, LFO_VENDOR_ID);   off += 2;
    put_u32le(buf + off, orig_serial);     off += 4;
    put_u32le(buf + off, 0x00000003);      off += 4;   /* timeout mult */
    put_u32le(buf + off, LFO_RPI_US);      off += 4;
    put_u32le(buf + off, LFO_PARAMS_HI | (uint32_t)ot_size_bytes); off += 4;
    put_u32le(buf + off, LFO_RPI_US);      off += 4;
    put_u32le(buf + off, LFO_PARAMS_HI | (uint32_t)ot_size_bytes); off += 4;
    buf[off++] = 0xA3;
    buf[off++] = 0x02;
    buf[off++] = 0x20; buf[off++] = 0x02;
    buf[off++] = 0x24; buf[off++] = 0x01;
    return off;
}

/* Build a Forward_Close (CIP service 0x4E) request body.
 * Returns 22.  Mirrors apex2_cip_connection.c::build_forward_close
 * lines 1244-1283. */
static int build_fc(uint8_t *buf,
                    uint16_t conn_serial, uint16_t vendor_id,
                    uint32_t orig_serial) {
    int off = 0;
    buf[off++] = 0x4E;
    buf[off++] = 0x02;
    buf[off++] = 0x20; buf[off++] = 0x06;
    buf[off++] = 0x24; buf[off++] = 0x01;
    buf[off++] = 0x0A;
    buf[off++] = 0x0E;
    put_u16le(buf + off, conn_serial);   off += 2;
    put_u16le(buf + off, vendor_id);     off += 2;
    put_u32le(buf + off, orig_serial);   off += 4;
    buf[off++] = 0x02;
    buf[off++] = 0x00;
    buf[off++] = 0x20; buf[off++] = 0x02;
    buf[off++] = 0x24; buf[off++] = 0x01;
    return off;
}

/* ────────── connection cache (per-client) ───────────────────── */
/* All accessors take cl->txrx_mu. */

/* Find an existing entry by app_handle.  Caller must hold txrx_mu. */
static struct bp_txrx_conn *find_conn(bp_client_t *cl, uint16_t app_handle) {
    for (int i = 0; i < BP_TXRX_MAX_CONNS; i++) {
        if (cl->txrx_conns[i].in_use && cl->txrx_conns[i].app_handle == app_handle)
            return &cl->txrx_conns[i];
    }
    return NULL;
}

/* Find a free entry.  Caller must hold txrx_mu. */
static struct bp_txrx_conn *alloc_conn(bp_client_t *cl) {
    for (int i = 0; i < BP_TXRX_MAX_CONNS; i++) {
        if (!cl->txrx_conns[i].in_use) return &cl->txrx_conns[i];
    }
    return NULL;
}

/* ────────── encoded_path → slot extraction ──────────────────── */
/* v0.7.0 supports only the canonical backplane-direct route:
 *   encoded_path = {0x01, slot}   (port 1 = backplane, link = slot).
 * Multi-hop / off-chassis routes go through Unconnected_Send (0x52)
 * inside the request body, not the route bytes. */
static int extract_slot(const uint8_t *path, uint16_t path_size, uint8_t *out_slot) {
    if (!path || path_size != 2 || path[0] != 0x01) return BP_ERR_PARAM_RANGE;
    if (path[1] > BP_MSG_MAX_SLOT) return BP_ERR_SLOT_TOO_LARGE;
    *out_slot = path[1];
    return BP_OK;
}

/* Generate a random non-zero conn_serial. */
static uint16_t rand_conn_serial(void) {
    /* time + pid + a single arc4random-style mix would be ideal but
     * we don't link libbsd.  rand() seeded once per process is fine
     * for "unique within a few seconds" — the PLC removes serials on
     * Forward_Close and on idle timeout. */
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned)(time(NULL) ^ (long)getpid()));
        seeded = 1;
    }
    uint16_t v = (uint16_t)(rand() & 0xFFFF);
    if (v == 0) v = 0xBEEF;
    return v;
}

static uint32_t rand_orig_serial(void) {
    return (uint32_t)getpid() ^ ((uint32_t)time(NULL) << 16);
}

/* ────────── bp_client_txrx_open ─────────────────────────────── */
int bp_client_txrx_open(bp_client_t *cl, const bp_conn_spec_t *spec,
                         uint16_t *out_conn_id, uint16_t *out_conn_serial) {
    if (!cl || !spec || !spec->encoded_path) return BP_ERR_NULL_ARG;

    uint8_t slot;
    int rc = extract_slot(spec->encoded_path, spec->path_size, &slot);
    if (rc != BP_OK) return rc;

    uint16_t ot_size = spec->conn_params ? spec->conn_params : LFO_DEFAULT_OT_SIZE;
    if (ot_size > LFO_MAX_OT_SIZE) {
        fprintf(stderr, "[bp_client_txrx_open] conn_params=%u exceeds LFO max %u; "
                        "capping (caller probably passed a stale OEM 16-bit param)\n",
                ot_size, LFO_MAX_OT_SIZE);
        ot_size = LFO_MAX_OT_SIZE;
    }
    uint16_t conn_serial = rand_conn_serial();
    uint32_t orig_serial = rand_orig_serial();

    uint8_t lfo[64];
    int lfo_len = build_lfo(lfo, conn_serial, orig_serial, ot_size);

    uint8_t resp[64];
    bp_message_t msg = {
        .slot          = slot,
        .cip_request   = lfo,
        .req_size      = (uint16_t)lfo_len,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = sizeof(resp),
    };
    rc = bp_client_message_send(cl, &msg);
    if (rc != BP_OK) return rc;

    /* CIP reply parse: 0xDB = Large FO success, 0xD4 = small FO,
     * 0x5B (no high bit) would be a malformed framing. */
    if (msg.resp_len < 12 ||
        (resp[0] != 0xDB && resp[0] != 0xD4) ||
        resp[2] != 0x00) {
        uint8_t svc = msg.resp_len ? resp[0] : 0;
        uint8_t st  = msg.resp_len >= 3 ? resp[2] : 0xFF;
        uint16_t ext = (msg.resp_len >= 6 && resp[3])
                        ? (uint16_t)(resp[4] | (resp[5] << 8)) : 0;
        bp_record_cip_error(cl, svc, st, ext, slot);
        fprintf(stderr, "[bp_client_txrx_open] LFO CIP failure: "
                        "svc=0x%02x status=0x%02x ext=0x%04x slot=%u (%s)\n",
                svc, st, ext, slot, bp_cip_status_string(st, ext));
        return BP_ERR_CIP_STATUS;
    }

    uint32_t ot_conn_id = bp_ld_u32(resp + 4);
    uint32_t to_conn_id = bp_ld_u32(resp + 8);

    /* Cache the connection state. */
    pthread_mutex_lock(&cl->txrx_mu);
    struct bp_txrx_conn *existing = find_conn(cl, spec->app_handle);
    if (existing) {
        pthread_mutex_unlock(&cl->txrx_mu);
        fprintf(stderr, "[bp_client_txrx_open] app_handle=%u already open "
                        "(slot=%u, conn_serial=0x%04x) — call txrx_close first\n",
                spec->app_handle, existing->slot, existing->conn_serial);
        /* The PLC now has an extra connection we won't close — emit a
         * best-effort Forward_Close so we don't leak PLC table entries. */
        uint8_t fc[32];
        int fc_len = build_fc(fc, conn_serial, LFO_VENDOR_ID, orig_serial);
        bp_message_t fc_msg = {
            .slot = slot, .cip_request = fc, .req_size = (uint16_t)fc_len,
            .timeout_ms = 5000, .resp_data = resp, .resp_capacity = sizeof(resp),
        };
        (void)bp_client_message_send(cl, &fc_msg);
        return BP_ERR_GENERIC;
    }
    struct bp_txrx_conn *e = alloc_conn(cl);
    if (!e) {
        pthread_mutex_unlock(&cl->txrx_mu);
        /* Best-effort FC to avoid leaking PLC state. */
        uint8_t fc[32];
        int fc_len = build_fc(fc, conn_serial, LFO_VENDOR_ID, orig_serial);
        bp_message_t fc_msg = {
            .slot = slot, .cip_request = fc, .req_size = (uint16_t)fc_len,
            .timeout_ms = 5000, .resp_data = resp, .resp_capacity = sizeof(resp),
        };
        (void)bp_client_message_send(cl, &fc_msg);
        return BP_ERR_NO_FREE_SLOT;
    }
    e->in_use      = 1;
    e->app_handle  = spec->app_handle;
    e->slot        = slot;
    e->conn_serial = conn_serial;
    e->vendor_id   = LFO_VENDOR_ID;
    e->orig_serial = orig_serial;
    e->ot_conn_id  = ot_conn_id;
    e->to_conn_id  = to_conn_id;
    e->sequence    = 0;
    pthread_mutex_unlock(&cl->txrx_mu);

    /* The OEM signature returns 16-bit values; we expose the low 16
     * of the 32-bit O→T conn ID (the meaningful routing tag) and
     * our random conn_serial.  Callers that want the full 32-bit
     * IDs can re-derive from spec->app_handle + a future getter. */
    if (out_conn_id)     *out_conn_id     = (uint16_t)(ot_conn_id & 0xFFFF);
    if (out_conn_serial) *out_conn_serial = conn_serial;
    return BP_OK;
}

/* ────────── bp_client_txrx_msg ──────────────────────────────── */
int bp_client_txrx_msg(bp_client_t *cl, const bp_conn_spec_t *spec,
                        const void *req, uint16_t req_size,
                        void *resp, uint16_t resp_capacity,
                        uint16_t *out_resp_size) {
    if (!cl || !spec) return BP_ERR_NULL_ARG;
    if (!req || req_size == 0) return BP_ERR_NULL_ARG;
    if (!resp || resp_capacity == 0) return BP_ERR_NULL_ARG;
    if (out_resp_size) *out_resp_size = 0;

    pthread_mutex_lock(&cl->txrx_mu);
    struct bp_txrx_conn *e = find_conn(cl, spec->app_handle);
    if (!e) {
        pthread_mutex_unlock(&cl->txrx_mu);
        return BP_ERR_NOT_OPEN;
    }
    uint8_t slot = e->slot;
    e->sequence++;  /* diagnostic only — not on the wire (see protocol.md) */
    pthread_mutex_unlock(&cl->txrx_mu);

    bp_message_t msg = {
        .slot          = slot,
        .cip_request   = (const uint8_t *)req,
        .req_size      = req_size,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = resp_capacity,
    };
    int rc = bp_client_message_send(cl, &msg);
    if (rc == BP_OK && out_resp_size) *out_resp_size = msg.resp_len;
    return rc;
}

/* ────────── bp_txrx_force_close_local (v0.9.0 Phase 4) ──────── */
/* Wipes a single txrx_conn slot locally — no Forward_Close on the
 * wire.  Used by the pool's auto-reopen path when the PLC has
 * already dropped the connection (keepalive ping failed); the
 * caller will re-issue Forward_Open with the same app_handle. */
int bp_txrx_force_close_local(bp_client_t *cl, uint16_t app_handle) {
    if (!cl) return BP_ERR_NULL_ARG;
    pthread_mutex_lock(&cl->txrx_mu);
    struct bp_txrx_conn *e = find_conn(cl, app_handle);
    if (!e) {
        pthread_mutex_unlock(&cl->txrx_mu);
        return BP_ERR_NOT_OPEN;
    }
    e->in_use = 0;
    pthread_mutex_unlock(&cl->txrx_mu);
    return BP_OK;
}

/* ────────── bp_client_txrx_close ────────────────────────────── */
int bp_client_txrx_close(bp_client_t *cl, const bp_conn_spec_t *spec) {
    if (!cl || !spec) return BP_ERR_NULL_ARG;

    /* Snapshot the entry under lock, then release it before doing
     * the network FC.  A second concurrent close() for the same
     * handle will find no entry and return NOT_OPEN. */
    pthread_mutex_lock(&cl->txrx_mu);
    struct bp_txrx_conn *e = find_conn(cl, spec->app_handle);
    if (!e) {
        pthread_mutex_unlock(&cl->txrx_mu);
        return BP_ERR_NOT_OPEN;
    }
    uint8_t  slot        = e->slot;
    uint16_t conn_serial = e->conn_serial;
    uint16_t vendor_id   = e->vendor_id;
    uint32_t orig_serial = e->orig_serial;
    /* Free the slot immediately — even if the FC fails the PLC's
     * idle timeout will clean up, and we don't want a stuck slot
     * to block subsequent opens with the same app_handle. */
    e->in_use = 0;
    pthread_mutex_unlock(&cl->txrx_mu);

    uint8_t fc[32];
    int fc_len = build_fc(fc, conn_serial, vendor_id, orig_serial);

    uint8_t resp[64];
    bp_message_t msg = {
        .slot          = slot,
        .cip_request   = fc,
        .req_size      = (uint16_t)fc_len,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = sizeof(resp),
    };
    int rc = bp_client_message_send(cl, &msg);
    if (rc != BP_OK) return rc;

    if (msg.resp_len < 4 || resp[0] != 0xCE || resp[2] != 0x00) {
        uint8_t svc = msg.resp_len ? resp[0] : 0;
        uint8_t st  = msg.resp_len >= 3 ? resp[2] : 0xFF;
        uint16_t ext = (msg.resp_len >= 6 && resp[3])
                        ? (uint16_t)(resp[4] | (resp[5] << 8)) : 0;
        bp_record_cip_error(cl, svc, st, ext, slot);
        fprintf(stderr, "[bp_client_txrx_close] FC CIP failure: "
                        "svc=0x%02x status=0x%02x ext=0x%04x slot=%u serial=0x%04x (%s)\n",
                svc, st, ext, slot, conn_serial, bp_cip_status_string(st, ext));
        return BP_ERR_CIP_STATUS;
    }
    return BP_OK;
}
