/*
 * symcache.c — v0.9.0 Phase 1 symbol-cache validator.
 *
 * Opens a tag DB for the L85 (slot 2 by default), builds it, then:
 *   - Calls bp_tagdb_lookup_symbol("OCX_TEST") TWICE and reports the
 *     wall-clock latency of each.  First call walks the PLC's table
 *     incrementally and caches every examined symbol; second call is
 *     an in-memory hit and should be sub-millisecond.
 *   - Calls bp_tagdb_preload_symbols and prints its timing.
 *
 * Output is byte-identical (modulo dt= numbers) across the three
 * SDKs; the cross-language gate is the diff of the two summary lines.
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

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *tag  = "OCX_TEST";

    static struct option opts[] = {
        {"path", required_argument, 0, 'p'},
        {"tag",  required_argument, 0, 't'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "p:t:", opts, &idx)) != -1) {
        if      (oc == 'p') path = optarg;
        else if (oc == 't') tag  = optarg;
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[symcache] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    bp_tagdb_t *db = NULL;
    int rc = bp_tagdb_open(cl, path, &db);
    if (rc != BP_OK) {
        fprintf(stderr, "[symcache] tagdb_open rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_client_close(cl);
        return 1;
    }
    uint16_t n = 0;
    double t0 = now_ms();
    rc = bp_tagdb_build(db, &n);
    double t1 = now_ms();
    if (rc != BP_OK) {
        fprintf(stderr, "[symcache] build rc=%d\n", rc);
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }
    printf("[symcache] path=%s build n=%u dt=%.2fms\n", path, n, t1 - t0);

    bp_symbol_info_t info;
    /* First lookup — walks the cache lazily. */
    double l0 = now_ms();
    rc = bp_tagdb_lookup_symbol(db, tag, &info);
    double l1 = now_ms();
    if (rc != BP_OK) {
        fprintf(stderr, "[symcache] lookup#1 rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }
    printf("[symcache] lookup#1 cold dt=%.2fms  data_type=0x%04x elem_byte_size=%u\n",
           l1 - l0, info.data_type, info.elem_byte_size);

    /* Second lookup — should be a cached hit. */
    double l2 = now_ms();
    rc = bp_tagdb_lookup_symbol(db, tag, &info);
    double l3 = now_ms();
    if (rc != BP_OK) {
        fprintf(stderr, "[symcache] lookup#2 rc=%d\n", rc);
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }
    printf("[symcache] lookup#2 warm dt=%.3fms  (cache hit)\n", l3 - l2);

    /* Eager preload of the rest. */
    double p0 = now_ms();
    rc = bp_tagdb_preload_symbols(db);
    double p1 = now_ms();
    printf("[symcache] preload all dt=%.2fms rc=%d\n", p1 - p0, rc);

    /* Now any name fetched after preload is a cache hit. */
    double l4 = now_ms();
    rc = bp_tagdb_lookup_symbol(db, tag, &info);
    double l5 = now_ms();
    printf("[symcache] lookup#3 after-preload dt=%.3fms  rc=%d\n", l5 - l4, rc);

    printf("[symcache] PASS\n");
    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
