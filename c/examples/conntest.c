/*
 * conntest.c — v0.7.0 connected-messaging round-trip validator.
 *
 * Opens a class-3 connection to a slot (default 2 = L85), sends N
 * (default 10) Identity Get_Attributes_All requests over the
 * connection, closes, prints a PASS/FAIL summary.  Output format
 * is intentionally byte-identical (modulo dt= timing values) to
 * the Go + Python conntest CLIs — the cross-language gate is the
 * three diffs.
 *
 * --bench flag adds a UCMM-vs-class3 micro-benchmark: 100 Identity
 * round-trips each, prints median + p95 per transport.  Useful as
 * a v0.8 regression baseline once mbox 0x204 lands.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bpclient.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    if (da < db) return -1;
    if (da > db) return  1;
    return 0;
}

static double percentile(double *xs, int n, double p) {
    /* In-place sort.  p in [0..1]. */
    if (n <= 0) return 0.0;
    qsort(xs, n, sizeof(double), cmp_double);
    double idx = p * (n - 1);
    int lo = (int)idx;
    int hi = lo + 1 >= n ? n - 1 : lo + 1;
    double frac = idx - lo;
    return xs[lo] * (1.0 - frac) + xs[hi] * frac;
}

/* Identity Get_Attributes_All on class 1 instance 1. */
static const uint8_t identity_req[] = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };

/* Returns 1 on a clean Identity reply (svc 0x81, gs 0x00). */
static int validate_identity_reply(const uint8_t *resp, uint16_t len) {
    return (len >= 4 && resp[0] == 0x81 && resp[2] == 0x00);
}

static void print_identity(const uint8_t *resp, uint16_t len) {
    if (len < 14) {
        printf("[conntest] identity: (body too short)\n");
        return;
    }
    const uint8_t *body = resp + 4 + resp[3] * 2;
    uint16_t blen = len - (uint16_t)(4 + resp[3] * 2);
    if (blen < 14) {
        printf("[conntest] identity: (body too short)\n");
        return;
    }
    uint16_t vendor = body[0] | (body[1] << 8);
    uint16_t dev    = body[2] | (body[3] << 8);
    uint16_t prod   = body[4] | (body[5] << 8);
    uint8_t  major  = body[6];
    uint8_t  minor  = body[7];
    uint8_t  name_len = blen > 14 ? body[14] : 0;
    if (15u + name_len > blen) name_len = (uint8_t)(blen - 15);
    printf("[conntest] identity: Vendor=0x%04x DevType=0x%04x Product=0x%04x "
           "fw=%u.%u Name='%.*s'\n",
           vendor, dev, prod, major, minor, (int)name_len, body + 15);
}

static int run_bench(bp_client_t *cl, int slot, int n) {
    printf("[conntest] benchmark: %d Identity round-trips per transport, slot=%d\n",
           n, slot);

    double *ucmm_dt   = calloc((size_t)n, sizeof(double));
    double *class3_dt = calloc((size_t)n, sizeof(double));
    if (!ucmm_dt || !class3_dt) {
        free(ucmm_dt); free(class3_dt);
        fprintf(stderr, "[conntest] bench: alloc failed\n");
        return 1;
    }

    /* UCMM loop. */
    for (int i = 0; i < n; i++) {
        uint8_t resp[256];
        bp_message_t msg = {
            .slot = (uint8_t)slot,
            .cip_request = identity_req,
            .req_size = sizeof(identity_req),
            .timeout_ms = 5000,
            .resp_data = resp,
            .resp_capacity = sizeof(resp),
        };
        double t0 = now_ms();
        int rc = bp_client_message_send(cl, &msg);
        ucmm_dt[i] = now_ms() - t0;
        if (rc != BP_OK || !validate_identity_reply(resp, msg.resp_len)) {
            fprintf(stderr, "[conntest] UCMM bench: req[%d] failed (rc=%d)\n", i, rc);
            free(ucmm_dt); free(class3_dt);
            return 1;
        }
    }

    /* Class-3 loop — open once, msg N times, close once. */
    uint8_t epath[] = { 0x01, (uint8_t)slot };
    bp_conn_spec_t spec = {
        .app_handle   = 2,
        .options      = 0,
        .encoded_path = epath,
        .path_size    = 2,
        .conn_params  = 0,
    };
    int rc = bp_client_txrx_open(cl, &spec, NULL, NULL);
    if (rc != BP_OK) {
        fprintf(stderr, "[conntest] bench: txrx_open rc=%d (%s)\n", rc, bp_strerror(rc));
        free(ucmm_dt); free(class3_dt);
        return 1;
    }
    for (int i = 0; i < n; i++) {
        uint8_t resp[256];
        uint16_t got = 0;
        double t0 = now_ms();
        int mrc = bp_client_txrx_msg(cl, &spec, identity_req, sizeof(identity_req),
                                      resp, sizeof(resp), &got);
        class3_dt[i] = now_ms() - t0;
        if (mrc != BP_OK || !validate_identity_reply(resp, got)) {
            fprintf(stderr, "[conntest] Class3 bench: req[%d] failed (rc=%d)\n", i, mrc);
            (void)bp_client_txrx_close(cl, &spec);
            free(ucmm_dt); free(class3_dt);
            return 1;
        }
    }
    (void)bp_client_txrx_close(cl, &spec);

    double um = percentile(ucmm_dt,   n, 0.50);
    double up = percentile(ucmm_dt,   n, 0.95);
    double cm = percentile(class3_dt, n, 0.50);
    double cp = percentile(class3_dt, n, 0.95);
    printf("[conntest]   UCMM     median dt=%.2fms  p95 dt=%.2fms\n", um, up);
    printf("[conntest]   Class3   median dt=%.2fms  p95 dt=%.2fms\n", cm, cp);

    free(ucmm_dt); free(class3_dt);
    return 0;
}

int main(int argc, char *argv[]) {
    int slot = 2;
    int N = 10;
    int bench = 0;

    static struct option opts[] = {
        {"slot",  required_argument, 0, 's'},
        {"n",     required_argument, 0, 'n'},
        {"bench", no_argument,       0, 'b'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "s:n:b", opts, &idx)) != -1) {
        if      (oc == 's') slot  = (int)strtol(optarg, NULL, 0);
        else if (oc == 'n') N     = (int)strtol(optarg, NULL, 0);
        else if (oc == 'b') bench = 1;
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[conntest] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    uint8_t epath[] = { 0x01, (uint8_t)slot };
    bp_conn_spec_t spec = {
        .app_handle   = 1,
        .options      = 0,
        .encoded_path = epath,
        .path_size    = 2,
        .conn_params  = 0,
    };

    printf("[conntest] slot=%d N=%d app_handle=%u\n", slot, N, spec.app_handle);

    uint16_t conn_id = 0, conn_serial = 0;
    int rc = bp_client_txrx_open(cl, &spec, &conn_id, &conn_serial);
    if (rc != BP_OK) {
        fprintf(stderr, "[conntest] txrx_open rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_client_close(cl);
        return 1;
    }
    printf("[conntest] txrx_open  conn_id=0x%04x  serial=0x%04x\n", conn_id, conn_serial);

    double *dts = calloc((size_t)N, sizeof(double));
    if (!dts) {
        fprintf(stderr, "[conntest] alloc failed\n");
        (void)bp_client_txrx_close(cl, &spec);
        bp_client_close(cl);
        return 2;
    }

    uint8_t last_resp[256];
    uint16_t last_len = 0;
    int success = 0;
    for (int i = 0; i < N; i++) {
        uint8_t resp[256];
        uint16_t got = 0;
        double t0 = now_ms();
        int mrc = bp_client_txrx_msg(cl, &spec, identity_req, sizeof(identity_req),
                                      resp, sizeof(resp), &got);
        dts[i] = now_ms() - t0;
        uint8_t st = (got >= 3) ? resp[2] : 0xFF;
        uint16_t vendor = (got >= 6 && validate_identity_reply(resp, got))
                          ? (resp[4 + resp[3] * 2] | (resp[4 + resp[3] * 2 + 1] << 8))
                          : 0xFFFF;
        printf("[conntest] req[%d] dt=%.2fms status=0x%02x vendor=0x%04x\n",
               i, dts[i], st, vendor);
        if (mrc == BP_OK && validate_identity_reply(resp, got)) {
            success++;
            memcpy(last_resp, resp, got);
            last_len = got;
        }
    }

    if (last_len > 0) print_identity(last_resp, last_len);

    int crc = bp_client_txrx_close(cl, &spec);
    printf("[conntest] txrx_close %s\n", crc == BP_OK ? "ok" : "FAIL");

    double median = percentile(dts, N, 0.50);
    printf("[conntest] SUMMARY %d/%d success  median dt=%.2fms\n", success, N, median);
    int pass = (success == N) && (crc == BP_OK);
    printf("[conntest] %s\n", pass ? "PASS" : "FAIL");

    free(dts);

    int bench_rc = 0;
    if (bench) bench_rc = run_bench(cl, slot, 100);

    bp_client_close(cl);
    return (pass && bench_rc == 0) ? 0 : 1;
}
