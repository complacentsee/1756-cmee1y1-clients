/*
 * accessdbtest.c — v0.10.4 cross-validation of AccessTagDataDb
 * against AccessTagData.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sequence:
 *   1. open IPC / session / tagdb on --path (default P:1,S:2)
 *   2. Build symbol table (needed so the engine has a primed tag DB
 *      to satisfy the handle-based call against)
 *   3. Read --tag (default OCX_TEST) once via bp_tagdb_access      [OLD]
 *   4. Read --tag once via bp_tagdb_access_db                      [NEW]
 *   5. Assert both reads returned the same value byte-for-byte
 *   6. Loop --iters times (default 100): alternate OLD/NEW single-tag
 *      reads; track median + p99 wall-clock per call for each path.
 *   7. If --batch N > 1: same loop but each call contains N reads of
 *      --tag in one batch (1..16).  Measures whether the predicted
 *      per-call engine-side savings compound at larger batch sizes.
 *   8. Optional --write: round-trip 0xDEADBEEF via NEW, read back
 *      via OLD, then restore via NEW, read back via OLD.  Each step
 *      must match the expected value.
 *
 * Exit 0 on PASS, non-zero on FAIL.
 */
#include <getopt.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bpclient.h"

static double elapsed_ms(struct timespec a, struct timespec b) {
    return (b.tv_sec - a.tv_sec) * 1000.0
         + (b.tv_nsec - a.tv_nsec) / 1000000.0;
}

static int cmp_double(const void *a, const void *b) {
    double da = *(const double *)a, db = *(const double *)b;
    return (da > db) - (da < db);
}

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--path P:1,S:2] [--tag OCX_TEST]\n"
        "         [--iters 100] [--batch N] [--write] [--help]\n",
        argv0);
}

/* Run `iters` batches of `batch_n` reads of `tag` through `use_db_path`.
 * Buffers (`vals`, `cips`) must hold batch_n entries; they're reused
 * across iterations.  Stores per-call wall-clock in samples[0..iters-1].
 * Returns the number of failed iterations (rc!=0, any per-tag cip!=0, or
 * any value mismatch with `expect`). */
static int run_batch(bp_tagdb_t *db, const char *tag, int32_t expect,
                     int use_db_path, int batch_n, int iters,
                     int32_t *vals, uint32_t *cips,
                     double *samples,
                     bp_tag_request_t *reqs) {
    int failures = 0;
    for (int i = 0; i < iters; i++) {
        for (int k = 0; k < batch_n; k++) {
            vals[k] = 0;
            cips[k] = 0;
            reqs[k].tag_name = tag;
            reqs[k].data_type = BP_TYPE_DINT;
            reqs[k].elem_byte_size = 4;
            reqs[k].action = BP_ACTION_READ;
            reqs[k].elem_count = 1;
            reqs[k].data = &vals[k];
            reqs[k].result = 0;
        }
        struct timespec a, b;
        clock_gettime(CLOCK_MONOTONIC, &a);
        int rc = use_db_path ? bp_tagdb_access_db(db, reqs, (size_t)batch_n)
                              : bp_tagdb_access   (db, reqs, (size_t)batch_n);
        clock_gettime(CLOCK_MONOTONIC, &b);
        samples[i] = elapsed_ms(a, b);
        if (rc != BP_OK) { failures++; continue; }
        for (int k = 0; k < batch_n; k++) {
            cips[k] = reqs[k].result;
            if (reqs[k].result != 0 || vals[k] != expect) {
                failures++;
                break;
            }
        }
    }
    return failures;
}

/* Read a DINT via either access path; returns CIP general status in
 * *out_cip (0 on success), and the value in *out_value.  rc is the
 * slot-level engine code. */
static int read_dint(bp_tagdb_t *db, const char *tag,
                     int use_db_path,
                     int32_t *out_value, uint32_t *out_cip) {
    int32_t buf = 0;
    bp_tag_request_t r = {
        .tag_name = tag,
        .data_type = BP_TYPE_DINT,
        .elem_byte_size = 4,
        .action = BP_ACTION_READ,
        .elem_count = 1,
        .data = &buf,
        .result = 0,
    };
    int rc = use_db_path ? bp_tagdb_access_db(db, &r, 1)
                         : bp_tagdb_access   (db, &r, 1);
    *out_cip   = r.result;
    *out_value = buf;
    return rc;
}

static int write_dint(bp_tagdb_t *db, const char *tag,
                      int use_db_path,
                      int32_t value, uint32_t *out_cip) {
    bp_tag_request_t r = {
        .tag_name = tag,
        .data_type = BP_TYPE_DINT,
        .elem_byte_size = 4,
        .action = BP_ACTION_WRITE,
        .elem_count = 1,
        .data = &value,
        .result = 0,
    };
    int rc = use_db_path ? bp_tagdb_access_db(db, &r, 1)
                         : bp_tagdb_access   (db, &r, 1);
    *out_cip = r.result;
    return rc;
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *tag  = "OCX_TEST";
    int iters = 100;
    int batch_n = 1;
    int do_write = 0;

    static struct option opts[] = {
        {"path",  required_argument, 0, 'p'},
        {"tag",   required_argument, 0, 't'},
        {"iters", required_argument, 0, 'i'},
        {"batch", required_argument, 0, 'b'},
        {"write", no_argument,       0, 'w'},
        {"help",  no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:t:i:b:wh", opts, &idx)) != -1) {
        switch (c) {
        case 'p': path = optarg; break;
        case 't': tag = optarg; break;
        case 'i': iters = atoi(optarg); break;
        case 'b': batch_n = atoi(optarg); break;
        case 'w': do_write = 1; break;
        case 'h':
        default:  usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }
    if (iters < 1) iters = 1;
    if (iters > 10000) iters = 10000;
    if (batch_n < 1) batch_n = 1;
    if (batch_n > 16) batch_n = 16;   /* bp_tagdb_access cap */

    printf("[accessdbtest] path=%s tag=%s iters=%d batch=%d write=%s\n",
           path, tag, iters, batch_n, do_write ? "yes" : "no");

    bp_client_t *client = NULL;
    int rc = bp_client_open(&client);
    if (rc != BP_OK) {
        fprintf(stderr, "[accessdbtest] FATAL Open: %s (%d)\n", bp_strerror(rc), rc);
        return 2;
    }
    uint32_t session = 0;
    rc = bp_client_open_session(client, &session);
    if (rc != BP_OK) {
        fprintf(stderr, "[accessdbtest] FATAL OpenSession: %s (%d)\n",
                bp_strerror(rc), rc);
        bp_client_close(client);
        return 2;
    }
    bp_tagdb_t *db = NULL;
    rc = bp_tagdb_open(client, path, &db);
    if (rc != BP_OK) {
        fprintf(stderr, "[accessdbtest] FATAL OpenTagDB(%s): %s (%d)\n",
                path, bp_strerror(rc), rc);
        bp_client_close(client);
        return 2;
    }
    uint16_t n_symbols = 0;
    rc = bp_tagdb_build(db, &n_symbols);
    if (rc != BP_OK) {
        fprintf(stderr, "[accessdbtest] FATAL Build: %s (%d)\n",
                bp_strerror(rc), rc);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 2;
    }
    printf("[accessdbtest] Build ok  symbols=%u\n", (unsigned)n_symbols);

    /* Single-shot OLD vs NEW correctness check */
    int32_t v_old = 0, v_new = 0;
    uint32_t cip_old = 0, cip_new = 0;
    rc = read_dint(db, tag, 0, &v_old, &cip_old);
    if (rc != BP_OK || cip_old != 0) {
        fprintf(stderr,
            "[accessdbtest] FATAL Read OLD path: rc=%d (%s) cip=0x%08" PRIx32 "\n",
            rc, bp_strerror(rc), cip_old);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 2;
    }
    rc = read_dint(db, tag, 1, &v_new, &cip_new);
    if (rc != BP_OK || cip_new != 0) {
        fprintf(stderr,
            "[accessdbtest] FATAL Read NEW path: rc=%d (%s) cip=0x%08" PRIx32 "\n",
            rc, bp_strerror(rc), cip_new);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 2;
    }
    if (v_old != v_new) {
        fprintf(stderr,
            "[accessdbtest] FAIL  OLD=%" PRId32 " (0x%08" PRIx32 ")"
            " NEW=%" PRId32 " (0x%08" PRIx32 ")\n",
            v_old, (uint32_t)v_old, v_new, (uint32_t)v_new);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 1;
    }
    printf("[accessdbtest] correctness: OLD == NEW = %" PRId32
           " (0x%08" PRIx32 ")\n", v_old, (uint32_t)v_old);

    /* Latency comparison: iters batches through each path */
    double *t_old = calloc((size_t)iters, sizeof(double));
    double *t_new = calloc((size_t)iters, sizeof(double));
    int32_t *vals = calloc((size_t)batch_n, sizeof(int32_t));
    uint32_t *cips = calloc((size_t)batch_n, sizeof(uint32_t));
    bp_tag_request_t *reqs = calloc((size_t)batch_n, sizeof(*reqs));
    if (!t_old || !t_new || !vals || !cips || !reqs) {
        fprintf(stderr, "[accessdbtest] FATAL alloc\n");
        free(t_old); free(t_new); free(vals); free(cips); free(reqs);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 2;
    }
    int fail_old = run_batch(db, tag, v_old, 0, batch_n, iters,
                              vals, cips, t_old, reqs);
    int fail_new = run_batch(db, tag, v_old, 1, batch_n, iters,
                              vals, cips, t_new, reqs);
    qsort(t_old, (size_t)iters, sizeof(double), cmp_double);
    qsort(t_new, (size_t)iters, sizeof(double), cmp_double);
    int p50_idx = iters / 2;
    int p99_idx = (iters * 99) / 100;
    if (p99_idx >= iters) p99_idx = iters - 1;
    long tags_per_batch = batch_n;
    double per_tag_old = t_old[p50_idx] / (double)tags_per_batch;
    double per_tag_new = t_new[p50_idx] / (double)tags_per_batch;
    double speedup_pct =
        per_tag_old > 0.0
            ? (per_tag_old - per_tag_new) * 100.0 / per_tag_old
            : 0.0;
    printf("[accessdbtest] latency over %d batches of %d reads each path:\n",
           iters, batch_n);
    printf("  OLD path  median=%6.3fms  p99=%6.3fms  min=%6.3fms  max=%6.3fms"
           "  per-tag=%6.3fms\n",
           t_old[p50_idx], t_old[p99_idx], t_old[0], t_old[iters - 1],
           per_tag_old);
    printf("  NEW path  median=%6.3fms  p99=%6.3fms  min=%6.3fms  max=%6.3fms"
           "  per-tag=%6.3fms\n",
           t_new[p50_idx], t_new[p99_idx], t_new[0], t_new[iters - 1],
           per_tag_new);
    printf("  NEW vs OLD per-tag delta: %+.2f%% (positive = NEW faster)\n",
           speedup_pct);
    free(t_old);
    free(t_new);
    free(vals);
    free(cips);
    free(reqs);
    if (fail_old > 0 || fail_new > 0) {
        printf("[accessdbtest] FAIL  OLD fails=%d  NEW fails=%d  (of %d batches each)\n",
               fail_old, fail_new, iters);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 1;
    }

    int passed_write = 1, passed_restore = 1;
    if (do_write) {
        const int32_t sentinel = (int32_t)0xDEADBEEFu;
        uint32_t cip = 0;
        int32_t v_back = 0;

        /* Write sentinel via NEW path */
        rc = write_dint(db, tag, 1, sentinel, &cip);
        if (rc != BP_OK || cip != 0) {
            fprintf(stderr,
                "[accessdbtest] FATAL Write NEW: rc=%d (%s) cip=0x%08" PRIx32 "\n",
                rc, bp_strerror(rc), cip);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        /* Read back via OLD path */
        rc = read_dint(db, tag, 0, &v_back, &cip);
        if (rc != BP_OK || cip != 0) {
            fprintf(stderr,
                "[accessdbtest] FATAL Readback OLD: rc=%d (%s) cip=0x%08" PRIx32 "\n",
                rc, bp_strerror(rc), cip);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        passed_write = (v_back == sentinel);
        printf("[accessdbtest] NEW-write -> OLD-readback = 0x%08" PRIx32
               "  %s\n", (uint32_t)v_back,
               passed_write ? "<-- OK" : "<-- WRITE DID NOT TAKE");

        /* Restore via NEW path */
        rc = write_dint(db, tag, 1, v_old, &cip);
        if (rc != BP_OK || cip != 0) {
            fprintf(stderr,
                "[accessdbtest] FATAL Restore NEW: rc=%d (%s) cip=0x%08" PRIx32 "\n",
                rc, bp_strerror(rc), cip);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        /* Confirm restore via OLD path */
        rc = read_dint(db, tag, 0, &v_back, &cip);
        if (rc != BP_OK || cip != 0) {
            fprintf(stderr,
                "[accessdbtest] FATAL Confirm OLD: rc=%d (%s) cip=0x%08" PRIx32 "\n",
                rc, bp_strerror(rc), cip);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        passed_restore = (v_back == v_old);
        printf("[accessdbtest] NEW-restore -> OLD-readback = %" PRId32
               "  %s\n", v_back,
               passed_restore ? "<-- OK" : "<-- RESTORE DID NOT TAKE");
    }

    bp_tagdb_close(db);
    bp_client_close(client);
    if (passed_write && passed_restore) {
        printf("\n[accessdbtest] PASS\n");
        return 0;
    }
    printf("\n[accessdbtest] FAIL\n");
    return 1;
}
