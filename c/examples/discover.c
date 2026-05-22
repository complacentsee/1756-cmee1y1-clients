/*
 * discover.c — enumerate symbols on a PLC, group by type.
 *
 * Usage:
 *   discover [--path P:1,S:1] [--max 4000]
 *
 * Prints a per-type summary and lists up to 5 tag names per atomic type.
 * Useful for finding safe test tags on an unknown PLC.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static const char *type_name(uint16_t code) {
    switch (code) {
    case BP_TYPE_BOOL:  return "BOOL";
    case BP_TYPE_SINT:  return "SINT";
    case BP_TYPE_INT:   return "INT";
    case BP_TYPE_DINT:  return "DINT";
    case BP_TYPE_LINT:  return "LINT";
    case BP_TYPE_USINT: return "USINT";
    case BP_TYPE_UINT:  return "UINT";
    case BP_TYPE_UDINT: return "UDINT";
    case BP_TYPE_ULINT: return "ULINT";
    case BP_TYPE_REAL:  return "REAL";
    case BP_TYPE_LREAL: return "LREAL";
    default:            return "OTHER";
    }
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:1";
    int max_dump = 4000;

    static struct option opts[] = {
        {"path", required_argument, 0, 'p'},
        {"max",  required_argument, 0, 'm'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:m:", opts, &idx)) != -1) {
        switch (c) {
        case 'p': path = optarg; break;
        case 'm': max_dump = atoi(optarg); break;
        default: return 2;
        }
    }

    printf("[discover] path=%s\n", path);
    bp_client_t *client;
    if (bp_client_open(&client) != BP_OK) { printf("FATAL: Open\n"); return 2; }
    bp_client_open_session(client, NULL);

    bp_tagdb_t *db;
    if (bp_tagdb_open(client, path, &db) != BP_OK) {
        printf("FATAL: OpenTagDB(%s)\n", path); bp_client_close(client); return 2;
    }

    uint16_t n;
    int rc = bp_tagdb_build(db, &n);
    if (rc != BP_OK) { printf("FATAL: Build: %s\n", bp_strerror(rc));
        bp_tagdb_close(db); bp_client_close(client); return 2; }
    printf("[discover] %u symbols total\n", (unsigned)n);

    /* Counts per type code, plus example names */
    struct { uint16_t code; const char *name; int count; int arr_count;
             char examples[5][80]; int n_examples; char arr_examples[5][80];
             int n_arr_examples; } buckets[20] = {0};
    const uint16_t known[] = {
        BP_TYPE_BOOL, BP_TYPE_SINT, BP_TYPE_USINT, BP_TYPE_INT, BP_TYPE_UINT,
        BP_TYPE_DINT, BP_TYPE_UDINT, BP_TYPE_LINT, BP_TYPE_ULINT,
        BP_TYPE_REAL, BP_TYPE_LREAL,
    };
    int n_buckets = (int)(sizeof(known) / sizeof(known[0]));
    for (int i = 0; i < n_buckets; i++) {
        buckets[i].code = known[i];
        buckets[i].name = type_name(known[i]);
    }
    int other_idx = n_buckets++;
    buckets[other_idx].code = 0xFFFF;
    buckets[other_idx].name = "OTHER";

    int total_dumped = 0;
    int n_atomic_scalar = 0;
    int n_arrays = 0;
    int n_structs = 0;

    int limit = max_dump < (int)n ? max_dump : (int)n;
    for (int i = 0; i < limit; i++) {
        bp_symbol_info_t info;
        if (bp_tagdb_symbol_at(db, (uint16_t)i, &info) != BP_OK) continue;
        total_dumped++;

        uint16_t code = bp_symbol_type_code(&info);
        int is_arr  = bp_symbol_is_array(&info);
        int is_str  = bp_symbol_is_struct(&info);
        if (is_str) n_structs++;

        int b = other_idx;
        for (int k = 0; k < n_buckets; k++) {
            if (buckets[k].code == code) { b = k; break; }
        }

        if (is_str) continue;  /* don't bucket UDTs as atomic */
        if (is_arr) {
            buckets[b].arr_count++;
            n_arrays++;
            if (buckets[b].n_arr_examples < 5) {
                strncpy(buckets[b].arr_examples[buckets[b].n_arr_examples],
                        info.name, sizeof(buckets[b].arr_examples[0]) - 1);
                buckets[b].n_arr_examples++;
            }
        } else {
            buckets[b].count++;
            n_atomic_scalar++;
            if (buckets[b].n_examples < 5) {
                strncpy(buckets[b].examples[buckets[b].n_examples],
                        info.name, sizeof(buckets[b].examples[0]) - 1);
                buckets[b].n_examples++;
            }
        }
    }

    printf("\n[discover] enumerated %d symbols:\n", total_dumped);
    printf("  atomic scalars : %d\n", n_atomic_scalar);
    printf("  atomic arrays  : %d\n", n_arrays);
    printf("  UDT (struct)   : %d\n", n_structs);

    printf("\n[discover] per-atomic-type summary:\n");
    for (int k = 0; k < n_buckets; k++) {
        if (buckets[k].count == 0 && buckets[k].arr_count == 0) continue;
        printf("\n  %-6s (code 0x%04x): %d scalar, %d array\n",
               buckets[k].name, buckets[k].code,
               buckets[k].count, buckets[k].arr_count);
        if (buckets[k].n_examples > 0) {
            printf("    scalar examples:\n");
            for (int e = 0; e < buckets[k].n_examples; e++)
                printf("      %s\n", buckets[k].examples[e]);
        }
        if (buckets[k].n_arr_examples > 0) {
            printf("    array examples:\n");
            for (int e = 0; e < buckets[k].n_arr_examples; e++)
                printf("      %s\n", buckets[k].arr_examples[e]);
        }
    }

    bp_tagdb_close(db);
    bp_client_close(client);
    return 0;
}
