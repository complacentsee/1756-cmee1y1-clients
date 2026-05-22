/*
 * udtinfo.c — dump a UDT's full schema using the proper API.
 *
 * Usage:
 *   udtinfo --path P:1,S:2 --tag Tran_To_iSeries_FIFO_Loader
 *
 * Looks up the tag's struct_id from symbol enumeration, then walks
 * every member with bp_tagdb_get_struct_member().  For nested UDT
 * members, also dumps the nested struct's members.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bpclient.h"

static const char *typename(uint16_t code) {
    switch (code & 0x1FFF) {
    case 0xC1: return "BOOL";   case 0xC2: return "SINT";
    case 0xC3: return "INT";    case 0xC4: return "DINT";
    case 0xC5: return "LINT";   case 0xC6: return "USINT";
    case 0xC7: return "UINT";   case 0xC8: return "UDINT";
    case 0xC9: return "ULINT";  case 0xCA: return "REAL";
    case 0xCB: return "LREAL";  case 0xD3: return "BIT_ARRAY";
    default: return "STRUCT/?";
    }
}

static void dump_struct(bp_tagdb_t *db, uint16_t struct_id, int depth) {
    bp_struct_info_t si;
    int rc = bp_tagdb_get_struct_info(db, struct_id, &si);
    if (rc != BP_OK) {
        printf("%*s[ERROR] struct 0x%02x: %s (%d)\n",
               depth * 2, "", struct_id, bp_strerror(rc), rc);
        return;
    }
    printf("%*sstruct '%s'  id=0x%02x  data_type=0x%04x  size=%u bytes  members=%u\n",
           depth * 2, "", si.name, struct_id, si.data_type, si.byte_size, si.n_members);

    for (uint32_t i = 0; i < si.n_members; i++) {
        bp_struct_member_info_t m;
        rc = bp_tagdb_get_struct_member(db, struct_id, (uint16_t)i, &m);
        if (rc != BP_OK) {
            printf("%*s  [%u] ERROR: %s\n", depth * 2, "", i, bp_strerror(rc));
            continue;
        }
        const char *tn = m.struct_id ? "STRUCT" : typename(m.data_type);
        printf("%*s  [%u] %-24s @+%-4u  type=%-8s  size=%u",
               depth * 2, "", i, m.name, m.offset, tn, m.byte_size);
        if (m.array_count) printf("[%u]", m.array_count);
        if (m.struct_id)   printf("  struct_id=0x%02x", m.struct_id);
        printf("  flags=0x%02x\n", m.flags);

        /* Recurse into nested UDTs (depth-limited) */
        if (m.struct_id && depth < 2 && m.struct_id != struct_id) {
            dump_struct(db, m.struct_id, depth + 1);
        }
    }
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *tag = "Tran_To_iSeries_FIFO_Loader";
    int struct_id = -1;
    static struct option opts[] = {
        {"path",      required_argument, 0, 'p'},
        {"tag",       required_argument, 0, 't'},
        {"struct-id", required_argument, 0, 's'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:t:s:", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
        else if (c == 't') tag = optarg;
        else if (c == 's') struct_id = (int)strtol(optarg, NULL, 0);
    }

    bp_client_t *cl;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    if (bp_tagdb_open(cl, path, &db) != BP_OK) { bp_client_close(cl); return 2; }
    uint16_t n; bp_tagdb_build(db, &n);
    printf("[udtinfo] path=%s symbols=%u\n\n", path, n);

    /* If --struct-id wasn't given, look up the tag and use its struct_type */
    if (struct_id < 0) {
        int found = 0;
        for (uint16_t i = 0; i < n; i++) {
            bp_symbol_info_t info;
            if (bp_tagdb_symbol_at(db, i, &info) != BP_OK) continue;
            if (strcmp(info.name, tag) == 0) {
                struct_id = info.struct_type;
                found = 1;
                printf("Found tag '%s' -> struct_id=0x%02x\n\n", tag, struct_id);
                break;
            }
        }
        if (!found) {
            printf("Tag '%s' not found in enumeration\n", tag);
            bp_tagdb_close(db); bp_client_close(cl); return 2;
        }
    }

    dump_struct(db, (uint16_t)struct_id, 0);

    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
