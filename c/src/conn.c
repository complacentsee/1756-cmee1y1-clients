/*
 * conn.c — class-3 connected CIP messaging
 *           (OCXcip_TxRxOpenConn / TxRxMsg / TxRxCloseConn).
 *
 * Wire formats RE'd from libocxbpapi-w.so:
 *   OCXcip_TxRxOpenConn  @ 0x1097b0   payload 0x188
 *   OCXcip_TxRxMsg       @ 0x109bd4   payload 0x32190
 *   OCXcip_TxRxCloseConn @ 0x1099f0   payload 0x188
 *
 * All three share the leading layout:
 *   slot + 0x78   uint16  app_handle
 *   slot + 0x7c   uint32  options
 *   slot + 0x80   uint8[] encoded_path bytes (max 0xff)
 *   slot + 0x180  uint16  path_size
 *
 * TxRxOpenConn / TxRxMsg additionally have:
 *   slot + 0x182  uint16  conn_params
 *
 * TxRxOpenConn response:
 *   slot + 0x184  uint16  conn_id
 *   slot + 0x186  uint16  conn_serial
 *
 * TxRxMsg request:
 *   slot + 0x184      uint8[]  req_buf data (req_size bytes)
 *   slot + 0x19084    uint16   req_size
 *   slot + 0x32186    uint16   resp_capacity (in) / resp_size (out)
 * TxRxMsg response:
 *   slot + 0x19086    uint8[]  response data (length = resp_size)
 *
 * SPDX-License-Identifier: MIT
 */
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

#define TXRX_APP_HANDLE_OFF   0x00078u
#define TXRX_OPTIONS_OFF      0x0007cu
#define TXRX_PATH_OFF         0x00080u
#define TXRX_PATH_SIZE_OFF    0x00180u
#define TXRX_CONN_PARAMS_OFF  0x00182u
#define TXRX_CONN_ID_OFF      0x00184u   /* OpenConn response only */
#define TXRX_CONN_SERIAL_OFF  0x00186u

#define TXRXMSG_REQ_BUF_OFF   0x00184u
#define TXRXMSG_REQ_SIZE_OFF  0x19084u
#define TXRXMSG_RESP_BUF_OFF  0x19086u
#define TXRXMSG_RESP_LEN_OFF  0x32186u

#define TXRX_OPENCLOSE_PAYLOAD  0x188u
#define TXRXMSG_PAYLOAD         0x32190u
#define TXRX_MAX_PATH           0xffu

/* ---------- TxRxOpenConn ---------- */
typedef struct {
    const bp_conn_spec_t *spec;
    uint16_t             *out_conn_id;
    uint16_t             *out_conn_serial;
} open_ctx_t;

static void open_fill(uint8_t *slot, void *user) {
    open_ctx_t *c = user;
    bp_st_u16(slot + TXRX_APP_HANDLE_OFF,  c->spec->app_handle);
    bp_st_u32(slot + TXRX_OPTIONS_OFF,     c->spec->options);
    memcpy(slot + TXRX_PATH_OFF,           c->spec->encoded_path, c->spec->path_size);
    bp_st_u16(slot + TXRX_PATH_SIZE_OFF,   c->spec->path_size);
    bp_st_u16(slot + TXRX_CONN_PARAMS_OFF, c->spec->conn_params);
}

static void open_read(uint8_t *slot, void *user) {
    open_ctx_t *c = user;
    if (c->out_conn_id)     *c->out_conn_id     = bp_ld_u16(slot + TXRX_CONN_ID_OFF);
    if (c->out_conn_serial) *c->out_conn_serial = bp_ld_u16(slot + TXRX_CONN_SERIAL_OFF);
}

int bp_client_txrx_open(bp_client_t *cl, const bp_conn_spec_t *spec,
                         uint16_t *out_conn_id, uint16_t *out_conn_serial) {
    if (!cl || !spec || !spec->encoded_path) return BP_ERR_NULL_ARG;
    if (spec->path_size == 0 || spec->path_size > TXRX_MAX_PATH) return BP_ERR_PARAM_RANGE;
    open_ctx_t ctx = { .spec = spec, .out_conn_id = out_conn_id, .out_conn_serial = out_conn_serial };
    bp_call_spec_t spec_call = {
        .fn_name      = "OCXcip_TxRxOpenConn",
        .payload_size = TXRX_OPENCLOSE_PAYLOAD,
        .fill_payload = open_fill,
        .read_reply   = open_read,
        .timeout_ms   = 30000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec_call);
}

/* ---------- TxRxMsg ---------- */
typedef struct {
    const bp_conn_spec_t *spec;
    const void           *req;
    uint16_t              req_size;
    void                 *resp;
    uint16_t              resp_capacity;
    uint16_t             *out_resp_size;
} msg_ctx_t;

static void msg_fill(uint8_t *slot, void *user) {
    msg_ctx_t *c = user;
    bp_st_u16(slot + TXRX_APP_HANDLE_OFF,   c->spec->app_handle);
    bp_st_u32(slot + TXRX_OPTIONS_OFF,      c->spec->options);
    memcpy(slot + TXRX_PATH_OFF,            c->spec->encoded_path, c->spec->path_size);
    bp_st_u16(slot + TXRX_PATH_SIZE_OFF,    c->spec->path_size);
    bp_st_u16(slot + TXRX_CONN_PARAMS_OFF,  c->spec->conn_params);
    if (c->req && c->req_size > 0) {
        memcpy(slot + TXRXMSG_REQ_BUF_OFF, c->req, c->req_size);
    }
    bp_st_u16(slot + TXRXMSG_REQ_SIZE_OFF,  c->req_size);
    bp_st_u16(slot + TXRXMSG_RESP_LEN_OFF,  c->resp_capacity);
}

static void msg_read(uint8_t *slot, void *user) {
    msg_ctx_t *c = user;
    uint16_t got = bp_ld_u16(slot + TXRXMSG_RESP_LEN_OFF);
    if (got > c->resp_capacity) got = c->resp_capacity;
    if (c->resp && got > 0) {
        memcpy(c->resp, slot + TXRXMSG_RESP_BUF_OFF, got);
    }
    if (c->out_resp_size) *c->out_resp_size = got;
}

int bp_client_txrx_msg(bp_client_t *cl, const bp_conn_spec_t *spec,
                        const void *req, uint16_t req_size,
                        void *resp, uint16_t resp_capacity,
                        uint16_t *out_resp_size) {
    if (!cl || !spec || !spec->encoded_path)     return BP_ERR_NULL_ARG;
    if (!resp || resp_capacity == 0)             return BP_ERR_NULL_ARG;
    if (spec->path_size == 0 || spec->path_size > TXRX_MAX_PATH) return BP_ERR_PARAM_RANGE;
    if (out_resp_size) *out_resp_size = 0;
    msg_ctx_t ctx = {
        .spec = spec, .req = req, .req_size = req_size,
        .resp = resp, .resp_capacity = resp_capacity,
        .out_resp_size = out_resp_size,
    };
    bp_call_spec_t spec_call = {
        .fn_name      = "OCXcip_TxRxMsg",
        .payload_size = TXRXMSG_PAYLOAD,
        .fill_payload = msg_fill,
        .read_reply   = msg_read,
        .timeout_ms   = 30000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec_call);
}

/* ---------- TxRxCloseConn ---------- */
typedef struct { const bp_conn_spec_t *spec; } close_ctx_t;

static void close_fill(uint8_t *slot, void *user) {
    close_ctx_t *c = user;
    bp_st_u16(slot + TXRX_APP_HANDLE_OFF,  c->spec->app_handle);
    bp_st_u32(slot + TXRX_OPTIONS_OFF,     c->spec->options);
    memcpy(slot + TXRX_PATH_OFF,           c->spec->encoded_path, c->spec->path_size);
    bp_st_u16(slot + TXRX_PATH_SIZE_OFF,   c->spec->path_size);
}

int bp_client_txrx_close(bp_client_t *cl, const bp_conn_spec_t *spec) {
    if (!cl || !spec || !spec->encoded_path) return BP_ERR_NULL_ARG;
    if (spec->path_size == 0 || spec->path_size > TXRX_MAX_PATH) return BP_ERR_PARAM_RANGE;
    close_ctx_t ctx = { .spec = spec };
    bp_call_spec_t spec_call = {
        .fn_name      = "OCXcip_TxRxCloseConn",
        .payload_size = TXRX_OPENCLOSE_PAYLOAD,
        .fill_payload = close_fill,
        .read_reply   = NULL,
        .timeout_ms   = 5000,
        .user         = &ctx,
    };
    return bp_client_call(cl, &spec_call);
}
