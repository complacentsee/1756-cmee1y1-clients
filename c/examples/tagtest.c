/*
 * tagtest.c — the canonical smoke test.
 *
 * SPDX-License-Identifier: MIT
 *
 * Sequence:
 *   1. bp_client_open
 *   2. bp_client_open_session
 *   3. bp_tagdb_open("P:1,S:2")
 *   4. bp_tagdb_build  → expect symbol_count > 0
 *   5. bp_tagdb_symbol_at(0..9)  → print names + type codes
 *   6. bp_tagdb_read_dint  ("OCX_TEST")  → V0
 *   7. bp_tagdb_write_dint ("OCX_TEST", 0xDEADBEEF)
 *   8. bp_tagdb_read_dint  ("OCX_TEST")  → expect 0xDEADBEEF
 *   9. bp_tagdb_write_dint ("OCX_TEST", V0)   (restore)
 *  10. bp_tagdb_read_dint  ("OCX_TEST")  → expect V0
 *
 * Exit 0 on PASS, non-zero on FAIL.
 *
 * Build (via cmake — preferred):
 *   cmake -B build && cmake --build build
 *
 * Build (manual):
 *   cc -I../include tagtest.c ../src/client.c ../src/tagdb.c \
 *      ../src/access.c ../src/errors.c -lpthread -lrt -o tagtest
 *
 * Usage:
 *   tagtest [--path P:1,S:2] [--tag OCX_TEST] [--dump 10] [--no-write]
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

static void usage(const char *argv0) {
    fprintf(stderr,
        "usage: %s [--path P:1,S:2] [--tag OCX_TEST]\n"
        "         [--dump 10] [--no-write] [--help]\n",
        argv0);
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *tag  = "OCX_TEST";
    int dump_n = 10;
    int do_write = 1;

    static struct option opts[] = {
        {"path",     required_argument, 0, 'p'},
        {"tag",      required_argument, 0, 't'},
        {"dump",     required_argument, 0, 'd'},
        {"no-write", no_argument,       0, 'n'},
        {"help",     no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:t:d:nh", opts, &idx)) != -1) {
        switch (c) {
        case 'p': path = optarg; break;
        case 't': tag = optarg; break;
        case 'd': dump_n = atoi(optarg); break;
        case 'n': do_write = 0; break;
        case 'h':
        default:  usage(argv[0]); return c == 'h' ? 0 : 2;
        }
    }

    struct timespec t_start, t_a, t_b;
    clock_gettime(CLOCK_MONOTONIC, &t_start);

    printf("[tagtest] opening wrapper IPC\n");
    bp_client_t *client = NULL;
    int rc = bp_client_open(&client);
    if (rc != BP_OK) {
        fprintf(stderr, "[tagtest] FATAL Open: %s (%d)\n", bp_strerror(rc), rc);
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_a);
    printf("[tagtest]   ipc ready  dt=%.2fms\n", elapsed_ms(t_start, t_a));

    uint32_t session = 0;
    rc = bp_client_open_session(client, &session);
    if (rc != BP_OK) {
        fprintf(stderr, "[tagtest] FATAL OpenSession: %s (%d)\n", bp_strerror(rc), rc);
        bp_client_close(client);
        return 2;
    }
    printf("[tagtest] OCXcip_Open ok  session=0x%08" PRIx32 "\n", session);

    clock_gettime(CLOCK_MONOTONIC, &t_a);
    bp_tagdb_t *db = NULL;
    rc = bp_tagdb_open(client, path, &db);
    if (rc != BP_OK) {
        fprintf(stderr, "[tagtest] FATAL OpenTagDB(%s): %s (%d)\n",
                path, bp_strerror(rc), rc);
        bp_client_close(client);
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_b);
    printf("[tagtest] OpenTagDB(\"%s\") ok  dt=%.2fms\n",
           path, elapsed_ms(t_a, t_b));

    clock_gettime(CLOCK_MONOTONIC, &t_a);
    uint16_t n_symbols = 0;
    rc = bp_tagdb_build(db, &n_symbols);
    if (rc != BP_OK) {
        fprintf(stderr, "[tagtest] FATAL Build: %s (%d)\n", bp_strerror(rc), rc);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_b);
    printf("[tagtest] Build ok  symbols=%u  dt=%.2fms\n",
           (unsigned)n_symbols, elapsed_ms(t_a, t_b));

    printf("[tagtest] first %d symbols:\n", dump_n);
    for (int i = 0; i < dump_n && i < (int)n_symbols; i++) {
        bp_symbol_info_t info;
        rc = bp_tagdb_symbol_at(db, (uint16_t)i, &info);
        if (rc != BP_OK) {
            printf("  [%4d] error: %s (%d)\n", i, bp_strerror(rc), rc);
            continue;
        }
        printf("  [%4d] %-40s type=0x%04x struct=0x%04x\n",
               i, info.name, info.data_type & 0x1FFF, info.struct_type);
    }

    clock_gettime(CLOCK_MONOTONIC, &t_a);
    int32_t v0 = 0;
    rc = bp_tagdb_read_dint(db, tag, &v0);
    if (rc != BP_OK) {
        fprintf(stderr, "[tagtest] FATAL ReadDINT(%s): %s (%d)\n",
                tag, bp_strerror(rc), rc);
        bp_tagdb_close(db);
        bp_client_close(client);
        return 2;
    }
    clock_gettime(CLOCK_MONOTONIC, &t_b);
    printf("[tagtest] V0 = %" PRId32 " (0x%08" PRIx32 ")  dt=%.2fms\n",
           v0, (uint32_t)v0, elapsed_ms(t_a, t_b));

    int passed_write = 1;
    int passed_restore = 1;

    if (do_write) {
        const int32_t sentinel = (int32_t)0xDEADBEEFu;
        clock_gettime(CLOCK_MONOTONIC, &t_a);
        rc = bp_tagdb_write_dint(db, tag, sentinel);
        if (rc != BP_OK) {
            fprintf(stderr, "[tagtest] FATAL Write 0xDEADBEEF: %s (%d)\n",
                    bp_strerror(rc), rc);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        clock_gettime(CLOCK_MONOTONIC, &t_b);
        printf("[tagtest] wrote 0x%08" PRIx32 "  dt=%.2fms\n",
               (uint32_t)sentinel, elapsed_ms(t_a, t_b));

        int32_t v1 = 0;
        rc = bp_tagdb_read_dint(db, tag, &v1);
        if (rc != BP_OK) {
            fprintf(stderr, "[tagtest] FATAL Read post-write: %s (%d)\n",
                    bp_strerror(rc), rc);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        passed_write = (v1 == sentinel);
        printf("[tagtest] V1 = %" PRId32 " (0x%08" PRIx32 ")  %s\n",
               v1, (uint32_t)v1,
               passed_write ? "<-- WRITE OK" : "<-- WRITE DID NOT TAKE");

        rc = bp_tagdb_write_dint(db, tag, v0);
        if (rc != BP_OK) {
            fprintf(stderr, "[tagtest] FATAL Restore: %s (%d)\n",
                    bp_strerror(rc), rc);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }

        int32_t v2 = 0;
        rc = bp_tagdb_read_dint(db, tag, &v2);
        if (rc != BP_OK) {
            fprintf(stderr, "[tagtest] FATAL Read post-restore: %s (%d)\n",
                    bp_strerror(rc), rc);
            bp_tagdb_close(db);
            bp_client_close(client);
            return 2;
        }
        passed_restore = (v2 == v0);
        printf("[tagtest] V2 = %" PRId32 " (0x%08" PRIx32 ")  %s\n",
               v2, (uint32_t)v2,
               passed_restore ? "<-- RESTORED OK" : "<-- RESTORE DID NOT TAKE");
    } else {
        printf("[tagtest] --no-write  skipping write/restore\n");
    }

    bp_tagdb_close(db);
    bp_client_close(client);

    struct timespec t_end;
    clock_gettime(CLOCK_MONOTONIC, &t_end);
    if (passed_write && passed_restore) {
        printf("\n[tagtest] READ-WRITE-READBACK: PASS  total dt=%.2fms\n",
               elapsed_ms(t_start, t_end));
        return 0;
    }
    printf("\n[tagtest] READ-WRITE-READBACK: FAIL\n");
    return 1;
}
