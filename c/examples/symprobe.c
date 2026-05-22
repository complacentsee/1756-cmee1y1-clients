/*
 * symprobe.c — call OCXcip_GetSymbolInfo with an oversized payload
 *              window and hex-dump the raw response.  Diagnostic
 *              tool for RE'ing the symbol-info struct layout
 *              (specifically: where does a 3-D array's third
 *              dimension live?).
 *
 * Usage:
 *   symprobe --path P:1,S:2                       # dump all symbols
 *   symprobe --path P:1,S:2 --match Test_DINT     # filter by prefix
 *   symprobe --path P:1,S:2 --index 12            # one symbol by index
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"
/* internal headers for direct call dispatch */
#include "../src/proto.h"

/* Dump 256 bytes from slot+0x80 — double the documented 128 — so
 * any extra fields the vendor engine writes beyond the known layout
 * become visible. */
#define SYMPROBE_RAW_LEN 256

typedef struct {
    uint32_t db_handle;
    uint16_t index;
    uint8_t  resp[SYMPROBE_RAW_LEN];
} sp_ctx_t;

static void sp_fill(uint8_t *slot, void *user) {
    sp_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->db_handle);
    bp_st_u16(slot + 0x7C, c->index);
}

static void sp_read(uint8_t *slot, void *user) {
    sp_ctx_t *c = user;
    memcpy(c->resp, slot + 0x80, SYMPROBE_RAW_LEN);
}

static void hexdump(const void *buf, size_t len) {
    const uint8_t *b = buf;
    for (size_t i = 0; i < len; i += 16) {
        printf("    +%03zx ", i);
        size_t line = (len - i) > 16 ? 16 : (len - i);
        for (size_t j = 0; j < line; j++) printf("%02x ", b[i+j]);
        for (size_t j = line; j < 16; j++) printf("   ");
        printf(" ");
        for (size_t j = 0; j < line; j++) {
            unsigned char ch = b[i+j];
            putchar((ch >= 0x20 && ch < 0x7f) ? (char)ch : '.');
        }
        putchar('\n');
    }
}

/* Mirror tagdb.c's internal struct layout so we can read the handle. */
struct bp_tagdb_internal { bp_client_t *client; uint32_t handle; char path[256]; };

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *match = NULL;
    int specific_index = -1;

    static struct option opts[] = {
        {"path",  required_argument, 0, 'p'},
        {"match", required_argument, 0, 'm'},
        {"index", required_argument, 0, 'i'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:m:i:", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
        else if (c == 'm') match = optarg;
        else if (c == 'i') specific_index = atoi(optarg);
    }

    bp_client_t *cl;
    if (bp_client_open(&cl) != BP_OK) { fprintf(stderr, "open client failed\n"); return 2; }
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    if (bp_tagdb_open(cl, path, &db) != BP_OK) { bp_client_close(cl); return 2; }
    uint16_t n = 0;
    bp_tagdb_build(db, &n);
    printf("[symprobe] path=%s symbols=%u raw_len=%d\n", path, (unsigned)n, SYMPROBE_RAW_LEN);
    if (match)              printf("           filter: name has prefix '%s'\n", match);
    if (specific_index >= 0) printf("           filter: index = %d\n", specific_index);
    printf("\n");

    uint32_t handle = ((struct bp_tagdb_internal *)db)->handle;

    int dumped = 0;
    for (uint16_t i = 0; i < n; i++) {
        if (specific_index >= 0 && i != (uint16_t)specific_index) continue;

        /* First grab the public-API view so we can filter by name
         * without re-decoding the raw bytes here. */
        bp_symbol_info_t info;
        if (bp_tagdb_symbol_at(db, i, &info) != BP_OK) continue;
        if (match && strncmp(info.name, match, strlen(match)) != 0) continue;

        /* Now do an oversized raw dump.  Payload size 0x200 ensures
         * the server can write past the documented 128-byte limit
         * if there's more data to surface. */
        sp_ctx_t ctx = { .db_handle = handle, .index = i };
        memset(ctx.resp, 0xAA, sizeof(ctx.resp));  /* sentinel: any 0xAA
                                                    * in the dump = bytes
                                                    * the server didn't
                                                    * overwrite */
        bp_call_spec_t spec = {
            .fn_name      = "OCXcip_GetSymbolInfo",
            .payload_size = 0x200,
            .fill_payload = sp_fill,
            .read_reply   = sp_read,
            .timeout_ms   = 5000,
            .user         = &ctx,
        };
        int rc = bp_client_call(cl, &spec);
        if (rc != BP_OK) {
            printf("[%u] '%s' — call failed: rc=%d (%s)\n",
                   i, info.name, rc, bp_strerror(rc));
            continue;
        }

        printf("[%u] '%s'\n", i, info.name);
        printf("    decoded: data_type=0x%04x struct_type=0x%04x "
               "elem_byte_size=%u dim0=%u dim1=%u dim2=%u flags=0x%04x rank=%d\n",
               info.data_type, info.struct_type, info.elem_byte_size,
               info.dim0, info.dim1, info.dim2, info.flags,
               bp_symbol_rank(&info));
        hexdump(ctx.resp, SYMPROBE_RAW_LEN);
        printf("\n");
        dumped++;
    }

    if (!dumped) printf("(no symbols matched)\n");

    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
