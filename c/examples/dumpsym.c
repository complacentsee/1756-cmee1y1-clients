/*
 * dumpsym.c — dump full symbol info for every symbol on a path.
 *
 * Used to verify array detection and inspect UDT entries.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdio.h>
#include <stdlib.h>

#include "bpclient.h"

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:1";
    static struct option opts[] = {
        {"path", required_argument, 0, 'p'}, {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
    }

    bp_client_t *cl;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    if (bp_tagdb_open(cl, path, &db) != BP_OK) { bp_client_close(cl); return 2; }
    uint16_t n; bp_tagdb_build(db, &n);
    printf("[dumpsym] path=%s symbols=%u\n", path, (unsigned)n);
    printf("%-4s %-44s %-9s %-7s %-10s %-10s %-10s %-10s %-6s %-3s %-3s\n",
           "idx","name","datatype","struct","field1","field2","field3","instid","flags","arr","udt");
    for (uint16_t i = 0; i < n; i++) {
        bp_symbol_info_t info;
        if (bp_tagdb_symbol_at(db, i, &info) != BP_OK) continue;
        printf("%-4u %-44s 0x%04x    0x%04x  0x%08x 0x%08x 0x%08x 0x%08x 0x%04x %d   %d\n",
               i, info.name, info.data_type, info.struct_type,
               info.field1, info.field2, info.field3, info.instance_id, info.flags,
               bp_symbol_is_array(&info), bp_symbol_is_struct(&info));
    }
    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
