/*
 * findtags.c — find all symbols whose name starts with a prefix.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdio.h>
#include <string.h>
#include "bpclient.h"

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *prefix = "Test_";
    static struct option opts[] = {
        {"path",   required_argument, 0, 'p'},
        {"prefix", required_argument, 0, 'x'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:x:", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
        else if (c == 'x') prefix = optarg;
    }
    bp_client_t *cl; bp_client_open(&cl); bp_client_open_session(cl, NULL);
    bp_tagdb_t *db; bp_tagdb_open(cl, path, &db);
    uint16_t n; bp_tagdb_build(db, &n);
    printf("path=%s symbols=%u prefix='%s'\n\n", path, n, prefix);
    size_t plen = strlen(prefix);
    for (uint16_t i = 0; i < n; i++) {
        bp_symbol_info_t info;
        if (bp_tagdb_symbol_at(db, i, &info) != BP_OK) continue;
        if (strncmp(info.name, prefix, plen) != 0) continue;
        printf("  %-40s datatype=0x%04x struct=0x%04x  field1=0x%x  dim2=0x%x  dim3=0x%x  arr=%d  udt=%d  type_code=0x%04x\n",
               info.name, info.data_type, info.struct_type,
               info.field1, info.field2, info.field3,
               bp_symbol_is_array(&info), bp_symbol_is_struct(&info),
               bp_symbol_type_code(&info));
    }
    bp_tagdb_close(db); bp_client_close(cl);
    return 0;
}
