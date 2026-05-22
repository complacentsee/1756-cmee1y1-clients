/*
 * udtprobe.c — exercise different access patterns against a UDT
 *              instance, and against arrays-of-UDT.
 *
 * Three patterns to evaluate:
 *
 *   (A) Raw bytes via SINT array: elem_byte_size=1, elem_count=struct_bytes.
 *       Universal — works for any structured tag the engine can address.
 *
 *   (B) Struct elements via the enumerated data_type / field1:
 *       elem_byte_size=field1, elem_count=1 (or N for arrays).
 *       Lets you read multiple struct elements in one round-trip.
 *
 *   (C) Member dot-notation: tag_name = "MyUDT.MemberName" with the
 *       member's atomic CIP type.  Resolved by the engine.
 *
 * Usage:
 *   udtprobe --path P:1,S:2 --tag Tran_To_iSeries_FIFO_Loader
 *            [--member SomeField --member-type DINT]
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static void hexdump(const uint8_t *buf, size_t len, size_t max) {
    if (len > max) len = max;
    for (size_t i = 0; i < len; i += 16) {
        printf("    +%04zx ", i);
        size_t line = (len - i) > 16 ? 16 : (len - i);
        for (size_t j = 0; j < line; j++) printf("%02x ", buf[i + j]);
        for (size_t j = line; j < 16; j++) printf("   ");
        printf(" ");
        for (size_t j = 0; j < line; j++) {
            unsigned char b = buf[i + j];
            putchar((b >= 0x20 && b < 0x7f) ? (char)b : '.');
        }
        putchar('\n');
    }
    if (len == max) printf("    ... (truncated)\n");
}

int main(int argc, char *argv[]) {
    const char *path = "P:1,S:2";
    const char *tag = "Tran_To_iSeries_FIFO_Loader";
    const char *array_tag = NULL;
    const char *member = NULL;

    static struct option opts[] = {
        {"path", required_argument, 0, 'p'},
        {"tag",  required_argument, 0, 't'},
        {"array-tag", required_argument, 0, 'a'},
        {"member", required_argument, 0, 'm'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:t:a:m:", opts, &idx)) != -1) {
        if (c == 'p') path = optarg;
        else if (c == 't') tag = optarg;
        else if (c == 'a') array_tag = optarg;
        else if (c == 'm') member = optarg;
    }

    bp_client_t *cl;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    if (bp_tagdb_open(cl, path, &db) != BP_OK) { bp_client_close(cl); return 2; }
    uint16_t n;
    bp_tagdb_build(db, &n);
    printf("[udtprobe] path=%s symbols=%u tag=%s\n\n", path, n, tag);

    /* Look up the symbol info first to know the byte size */
    uint32_t struct_bytes = 0;
    uint16_t struct_type = 0;
    int found = 0;
    for (uint16_t i = 0; i < n; i++) {
        bp_symbol_info_t info;
        if (bp_tagdb_symbol_at(db, i, &info) != BP_OK) continue;
        if (strcmp(info.name, tag) == 0) {
            found = 1;
            struct_bytes = info.field1;
            struct_type  = info.struct_type;
            printf("  symbol:  name=%s  data_type=0x%04x  struct_type=0x%04x  "
                   "field1=%u  dim=%u  arr=%d  udt=%d\n",
                   info.name, info.data_type, info.struct_type,
                   info.field1, info.field2,
                   bp_symbol_is_array(&info), bp_symbol_is_struct(&info));
            break;
        }
    }
    if (!found) { printf("  tag not found in enumeration\n");
                  bp_tagdb_close(db); bp_client_close(cl); return 2; }
    if (struct_bytes == 0) { printf("  field1 zero — not a structured tag?\n");
                  bp_tagdb_close(db); bp_client_close(cl); return 2; }

    /* (A) Read whole UDT as SINT bytes */
    printf("\n[A] Read as SINT array, %u bytes:\n", struct_bytes);
    int8_t *raw = malloc(struct_bytes);
    bp_tag_request_t ra = {
        .tag_name = tag, .data_type = BP_TYPE_SINT,
        .elem_byte_size = 1, .action = BP_ACTION_READ,
        .elem_count = (uint16_t)struct_bytes,
        .data = raw, .result = 0,
    };
    int rc = bp_tagdb_access(db, &ra, 1);
    printf("  rc=%d (%s)  result=0x%x\n", rc, bp_strerror(rc), ra.result);
    if (rc == BP_OK && ra.result == 0) hexdump((uint8_t *)raw, struct_bytes, 256);
    free(raw);

    /* (B) Read using the enumerated struct data_type */
    printf("\n[B] Read using data_type=0x%04x, elem_byte_size=%u, count=1:\n",
           0x4527, struct_bytes);
    int8_t *raw2 = malloc(struct_bytes);
    /* try with the struct's data_type from the symbol */
    bp_tag_request_t rb = {
        .tag_name = tag, .data_type = 0x4527,
        .elem_byte_size = (uint16_t)struct_bytes,
        .action = BP_ACTION_READ, .elem_count = 1,
        .data = raw2, .result = 0,
    };
    rc = bp_tagdb_access(db, &rb, 1);
    printf("  rc=%d (%s)  result=0x%x\n", rc, bp_strerror(rc), rb.result);
    if (rc == BP_OK && rb.result == 0) hexdump((uint8_t *)raw2, struct_bytes, 256);
    free(raw2);

    /* (C) Member dot-notation, if provided */
    if (member) {
        char full[260];
        snprintf(full, sizeof(full), "%s.%s", tag, member);
        printf("\n[C] Member dot-notation: read '%s' as DINT:\n", full);
        int32_t v = 0;
        rc = bp_tagdb_read_dint(db, full, &v);
        printf("  rc=%d (%s)  value=%d (0x%08x)\n",
               rc, bp_strerror(rc), v, (uint32_t)v);
    }

    /* If an array_tag was given, also try reading first 2 elements */
    if (array_tag) {
        printf("\n[D] Array case: read first 2 elements of %s\n", array_tag);
        /* Try as SINT[struct_bytes * 2] */
        size_t bytes2 = (size_t)struct_bytes * 2;
        int8_t *raw3 = malloc(bytes2);
        bp_tag_request_t rd = {
            .tag_name = array_tag, .data_type = BP_TYPE_SINT,
            .elem_byte_size = 1, .action = BP_ACTION_READ,
            .elem_count = (uint16_t)bytes2,
            .data = raw3, .result = 0,
        };
        rc = bp_tagdb_access(db, &rd, 1);
        printf("  SINT[%zu]: rc=%d result=0x%x\n", bytes2, rc, rd.result);
        if (rc == BP_OK && rd.result == 0) hexdump((uint8_t *)raw3, bytes2, 256);
        free(raw3);

        /* Also try data_type=0x4527, elem_byte_size=struct_bytes, count=2 */
        int8_t *raw4 = malloc(bytes2);
        bp_tag_request_t re = {
            .tag_name = array_tag, .data_type = 0x4527,
            .elem_byte_size = (uint16_t)struct_bytes,
            .action = BP_ACTION_READ, .elem_count = 2,
            .data = raw4, .result = 0,
        };
        rc = bp_tagdb_access(db, &re, 1);
        printf("  STRUCT count=2: rc=%d result=0x%x\n", rc, re.result);
        if (rc == BP_OK && re.result == 0) hexdump((uint8_t *)raw4, bytes2, 256);
        free(raw4);
    }

    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
