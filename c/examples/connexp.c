/*
 * connexp.c — v0.7.0 Phase 1 EXPERIMENT (throwaway).
 *
 * Drives Forward_Open + Identity + Forward_Close as three separate
 * bp_client_message_send calls (mbox 0x200 / UCMM transport).  Answers
 * the open question from docs/protocol.md:
 *
 *   After a Large Forward Open succeeds, can subsequent CIP requests
 *   ride the same UCMM transport — and does Forward_Close still match
 *   PLC-side connection state?
 *
 * Sibling daemon historianupdate empirically reports yes
 * (apex2_cip_connection.c:1346-1352 "ASIC treats all MBOX_LOOPBACK
 * sends identically"), but that's the chip's view, not necessarily
 * libocxbpeng.so.2.3 → OCXcip_MessageSend on cm1756.  This tool is
 * the cross-check.  Result decides v0.7.0 scope.
 *
 * Usage: connexp [--slot 2]
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "bpclient.h"

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

static void put_u16le(uint8_t *p, uint16_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF; }
static void put_u32le(uint8_t *p, uint32_t v) { p[0] = v & 0xFF; p[1] = (v >> 8) & 0xFF;
                                                p[2] = (v >> 16) & 0xFF; p[3] = (v >> 24) & 0xFF; }

/* Large Forward Open (CIP service 0x5B), wire format mirrored from
 * historianupdate apex2_cip_connection.c::build_forward_open (lines 682-820).
 * conn_serial / orig_serial are caller-supplied (random per run). */
static int build_lfo(uint8_t *buf, uint16_t conn_serial, uint32_t orig_serial,
                     uint16_t vendor_id, uint32_t to_id_hint, uint32_t ot_id_hint,
                     uint32_t rpi_us, uint16_t ot_size_bytes) {
    int off = 0;
    buf[off++] = 0x5B;           /* Large Forward Open */
    buf[off++] = 0x02;           /* path size words */
    buf[off++] = 0x20; buf[off++] = 0x06; /* class 6 (CM) */
    buf[off++] = 0x24; buf[off++] = 0x01; /* instance 1 */

    buf[off++] = 0x05;           /* priority/tick */
    buf[off++] = 0xF7;           /* timeout ticks */

    put_u32le(buf + off, ot_id_hint); off += 4;   /* O->T conn ID hint */
    put_u32le(buf + off, to_id_hint); off += 4;   /* T->O conn ID hint */

    put_u16le(buf + off, conn_serial); off += 2;  /* connection serial */
    put_u16le(buf + off, vendor_id);   off += 2;  /* vendor ID */
    put_u32le(buf + off, orig_serial); off += 4;  /* originator serial */

    put_u32le(buf + off, 0x00000003); off += 4;   /* timeout multiplier (3 → RPI*4) */

    put_u32le(buf + off, rpi_us); off += 4;       /* O->T RPI µs */
    /* O->T connection params (32-bit): bit30=P2P, bit25=variable, size in low 16 */
    put_u32le(buf + off, 0x42000000U | (uint32_t)ot_size_bytes); off += 4;

    put_u32le(buf + off, rpi_us); off += 4;       /* T->O RPI µs */
    put_u32le(buf + off, 0x42000000U | (uint32_t)ot_size_bytes); off += 4;

    buf[off++] = 0xA3;           /* transport trigger: Class 3, server */

    buf[off++] = 0x02;           /* conn path size words */
    buf[off++] = 0x20; buf[off++] = 0x02; /* class 2 (Msg Router) */
    buf[off++] = 0x24; buf[off++] = 0x01; /* instance 1 */

    return off;
}

/* Forward_Close (CIP service 0x4E), mirrored from
 * historianupdate apex2_cip_connection.c::build_forward_close (lines 1244-1283). */
static int build_fc(uint8_t *buf, uint16_t conn_serial, uint16_t vendor_id,
                    uint32_t orig_serial) {
    int off = 0;
    buf[off++] = 0x4E;
    buf[off++] = 0x02;
    buf[off++] = 0x20; buf[off++] = 0x06;
    buf[off++] = 0x24; buf[off++] = 0x01;

    buf[off++] = 0x0A;
    buf[off++] = 0x0E;

    put_u16le(buf + off, conn_serial); off += 2;
    put_u16le(buf + off, vendor_id);   off += 2;
    put_u32le(buf + off, orig_serial); off += 4;

    buf[off++] = 0x02;
    buf[off++] = 0x00;
    buf[off++] = 0x20; buf[off++] = 0x02;
    buf[off++] = 0x24; buf[off++] = 0x01;
    return off;
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int send_one(bp_client_t *cl, const char *label, uint8_t slot,
                    const uint8_t *req, uint16_t req_len,
                    uint8_t *resp, uint16_t resp_cap, uint16_t *out_len) {
    bp_message_t msg = {
        .slot          = slot,
        .cip_request   = req,
        .req_size      = req_len,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = resp_cap,
    };
    printf("[%s] req (%u bytes):\n", label, req_len);
    hexdump(req, req_len);

    double t0 = now_ms();
    int rc = bp_client_message_send(cl, &msg);
    double dt = now_ms() - t0;

    printf("[%s] rc=%d (%s)  resp_len=%u  status=0x%08x  dt=%.2fms\n",
           label, rc, bp_strerror(rc), msg.resp_len, msg.status, dt);
    if (msg.resp_len) {
        printf("[%s] resp:\n", label);
        hexdump(resp, msg.resp_len);
    }
    *out_len = msg.resp_len;
    return rc;
}

int main(int argc, char *argv[]) {
    int slot = 2;
    static struct option opts[] = {
        {"slot", required_argument, 0, 's'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "s:", opts, &idx)) != -1) {
        if (oc == 's') slot = (int)strtol(optarg, NULL, 0);
    }

    /* Randomize serials per run so the PLC doesn't trip on a duplicate
     * connection serial number from a prior aborted run.  Mirror the
     * sibling's strategy (orig_serial = pid ^ (time << 16)). */
    srand((unsigned)(time(NULL) ^ getpid()));
    uint16_t conn_serial = (uint16_t)(rand() & 0xFFFF);
    if (conn_serial == 0) conn_serial = 0xBEEF;
    uint32_t orig_serial = (uint32_t)getpid() ^ ((uint32_t)time(NULL) << 16);
    uint16_t vendor_id   = 0x0001;
    uint32_t rpi_us      = 10000000U;  /* 10s — matches sibling FO_OT_RPI_US */
    uint16_t ot_size     = 4000;       /* matches sibling FO_REQUEST_OT_SIZE */

    /* Originator-chosen conn ID hints.  Per sibling RE: high bit of T->O
     * is required by L73 firmware v21 to enable CBS transport — we set
     * it for parity even though we won't be using CBS here. */
    uint32_t ot_id_hint = 0x80010000U;
    uint32_t to_id_hint = 0x80000001U;

    printf("=== connexp v0.7.0 Phase 1 experiment ===\n");
    printf("slot=%d  conn_serial=0x%04x  orig_serial=0x%08x  vendor=0x%04x\n",
           slot, conn_serial, orig_serial, vendor_id);
    printf("ot_size=%u  rpi_us=%u  ot_hint=0x%08x  to_hint=0x%08x\n\n",
           ot_size, rpi_us, ot_id_hint, to_id_hint);

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) { fprintf(stderr, "open failed\n"); return 2; }
    bp_client_open_session(cl, NULL);

    /* ── Step 1: Large Forward Open ─────────────────────────────── */
    uint8_t lfo[64];
    int lfo_len = build_lfo(lfo, conn_serial, orig_serial, vendor_id,
                            to_id_hint, ot_id_hint, rpi_us, ot_size);

    uint8_t resp[512];
    uint16_t resp_len = 0;

    int rc = send_one(cl, "LFO", (uint8_t)slot, lfo, (uint16_t)lfo_len,
                      resp, sizeof(resp), &resp_len);
    if (rc != BP_OK) {
        printf("\n=== RESULT: LFO transport failed — abort.\n");
        bp_client_close(cl);
        return 1;
    }
    if (resp_len < 4 || (resp[0] != 0xDB && resp[0] != 0xD4) || resp[2] != 0x00) {
        uint8_t st = resp_len >= 3 ? resp[2] : 0xFF;
        uint16_t ext = (resp_len >= 6 && resp[3]) ? (resp[4] | (resp[5] << 8)) : 0;
        printf("\n=== RESULT: LFO CIP-level failure (svc=0x%02x status=0x%02x ext=0x%04x)\n",
               resp_len ? resp[0] : 0, st, ext);
        bp_client_close(cl);
        return 1;
    }
    uint32_t ot_conn_id = resp[4] | (resp[5] << 8) | (resp[6] << 16) | (resp[7] << 24);
    uint32_t to_conn_id = resp[8] | (resp[9] << 8) | (resp[10] << 16) | (resp[11] << 24);
    printf("[LFO] LFO success: O->T conn_id=0x%08x  T->O conn_id=0x%08x\n\n",
           ot_conn_id, to_conn_id);

    /* ── Step 2: Identity Get_Attributes_All via UCMM transport ──── */
    static const uint8_t identity_req[] = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };
    rc = send_one(cl, "IDENT", (uint8_t)slot, identity_req, sizeof(identity_req),
                  resp, sizeof(resp), &resp_len);
    int ident_ok = 0;
    if (rc == BP_OK && resp_len >= 4 && resp[0] == 0x81 && resp[2] == 0x00) {
        ident_ok = 1;
        if (resp_len >= 14) {
            const uint8_t *body = resp + 4 + resp[3] * 2;
            uint16_t vid  = body[0] | (body[1] << 8);
            uint16_t dev  = body[2] | (body[3] << 8);
            uint16_t pc   = body[4] | (body[5] << 8);
            printf("[IDENT] Identity OK: vendor=0x%04x  devtype=0x%04x  product=0x%04x\n",
                   vid, dev, pc);
        }
    }
    printf("\n");

    /* ── Step 3: Forward_Close ──────────────────────────────────── */
    uint8_t fc[32];
    int fc_len = build_fc(fc, conn_serial, vendor_id, orig_serial);
    rc = send_one(cl, "FC", (uint8_t)slot, fc, (uint16_t)fc_len,
                  resp, sizeof(resp), &resp_len);
    int fc_ok = (rc == BP_OK && resp_len >= 4 && resp[0] == 0xCE && resp[2] == 0x00);
    if (fc_ok) {
        printf("[FC] Forward_Close success — PLC accepted our conn_serial=0x%04x\n",
               conn_serial);
    }
    printf("\n");

    /* ── Summary ─────────────────────────────────────────────────── */
    printf("=== SUMMARY ===\n");
    printf("  LFO    : OK (ot_id=0x%08x  to_id=0x%08x)\n", ot_conn_id, to_conn_id);
    printf("  IDENT  : %s\n", ident_ok ? "OK"   : "FAIL");
    printf("  FC     : %s\n", fc_ok    ? "OK"   : "FAIL");
    printf("Conclusion: %s\n",
           (ident_ok && fc_ok)
             ? "v0.7.0 small-buffer TxRx via bp_client_message_send is VIABLE."
             : "v0.7.0 design needs revision — see per-step output above.");

    bp_client_close(cl);
    return (ident_ok && fc_ok) ? 0 : 1;
}
