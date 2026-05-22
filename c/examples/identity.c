/*
 * identity.c — print the Identity object of a CIP device.
 *
 *   1. bp_client_get_id_local()    — LOCAL cm1756 (no path)
 *   2. bp_client_get_device_id()   — REMOTE by text path, OEM-parsed
 *      (and dumps the active node table for context)
 *
 * For per-slot Identity via raw UCMM, see msgprobe with
 * --req "01 02 20 01 24 01" (Get_Attributes_All on Identity).
 *
 * Usage:
 *   identity                    # local cm1756
 *   identity --path "P:1,S:1"   # remote via OEM-parsed text path
 *   identity --path "P:1,S:2"
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static const char *vendor_name(uint16_t id) {
    switch (id) {
        case 0x0001: return "Allen-Bradley (Rockwell)";
        case 0x0030: return "Online Development Inc.";
        default:     return "(unknown)";
    }
}

static void print_id(const bp_id_object_t *id) {
    /* product_name in OCXcip_GetIdObject responses is sometimes a
     * "SHORT_STRING"-padded buffer (one-byte length prefix, then the
     * bytes); other times the length is implicit and the buffer is
     * just zero-padded.  Print up to 31 bytes ignoring leading 0x00
     * NUL prefix bytes and trailing NULs. */
    char nm[33] = {0};
    size_t skip = 0;
    while (skip < 31 && id->product_name[skip] == 0) skip++;
    size_t take = 0;
    while (skip + take < 32 && id->product_name[skip + take] != 0
                            && take < 32) {
        nm[take] = (char)id->product_name[skip + take];
        take++;
    }
    nm[take] = 0;
    printf("  Vendor ID       : 0x%04x  (%s)\n", id->vendor_id, vendor_name(id->vendor_id));
    printf("  Device Type     : 0x%04x\n", id->device_type);
    printf("  Product Code    : 0x%04x  (%u)\n", id->product_code, id->product_code);
    printf("  Revision        : %u.%u\n", id->major_rev, id->minor_rev);
    printf("  Status          : 0x%04x\n", id->status);
    printf("  Serial Number   : 0x%08x  (%u)\n", id->serial_number, id->serial_number);
    printf("  Product Name    : '%s'\n", nm);
}

int main(int argc, char *argv[]) {
    const char *text_path = NULL;
    static struct option opts[] = {
        {"path", required_argument, 0, 'p'}, {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "p:", opts, &idx)) != -1) {
        if (c == 'p') text_path = optarg;
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "client open failed\n"); return 2;
    }
    bp_client_open_session(cl, NULL);

    /* Always print the active node table for context. */
    uint32_t mask_lo = 0, mask_hi = 0;
    if (bp_client_get_active_nodes(cl, &mask_lo, &mask_hi) == BP_OK) {
        printf("[active nodes] mask_lo=0x%08x  mask_hi=0x%08x  =>", mask_lo, mask_hi);
        for (int i = 0; i < 32; i++) if (mask_lo & (1u << i)) printf(" %d", i);
        for (int i = 0; i < 32; i++) if (mask_hi & (1u << i)) printf(" %d", i + 32);
        printf("\n\n");
    }

    if (!text_path) {
        printf("=== LOCAL Identity (bp_client_get_id_local) ===\n");
        bp_id_object_t id;
        int rc = bp_client_get_id_local(cl, &id);
        printf("  rc=%d (%s)\n", rc, bp_strerror(rc));
        if (rc == BP_OK) print_id(&id);
    } else {
        printf("=== REMOTE Identity via OCXcip_GetDeviceIdObject('%s', inst=1) ===\n", text_path);
        bp_id_object_t id;
        int rc = bp_client_get_device_id(cl, text_path, 1, &id);
        printf("  rc=%d (%s)\n", rc, bp_strerror(rc));
        if (rc == BP_OK) print_id(&id);
    }

    bp_client_close(cl);
    return 0;
}
