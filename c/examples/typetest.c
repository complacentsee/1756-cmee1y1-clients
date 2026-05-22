/*
 * typetest.c — exercises every type helper against a configurable
 * mapping of test tags.  Each test: snapshot original, write probe
 * value, read back, assert, restore, read back, assert.
 *
 * Usage:
 *   typetest [--path P:1,S:1]
 *            [--bool TAG] [--dint TAG] [--real TAG]
 *            [--sint TAG] [--int TAG] [--lint TAG]
 *            [--usint TAG] [--uint TAG] [--udint TAG] [--ulint TAG]
 *            [--lreal TAG]
 *            [--string TAG]
 *            [--dint-array TAG --array-count N]
 *
 * Pass --skip-* to disable a section.  Exit 0 if all enabled sections
 * pass; 1 if any fail; 2 on infrastructure errors.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <getopt.h>
#include <inttypes.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static int g_pass = 0, g_fail = 0;

#define ASSERT_EQ(expected, actual, fmt)                                       \
    do {                                                                       \
        if ((expected) == (actual)) {                                          \
            printf("    ok\n");                                                \
            g_pass++;                                                          \
        } else {                                                               \
            printf("    FAIL: expected " fmt ", got " fmt "\n",                \
                   (expected), (actual));                                      \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define ASSERT_FEQ(expected, actual, eps)                                      \
    do {                                                                       \
        double _d = fabs((double)(expected) - (double)(actual));               \
        if (_d <= (eps)) {                                                     \
            printf("    ok\n");                                                \
            g_pass++;                                                          \
        } else {                                                               \
            printf("    FAIL: expected %.9g, got %.9g (delta %.9g)\n",         \
                   (double)(expected), (double)(actual), _d);                  \
            g_fail++;                                                          \
        }                                                                      \
    } while (0)

#define MAYBE_ERR(rc, what)                                                    \
    do {                                                                       \
        if ((rc) != BP_OK) {                                                   \
            printf("    FAIL: %s -> %s (%d)\n", (what), bp_strerror(rc), rc);  \
            g_fail++;                                                          \
            return;                                                            \
        }                                                                      \
    } while (0)

/* --- per-type tests --- */
#define TEST_SCALAR(NAME, CTYPE, FMT, PROBE)                                   \
static void test_##NAME(bp_tagdb_t *db, const char *tag) {                     \
    if (!tag) return;                                                          \
    printf("\n[" #NAME " scalar] tag=%s\n", tag);                              \
    CTYPE v0;                                                                  \
    MAYBE_ERR(bp_tagdb_read_##NAME(db, tag, &v0), "initial read");             \
    printf("  V0 = " FMT "\n", v0);                                            \
    CTYPE probe = (PROBE);                                                     \
    MAYBE_ERR(bp_tagdb_write_##NAME(db, tag, probe), "write probe");           \
    CTYPE v1;                                                                  \
    MAYBE_ERR(bp_tagdb_read_##NAME(db, tag, &v1), "post-write read");          \
    printf("  V1 = " FMT " (probe=" FMT ")\n", v1, probe);                     \
    ASSERT_EQ(probe, v1, FMT);                                                 \
    MAYBE_ERR(bp_tagdb_write_##NAME(db, tag, v0), "restore");                  \
    CTYPE v2;                                                                  \
    MAYBE_ERR(bp_tagdb_read_##NAME(db, tag, &v2), "post-restore read");        \
    printf("  V2 = " FMT " (restore=" FMT ")\n", v2, v0);                      \
    ASSERT_EQ(v0, v2, FMT);                                                    \
}

TEST_SCALAR(sint,  int8_t,   "%" PRId8,  (int8_t)42)
TEST_SCALAR(int,   int16_t,  "%" PRId16, (int16_t)12345)
TEST_SCALAR(dint,  int32_t,  "0x%08" PRIx32, (int32_t)0xDEADBEEF)
TEST_SCALAR(lint,  int64_t,  "0x%016" PRIx64, (int64_t)0x0123456789ABCDEFLL)
TEST_SCALAR(usint, uint8_t,  "%" PRIu8,  (uint8_t)0xAB)
TEST_SCALAR(uint,  uint16_t, "0x%04" PRIx16, (uint16_t)0xCAFE)
TEST_SCALAR(udint, uint32_t, "0x%08" PRIx32, (uint32_t)0xFEEDFACE)
TEST_SCALAR(ulint, uint64_t, "0x%016" PRIx64, (uint64_t)0xCAFEBABEDEADBEEFULL)

static void test_real(bp_tagdb_t *db, const char *tag) {
    if (!tag) return;
    printf("\n[real scalar] tag=%s\n", tag);
    float v0;  MAYBE_ERR(bp_tagdb_read_real(db, tag, &v0), "initial read");
    printf("  V0 = %.6g\n", v0);
    float probe = 3.14159f;
    MAYBE_ERR(bp_tagdb_write_real(db, tag, probe), "write");
    float v1;  MAYBE_ERR(bp_tagdb_read_real(db, tag, &v1), "post-write read");
    printf("  V1 = %.6g (probe=%.6g)\n", v1, probe);
    ASSERT_FEQ(probe, v1, 1e-5);
    MAYBE_ERR(bp_tagdb_write_real(db, tag, v0), "restore");
    float v2;  MAYBE_ERR(bp_tagdb_read_real(db, tag, &v2), "post-restore read");
    ASSERT_FEQ(v0, v2, 1e-5);
}

static void test_lreal(bp_tagdb_t *db, const char *tag) {
    if (!tag) return;
    printf("\n[lreal scalar] tag=%s\n", tag);
    double v0; MAYBE_ERR(bp_tagdb_read_lreal(db, tag, &v0), "initial read");
    printf("  V0 = %.15g\n", v0);
    double probe = 2.71828182845904523;
    MAYBE_ERR(bp_tagdb_write_lreal(db, tag, probe), "write");
    double v1; MAYBE_ERR(bp_tagdb_read_lreal(db, tag, &v1), "post-write read");
    printf("  V1 = %.15g (probe=%.15g)\n", v1, probe);
    ASSERT_FEQ(probe, v1, 1e-12);
    MAYBE_ERR(bp_tagdb_write_lreal(db, tag, v0), "restore");
    double v2; MAYBE_ERR(bp_tagdb_read_lreal(db, tag, &v2), "post-restore read");
    ASSERT_FEQ(v0, v2, 1e-12);
}

static void test_bool(bp_tagdb_t *db, const char *tag) {
    if (!tag) return;
    printf("\n[bool scalar] tag=%s\n", tag);
    int v0; MAYBE_ERR(bp_tagdb_read_bool(db, tag, &v0), "initial read");
    printf("  V0 = %d\n", v0);
    int probe = !v0;
    MAYBE_ERR(bp_tagdb_write_bool(db, tag, probe), "write");
    int v1; MAYBE_ERR(bp_tagdb_read_bool(db, tag, &v1), "post-write read");
    printf("  V1 = %d (probe=%d)\n", v1, probe);
    ASSERT_EQ(probe, v1, "%d");
    MAYBE_ERR(bp_tagdb_write_bool(db, tag, v0), "restore");
    int v2; MAYBE_ERR(bp_tagdb_read_bool(db, tag, &v2), "post-restore read");
    ASSERT_EQ(v0, v2, "%d");
}

static void test_string(bp_tagdb_t *db, const char *tag) {
    if (!tag) return;
    printf("\n[string] tag=%s\n", tag);
    char v0[100]; size_t v0_len = 0;
    int rc = bp_tagdb_read_string(db, tag, v0, sizeof(v0), &v0_len);
    if (rc != BP_OK) {
        printf("    FAIL: initial read -> %s (%d)\n", bp_strerror(rc), rc);
        g_fail++; return;
    }
    printf("  V0 = '%s' (len=%zu)\n", v0, v0_len);
    const char *probe = "hello from bpclient typetest";
    size_t probe_len = strlen(probe);
    MAYBE_ERR(bp_tagdb_write_string(db, tag, probe, probe_len), "write");
    char v1[100]; size_t v1_len = 0;
    MAYBE_ERR(bp_tagdb_read_string(db, tag, v1, sizeof(v1), &v1_len), "post-write read");
    printf("  V1 = '%s' (len=%zu)\n", v1, v1_len);
    int ok = (v1_len == probe_len) && (memcmp(v1, probe, probe_len) == 0);
    if (ok) { printf("    ok\n"); g_pass++; }
    else    { printf("    FAIL: string mismatch\n"); g_fail++; }
    MAYBE_ERR(bp_tagdb_write_string(db, tag, v0, v0_len), "restore");
}

static void test_bool_array(bp_tagdb_t *db, const char *tag, int count) {
    if (!tag || count <= 0) return;
    printf("\n[bool array] tag=%s count=%d\n", tag, count);
    uint8_t *v0 = calloc(count, 1);
    uint8_t *probe = calloc(count, 1);
    uint8_t *v1 = calloc(count, 1);
    if (!v0 || !probe || !v1) { free(v0); free(probe); free(v1); g_fail++; return; }

    int rc = bp_tagdb_read_bool_array(db, tag, v0, (uint16_t)count);
    if (rc != BP_OK) {
        printf("    FAIL: initial read -> %s (%d)\n", bp_strerror(rc), rc);
        free(v0); free(probe); free(v1); g_fail++; return;
    }
    printf("  V0[0..%d] =", count > 16 ? 15 : count - 1);
    for (int i = 0; i < count && i < 16; i++) printf(" %d", v0[i]);
    printf("%s\n", count > 16 ? " ..." : "");

    /* Probe pattern: alternating 1,0,1,0,...,1 */
    for (int i = 0; i < count; i++) probe[i] = (i & 1) ? 0 : 1;
    rc = bp_tagdb_write_bool_array(db, tag, probe, (uint16_t)count);
    if (rc != BP_OK) {
        printf("    FAIL: write -> %s (%d)\n", bp_strerror(rc), rc);
        free(v0); free(probe); free(v1); g_fail++; return;
    }
    rc = bp_tagdb_read_bool_array(db, tag, v1, (uint16_t)count);
    if (rc != BP_OK) {
        printf("    FAIL: readback -> %s (%d)\n", bp_strerror(rc), rc);
        free(v0); free(probe); free(v1); g_fail++; return;
    }
    int all_match = 1;
    for (int i = 0; i < count; i++) {
        if (v1[i] != probe[i]) {
            printf("    MISMATCH at bit[%d]: expected %d, got %d\n", i, probe[i], v1[i]);
            all_match = 0;
        }
    }
    if (all_match) { printf("    ok (all %d bits match)\n", count); g_pass++; }
    else           { g_fail++; }
    bp_tagdb_write_bool_array(db, tag, v0, (uint16_t)count);   /* restore */
    free(v0); free(probe); free(v1);
}

static void test_dint_2d(bp_tagdb_t *db, const char *tag, int dim0, int dim1) {
    if (!tag || dim0 <= 0 || dim1 <= 0) return;
    int total = dim0 * dim1;
    printf("\n[dint 2-D] tag=%s dims=%d,%d (total=%d)\n", tag, dim0, dim1, total);
    int32_t *v0 = calloc(total, sizeof(int32_t));
    int32_t *probe = calloc(total, sizeof(int32_t));
    int32_t *v1 = calloc(total, sizeof(int32_t));
    if (!v0 || !probe || !v1) { free(v0); free(probe); free(v1); g_fail++; return; }

    /* Build "tag[0,0]" for the batched read */
    char zero_idx[256];
    snprintf(zero_idx, sizeof(zero_idx), "%s[0,0]", tag);

    MAYBE_ERR(bp_tagdb_read_dint_array(db, zero_idx, v0, (uint16_t)total), "initial read");
    printf("  V0 (first 6 = first 2 rows): %d %d %d %d %d %d\n",
           v0[0], v0[1], v0[2], v0[3], v0[4], v0[5]);

    /* Probe: row r col c -> 1000*r + c */
    for (int r = 0; r < dim0; r++)
        for (int c = 0; c < dim1; c++)
            probe[r * dim1 + c] = 1000 * r + c;
    MAYBE_ERR(bp_tagdb_write_dint_array(db, zero_idx, probe, (uint16_t)total), "write");

    MAYBE_ERR(bp_tagdb_read_dint_array(db, zero_idx, v1, (uint16_t)total), "post-write read");
    int all_match = 1;
    for (int i = 0; i < total; i++) if (v1[i] != probe[i]) all_match = 0;
    if (all_match) { printf("    ok (batched readback row-major matches all %d)\n", total); g_pass++; }
    else           { printf("    FAIL: batched readback mismatch\n"); g_fail++; }

    /* Element-by-index spot check: [dim0/2, dim1/2] */
    int mr = dim0 / 2, mc = dim1 / 2;
    char idx[256];
    snprintf(idx, sizeof(idx), "%s[%d,%d]", tag, mr, mc);
    int32_t spot;
    MAYBE_ERR(bp_tagdb_read_dint(db, idx, &spot), "indexed read");
    int32_t expect = 1000 * mr + mc;
    printf("  %s = %d (expect %d) %s\n", idx, spot, expect,
           spot == expect ? "ok" : "FAIL");
    if (spot == expect) g_pass++; else g_fail++;

    /* Restore */
    bp_tagdb_write_dint_array(db, zero_idx, v0, (uint16_t)total);
    free(v0); free(probe); free(v1);
}

static void test_dint_array(bp_tagdb_t *db, const char *tag, int count) {
    if (!tag || count <= 0) return;
    printf("\n[dint array] tag=%s count=%d\n", tag, count);
    int32_t *v0 = calloc(count, sizeof(int32_t));
    int32_t *probe = calloc(count, sizeof(int32_t));
    int32_t *v1 = calloc(count, sizeof(int32_t));
    if (!v0 || !probe || !v1) { free(v0); free(probe); free(v1); g_fail++; return; }

    MAYBE_ERR(bp_tagdb_read_dint_array(db, tag, v0, (uint16_t)count), "initial read");
    printf("  V0[0..%d] =", count > 8 ? 7 : count - 1);
    for (int i = 0; i < count && i < 8; i++) printf(" %d", v0[i]);
    printf("%s\n", count > 8 ? " ..." : "");

    for (int i = 0; i < count; i++) probe[i] = (int32_t)(0x1000 + i);
    MAYBE_ERR(bp_tagdb_write_dint_array(db, tag, probe, (uint16_t)count), "write");
    MAYBE_ERR(bp_tagdb_read_dint_array(db, tag, v1, (uint16_t)count), "post-write read");
    int all_match = 1;
    for (int i = 0; i < count; i++) {
        if (v1[i] != probe[i]) {
            printf("    MISMATCH at [%d]: expected %d, got %d\n", i, probe[i], v1[i]);
            all_match = 0;
        }
    }
    if (all_match) { printf("    ok (all %d elements match)\n", count); g_pass++; }
    else           { g_fail++; }
    MAYBE_ERR(bp_tagdb_write_dint_array(db, tag, v0, (uint16_t)count), "restore");
    free(v0); free(probe); free(v1);
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:1";
    const char *tags[] = {
        NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL,
    };
    enum { TAG_BOOL, TAG_SINT, TAG_INT, TAG_DINT, TAG_LINT,
           TAG_USINT, TAG_UINT, TAG_UDINT, TAG_ULINT,
           TAG_REAL, TAG_LREAL, TAG_STRING };
    const char *array_tag = NULL;
    int array_count = 0;
    const char *bool_arr_tag = NULL;
    int bool_arr_count = 0;
    const char *dint_2d_tag = NULL;
    int dint_2d_dim0 = 0, dint_2d_dim1 = 0;

    static struct option opts[] = {
        {"path",   required_argument, 0, 'p'},
        {"bool",   required_argument, 0, 1000+TAG_BOOL},
        {"sint",   required_argument, 0, 1000+TAG_SINT},
        {"int",    required_argument, 0, 1000+TAG_INT},
        {"dint",   required_argument, 0, 1000+TAG_DINT},
        {"lint",   required_argument, 0, 1000+TAG_LINT},
        {"usint",  required_argument, 0, 1000+TAG_USINT},
        {"uint",   required_argument, 0, 1000+TAG_UINT},
        {"udint",  required_argument, 0, 1000+TAG_UDINT},
        {"ulint",  required_argument, 0, 1000+TAG_ULINT},
        {"real",   required_argument, 0, 1000+TAG_REAL},
        {"lreal",  required_argument, 0, 1000+TAG_LREAL},
        {"string", required_argument, 0, 1000+TAG_STRING},
        {"dint-array",  required_argument, 0, 'a'},
        {"array-count", required_argument, 0, 'n'},
        {"bool-array",        required_argument, 0, 'A'},
        {"bool-array-count",  required_argument, 0, 'N'},
        {"dint-2d",      required_argument, 0, 'X'},
        {"dint-2d-dim0", required_argument, 0, 'Y'},
        {"dint-2d-dim1", required_argument, 0, 'Z'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:a:n:A:N:X:Y:Z:", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
        else if (c == 'a') array_tag = optarg;
        else if (c == 'n') array_count = atoi(optarg);
        else if (c == 'A') bool_arr_tag = optarg;
        else if (c == 'N') bool_arr_count = atoi(optarg);
        else if (c == 'X') dint_2d_tag = optarg;
        else if (c == 'Y') dint_2d_dim0 = atoi(optarg);
        else if (c == 'Z') dint_2d_dim1 = atoi(optarg);
        else if (c >= 1000) tags[c - 1000] = optarg;
    }

    bp_client_t *cl;
    int rc = bp_client_open(&cl);
    if (rc != BP_OK) { printf("FATAL Open: %s\n", bp_strerror(rc)); return 2; }
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    rc = bp_tagdb_open(cl, path, &db);
    if (rc != BP_OK) { printf("FATAL OpenTagDB: %s\n", bp_strerror(rc));
        bp_client_close(cl); return 2; }
    uint16_t n; bp_tagdb_build(db, &n);
    printf("[typetest] path=%s symbols=%u\n", path, (unsigned)n);

    test_bool (db, tags[TAG_BOOL]);
    test_sint (db, tags[TAG_SINT]);
    test_int  (db, tags[TAG_INT]);
    test_dint (db, tags[TAG_DINT]);
    test_lint (db, tags[TAG_LINT]);
    test_usint(db, tags[TAG_USINT]);
    test_uint (db, tags[TAG_UINT]);
    test_udint(db, tags[TAG_UDINT]);
    test_ulint(db, tags[TAG_ULINT]);
    test_real (db, tags[TAG_REAL]);
    test_lreal(db, tags[TAG_LREAL]);
    test_string(db, tags[TAG_STRING]);
    test_dint_array(db, array_tag, array_count);
    test_bool_array(db, bool_arr_tag, bool_arr_count);
    test_dint_2d(db, dint_2d_tag, dint_2d_dim0, dint_2d_dim1);

    bp_tagdb_close(db); bp_client_close(cl);

    printf("\n[typetest] PASS=%d FAIL=%d\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
