/*
 * testtagver.c — validate bp_tagdb_test_version() against a PLC.
 *
 *   1. Open + OpenTagDB (no Build yet)
 *      → expect rc=BP_OK, changed=1 (engine 0x15: tagdb not built)
 *   2. Build
 *      → returns symbol count
 *   3. TestTagDbVer
 *      → expect rc=BP_OK, changed=0 (engine 0: versions match)
 *   4. TestTagDbVer again
 *      → still 0 (verify the version stays captured across calls)
 *
 * Manual follow-up after step 4 (user action):
 *   - In Studio 5000, add or rename a controller-scope tag,
 *     then run this program with `--after-edit`.  In that mode it
 *     skips Build and just calls TestTagDbVer once; expect changed=1.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    int after_edit = 0;
    static struct option opts[] = {
        {"path",       required_argument, 0, 'p'},
        {"after-edit", no_argument,       0, 'e'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:e", opts, &idx)) != -1) {
        if      (c == 'p') path = optarg;
        else if (c == 'e') after_edit = 1;
    }

    bp_client_t *cl = NULL;
    int rc = bp_client_open(&cl);
    if (rc != BP_OK) { printf("FATAL Open: %s (%d)\n", bp_strerror(rc), rc); return 2; }
    rc = bp_client_open_session(cl, NULL);
    if (rc != BP_OK) { printf("FATAL OpenSession: %s (%d)\n", bp_strerror(rc), rc); bp_client_close(cl); return 2; }

    bp_tagdb_t *db = NULL;
    rc = bp_tagdb_open(cl, path, &db);
    if (rc != BP_OK) { printf("FATAL OpenTagDB: %s (%d)\n", bp_strerror(rc), rc); bp_client_close(cl); return 2; }

    int pass = 0, fail = 0;

    if (after_edit) {
        printf("[after-edit mode] one TestTagDbVer call against existing tagdb handle:\n");
        printf("  (we just opened a fresh handle — engine will say 'not built' until BuildTagDb runs)\n");
        printf("  This mode is for manual re-runs after Studio 5000 tag edits to check that the\n"
               "  *server-side* version probe sees the change once a Build has been done in a\n"
               "  prior session.  For that you actually need a long-lived handle, which this\n"
               "  process doesn't have.  Use the 4-step sequence below for first-time validation.\n\n");
    }

    /* Step 1: TestTagDbVer before Build — expect changed=1 (engine 0x15 path) */
    printf("[step 1] TestTagDbVer pre-Build\n");
    {
        int changed = -1;
        rc = bp_tagdb_test_version(db, &changed);
        printf("  rc=%d (%s), changed=%d\n", rc, bp_strerror(rc), changed);
        if (rc == BP_OK && changed == 1) { printf("  ok (engine reports 'not built / different')\n"); pass++; }
        else { printf("  FAIL: expected rc=0 changed=1\n"); fail++; }
    }

    /* Step 2: Build */
    printf("\n[step 2] BuildTagDb\n");
    {
        uint16_t n = 0;
        rc = bp_tagdb_build(db, &n);
        printf("  rc=%d (%s), symbol_count=%u\n", rc, bp_strerror(rc), (unsigned)n);
        if (rc == BP_OK && n > 0) { printf("  ok\n"); pass++; }
        else { printf("  FAIL\n"); fail++; }
    }

    /* Step 3: TestTagDbVer post-Build — expect changed=0 */
    printf("\n[step 3] TestTagDbVer post-Build\n");
    {
        int changed = -1;
        rc = bp_tagdb_test_version(db, &changed);
        printf("  rc=%d (%s), changed=%d\n", rc, bp_strerror(rc), changed);
        if (rc == BP_OK && changed == 0) { printf("  ok (engine reports 'unchanged')\n"); pass++; }
        else { printf("  FAIL: expected rc=0 changed=0\n"); fail++; }
    }

    /* Step 4: TestTagDbVer again — still expect changed=0 */
    printf("\n[step 4] TestTagDbVer (second call, no Studio 5000 edits)\n");
    {
        int changed = -1;
        rc = bp_tagdb_test_version(db, &changed);
        printf("  rc=%d (%s), changed=%d\n", rc, bp_strerror(rc), changed);
        if (rc == BP_OK && changed == 0) { printf("  ok\n"); pass++; }
        else { printf("  FAIL: expected rc=0 changed=0\n"); fail++; }
    }

    bp_tagdb_close(db);
    bp_client_close(cl);

    printf("\n[testtagver] PASS=%d FAIL=%d\n", pass, fail);
    return fail == 0 ? 0 : 1;
}
