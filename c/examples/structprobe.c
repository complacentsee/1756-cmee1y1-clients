/*
 * structprobe.c — call OCXcip_GetStructInfo + OCXcip_GetStructMbrInfo
 *                 by manually building the slot payloads, dump raw
 *                 response bytes to figure out the layout.
 *
 * Usage:
 *   structprobe --path P:1,S:2 --struct-id 0x23
 *   structprobe --path P:1,S:2 --struct-id 0x23 --member 0
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

typedef struct {
    uint32_t db_handle;
    uint16_t struct_id;
    uint8_t  resp[64];
} si_ctx_t;

static void si_fill(uint8_t *slot, void *user) {
    si_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->db_handle);
    bp_st_u16(slot + 0x7C, c->struct_id);
}
static void si_read(uint8_t *slot, void *user) {
    si_ctx_t *c = user;
    /* 7 qwords copied from slot+0x80 .. slot+0xB7 (56 bytes) */
    memcpy(c->resp, slot + 0x80, 56);
}

typedef struct {
    uint32_t db_handle;
    uint16_t struct_id;
    uint16_t member_idx;
    uint8_t  resp[80];
} smi_ctx_t;

static void smi_fill(uint8_t *slot, void *user) {
    smi_ctx_t *c = user;
    bp_st_u32(slot + 0x78, c->db_handle);
    bp_st_u16(slot + 0x7C, c->struct_id);
    bp_st_u16(slot + 0x7E, c->member_idx);
}
static void smi_read(uint8_t *slot, void *user) {
    smi_ctx_t *c = user;
    /* GetStructMbrInfo writes ~76 bytes of response across slot+0x80..0xCB */
    memcpy(c->resp, slot + 0x80, 76);
}

static void hexdump(const void *buf, size_t len) {
    const uint8_t *b = buf;
    for (size_t i = 0; i < len; i += 16) {
        printf("    +%02zx ", i);
        size_t line = (len - i) > 16 ? 16 : (len - i);
        for (size_t j = 0; j < line; j++) printf("%02x ", b[i+j]);
        for (size_t j = line; j < 16; j++) printf("   ");
        printf(" ");
        for (size_t j = 0; j < line; j++) {
            unsigned char c = b[i+j];
            putchar((c >= 0x20 && c < 0x7f) ? (char)c : '.');
        }
        putchar('\n');
    }
}

/* Need access to internals to query the db_handle. */
struct bp_tagdb_internal { bp_client_t *client; uint32_t handle; char path[256]; };

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    int struct_id = -1;
    int member = -1;
    int dump_all_members = 0;

    static struct option opts[] = {
        {"path", required_argument, 0, 'p'},
        {"struct-id", required_argument, 0, 's'},
        {"member", required_argument, 0, 'm'},
        {"all", no_argument, 0, 'a'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:s:m:a", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
        else if (c == 's') struct_id = (int)strtol(optarg, NULL, 0);
        else if (c == 'm') member = atoi(optarg);
        else if (c == 'a') dump_all_members = 1;
    }
    if (struct_id < 0) {
        fprintf(stderr, "use --struct-id 0x23 (or decimal) to pick a UDT\n");
        return 2;
    }

    bp_client_t *cl;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    if (bp_tagdb_open(cl, path, &db) != BP_OK) { bp_client_close(cl); return 2; }
    uint16_t n; bp_tagdb_build(db, &n);
    printf("[structprobe] path=%s symbols=%u struct_id=0x%02x\n\n",
           path, n, struct_id);

    uint32_t handle = ((struct bp_tagdb_internal *)db)->handle;

    /* ---- GetStructInfo ---- */
    {
        si_ctx_t ctx = { .db_handle = handle, .struct_id = (uint16_t)struct_id };
        bp_call_spec_t spec = {
            .fn_name = "OCXcip_GetStructInfo",
            .payload_size = 0xB8,
            .fill_payload = si_fill,
            .read_reply   = si_read,
            .timeout_ms   = 5000,
            .user         = &ctx,
        };
        int rc = bp_client_call(cl, &spec);
        if (rc != BP_OK) {
            printf("GetStructInfo rc=%d (%s)\n", rc, bp_strerror(rc));
        } else {
            printf("GetStructInfo response (56 bytes):\n");
            hexdump(ctx.resp, 56);
        }
    }

    /* ---- GetStructMbrInfo ---- */
    if (member >= 0 || dump_all_members) {
        int start = member >= 0 ? member : 0;
        int end   = dump_all_members ? 64 : (member + 1);  /* try up to 64 members */
        for (int m = start; m < end; m++) {
            smi_ctx_t ctx = {
                .db_handle = handle,
                .struct_id = (uint16_t)struct_id,
                .member_idx = (uint16_t)m,
            };
            bp_call_spec_t spec = {
                .fn_name = "OCXcip_GetStructMbrInfo",
                .payload_size = 0xD0,
                .fill_payload = smi_fill,
                .read_reply   = smi_read,
                .timeout_ms   = 5000,
                .user         = &ctx,
            };
            int rc = bp_client_call(cl, &spec);
            if (rc != BP_OK) {
                if (dump_all_members) {
                    printf("\nGetStructMbrInfo[%d] rc=%d — stopping enumeration\n",
                           m, rc);
                    break;
                }
                printf("\nGetStructMbrInfo[%d] rc=%d (%s)\n", m, rc, bp_strerror(rc));
            } else {
                printf("\nGetStructMbrInfo[%d] response (76 bytes):\n", m);
                hexdump(ctx.resp, 76);
            }
        }
    }

    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
