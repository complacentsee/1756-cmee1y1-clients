/*
 * multitagtest.c — v0.9.0 Phase 2 multi-tag read validator.
 *
 * Reads OCX_TEST (DINT) via bp_tagdb_read_tags and prints the
 * decoded value.  Demonstrates the variant-struct API: each
 * bp_value_t carries (kind, cip_status, union value).
 *
 * Output format is byte-identical (modulo timing) across the three
 * SDKs; cross-language gate diffs the summary line.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
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

static void print_value(const char *name, const bp_value_t *v) {
    if (v->cip_status != 0) {
        printf("[multitagtest] %s: CIP status 0x%02x (FAIL)\n", name, v->cip_status);
        return;
    }
    switch (v->kind) {
    case BP_VAL_BOOL:  printf("[multitagtest] %s = %s (BOOL)\n",  name, v->v.boolean ? "TRUE" : "FALSE"); break;
    case BP_VAL_SINT:  printf("[multitagtest] %s = %d (SINT)\n",  name, v->v.sint); break;
    case BP_VAL_INT:   printf("[multitagtest] %s = %d (INT)\n",   name, v->v.int_); break;
    case BP_VAL_DINT:  printf("[multitagtest] %s = %d (DINT)\n",  name, v->v.dint); break;
    case BP_VAL_LINT:  printf("[multitagtest] %s = %lld (LINT)\n",name, (long long)v->v.lint); break;
    case BP_VAL_USINT: printf("[multitagtest] %s = %u (USINT)\n", name, v->v.usint); break;
    case BP_VAL_UINT:  printf("[multitagtest] %s = %u (UINT)\n",  name, v->v.uint); break;
    case BP_VAL_UDINT: printf("[multitagtest] %s = %u (UDINT)\n", name, v->v.udint); break;
    case BP_VAL_ULINT: printf("[multitagtest] %s = %llu (ULINT)\n",name,(unsigned long long)v->v.ulint); break;
    case BP_VAL_REAL:  printf("[multitagtest] %s = %f (REAL)\n",  name, (double)v->v.real); break;
    case BP_VAL_LREAL: printf("[multitagtest] %s = %f (LREAL)\n", name, v->v.lreal); break;
    default:           printf("[multitagtest] %s: unknown kind %d\n", name, v->kind); break;
    }
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";

    static struct option opts[] = {
        {"path", required_argument, 0, 'p'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "p:", opts, &idx)) != -1) {
        if (oc == 'p') path = optarg;
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[multitagtest] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    bp_tagdb_t *db = NULL;
    int rc = bp_tagdb_open(cl, path, &db);
    if (rc != BP_OK) {
        fprintf(stderr, "[multitagtest] tagdb_open rc=%d\n", rc);
        bp_client_close(cl); return 1;
    }
    uint16_t n = 0;
    rc = bp_tagdb_build(db, &n);
    if (rc != BP_OK) {
        fprintf(stderr, "[multitagtest] build rc=%d\n", rc);
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }
    printf("[multitagtest] path=%s build n=%u\n", path, n);

    /* Default scan set: walk the symbol table and grab the first
     * scalar tags we find.  This makes multitagtest work on any PLC
     * without pre-known tag names.  Cap at 10 to keep the batch
     * inside the engine's per-call request limit. */
    const size_t MAX_TAGS = 10;
    const char *names[MAX_TAGS];
    char *name_storage[MAX_TAGS];
    size_t count = 0;
    if (bp_tagdb_preload_symbols(db) != BP_OK) {
        fprintf(stderr, "[multitagtest] preload_symbols failed\n");
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }
    for (uint16_t i = 0; i < n && count < MAX_TAGS; i++) {
        bp_symbol_info_t sym;
        if (bp_tagdb_symbol_at(db, i, &sym) != BP_OK) continue;
        if (sym.dim0 != 0 || sym.struct_type != 0) continue;  /* scalars only */
        uint16_t t = sym.data_type & 0x1FFF;
        if (t < BP_TYPE_BOOL || t > BP_TYPE_LREAL) continue;
        if (sym.name[0] == '\0') continue;
        name_storage[count] = strdup(sym.name);
        names[count] = name_storage[count];
        count++;
    }
    if (count == 0) {
        fprintf(stderr, "[multitagtest] no scalar tags found on PLC\n");
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }
    bp_value_t *values = calloc(count, sizeof(*values));
    if (!values) {
        fprintf(stderr, "[multitagtest] alloc failed\n");
        bp_tagdb_close(db); bp_client_close(cl); return 1;
    }

    double t0 = now_ms();
    rc = bp_tagdb_read_tags(db, names, count, values);
    double t1 = now_ms();
    printf("[multitagtest] read_tags %zu tags dt=%.2fms rc=%d\n",
           count, t1 - t0, rc);

    int ok = 0, failed = 0;
    for (size_t i = 0; i < count; i++) {
        print_value(names[i], &values[i]);
        if (values[i].cip_status == 0) ok++; else failed++;
        bp_value_clear(&values[i]);
        free(name_storage[i]);
    }
    free(values);

    int pass = (failed == 0) && (rc == BP_OK || rc == BP_ERR_GENERIC);
    printf("[multitagtest] SUMMARY ok=%d failed=%d total=%zu\n", ok, failed, count);

    /* Phase 3 round-trip: write OCX_TEST, read back, restore.  Skips
     * gracefully if OCX_TEST isn't on this PLC. */
    bp_symbol_info_t ocx;
    if (bp_tagdb_lookup_symbol(db, "OCX_TEST", &ocx) == BP_OK
        && (ocx.data_type & 0x1FFF) == BP_TYPE_DINT) {
        const char *write_names[1] = { "OCX_TEST" };
        bp_value_t  read_back;
        bp_value_t  write_value = { .kind = BP_VAL_DINT };

        /* Snapshot original */
        int32_t original = 0;
        if (bp_tagdb_read_tags(db, write_names, 1, &read_back) == BP_OK) {
            original = read_back.v.dint;
            bp_value_clear(&read_back);
        }

        /* Write 0x12345678 */
        write_value.v.dint = 0x12345678;
        int wrc = bp_tagdb_write_tags(db, write_names, &write_value, 1);
        printf("[multitagtest] write_tags OCX_TEST=0x12345678 rc=%d\n", wrc);

        /* Read back */
        memset(&read_back, 0, sizeof(read_back));
        int rrc = bp_tagdb_read_tags(db, write_names, 1, &read_back);
        int verified = (rrc == BP_OK && read_back.v.dint == 0x12345678);
        printf("[multitagtest] read-after-write OCX_TEST=0x%08x verified=%s\n",
               read_back.v.dint, verified ? "YES" : "NO");
        bp_value_clear(&read_back);

        /* Restore */
        write_value.v.dint = original;
        (void)bp_tagdb_write_tags(db, write_names, &write_value, 1);

        /* Type-mismatch demonstration — should reject pre-IPC. */
        bp_value_t bad = { .kind = BP_VAL_REAL, .v.real = 1.5f };
        int brc = bp_tagdb_write_tags(db, write_names, &bad, 1);
        printf("[multitagtest] type-mismatch reject rc=%d (expect -305)\n", brc);

        if (!verified) pass = 0;
    } else {
        printf("[multitagtest] (skipped write roundtrip — OCX_TEST not found or not DINT)\n");
    }

    printf("[multitagtest] %s\n", pass ? "PASS" : "FAIL");

    bp_tagdb_close(db);
    bp_client_close(cl);
    return pass ? 0 : 1;
}
