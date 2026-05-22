/*
 * connidentity.c — query a device's Identity via class-3 connected
 *                  messaging.  UCMM equivalent is
 *                  `msgprobe --slot N --req "01 02 20 01 24 01"`.
 *
 * Drives bp_client_txrx_open + bp_client_txrx_msg + bp_client_txrx_close
 * to validate the v0.7.0 connected-messaging surface end-to-end on a
 * real PLC.  Compare against `connexp` (v0.7.0 Phase 1 experimental
 * harness, removed after Phase 2) for the equivalent flow built on
 * raw bp_client_message_send calls.
 *
 * Usage:
 *   connidentity              # default: slot 2 (L85), path 01 02
 *   connidentity --slot 1     # explicit
 *
 * SPDX-License-Identifier: MIT
 */
#include <ctype.h>
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static size_t parse_hex(const char *s, uint8_t *out, size_t cap) {
    size_t n = 0;
    while (*s && n < cap) {
        while (*s && !isxdigit((unsigned char)*s)) s++;
        if (!*s) break;
        unsigned v;
        if (sscanf(s, "%2x", &v) != 1) break;
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void print_id(const uint8_t *resp, uint16_t resp_len) {
    if (resp_len < 4) { printf("  (response too short)\n"); return; }
    uint8_t  reply_svc = resp[0];
    uint8_t  status    = resp[2];
    uint8_t  ext_size  = resp[3];
    printf("  CIP reply: service=0x%02x  general_status=0x%02x  ext_status_sz=%u\n",
           reply_svc, status, ext_size);
    if (status != 0) {
        printf("  CIP error.\n");
        return;
    }
    const uint8_t *body = resp + 4 + ext_size * 2;
    size_t body_len     = resp_len - (4 + ext_size * 2);
    if (body_len < 14) { printf("  (body too short for Identity)\n"); return; }
    uint16_t vendor   = body[0]  | ((uint16_t)body[1]  << 8);
    uint16_t devtype  = body[2]  | ((uint16_t)body[3]  << 8);
    uint16_t prodcode = body[4]  | ((uint16_t)body[5]  << 8);
    uint8_t  major    = body[6];
    uint8_t  minor    = body[7];
    uint32_t serial   = (uint32_t)body[10] | ((uint32_t)body[11] << 8)
                       | ((uint32_t)body[12] << 16) | ((uint32_t)body[13] << 24);
    uint8_t  name_len = body_len > 14 ? body[14] : 0;
    if (15u + name_len > body_len) name_len = (uint8_t)(body_len - 15);
    printf("  Vendor=0x%04x  DevType=0x%04x  ProductCode=0x%04x  fw=%u.%u  serial=0x%08x\n",
           vendor, devtype, prodcode, major, minor, serial);
    if (name_len)
        printf("  Name='%.*s'\n", (int)name_len, body + 15);
}

int main(int argc, char *argv[]) {
    int slot = 2;
    int conn_params = 0;       /* 0 → SDK default (4000-byte OT/TO size). */
    int app_handle  = 1;
    const char *path_hex = NULL;
    static struct option opts[] = {
        {"slot",        required_argument, 0, 's'},
        {"conn-params", required_argument, 0, 'p'},
        {"path",        required_argument, 0, 'P'},
        {"app-handle",  required_argument, 0, 'a'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "s:p:P:a:", opts, &idx)) != -1) {
        if      (c == 's') slot = atoi(optarg);
        else if (c == 'p') conn_params = (int)strtol(optarg, NULL, 0);
        else if (c == 'P') path_hex = optarg;
        else if (c == 'a') app_handle = (int)strtol(optarg, NULL, 0);
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) { fprintf(stderr, "open failed\n"); return 2; }
    bp_client_open_session(cl, NULL);

    /* Build route — default: port 1 (backplane), link = slot.
     * --path overrides with arbitrary hex bytes. */
    uint8_t epath[256];
    uint16_t epath_len;
    if (path_hex) {
        epath_len = (uint16_t)parse_hex(path_hex, epath, sizeof(epath));
    } else {
        epath[0] = 0x01;
        epath[1] = (uint8_t)slot;
        epath_len = 2;
    }
    bp_conn_spec_t spec = {
        .app_handle   = (uint16_t)app_handle,
        .options      = 0,
        .encoded_path = epath,
        .path_size    = epath_len,
        .conn_params  = (uint16_t)conn_params,
    };
    printf("[connidentity] app_handle=%d  slot=%d  conn_params=0x%04x  path=",
           app_handle, slot, (uint16_t)conn_params);
    for (uint16_t i = 0; i < epath_len; i++) printf("%02x ", epath[i]);
    printf("(%u bytes)\n", epath_len);

    int rc;
    uint16_t conn_id = 0, conn_serial = 0;
    rc = bp_client_txrx_open(cl, &spec, &conn_id, &conn_serial);
    printf("[connidentity] txrx_open rc=%d (%s)  conn_id=0x%04x  serial=0x%04x\n",
           rc, bp_strerror(rc), conn_id, conn_serial);
    if (rc != BP_OK) {
        bp_client_close(cl);
        return 1;
    }

    uint8_t req[]   = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };
    uint8_t resp[256];
    uint16_t resp_len = 0;
    rc = bp_client_txrx_msg(cl, &spec, req, sizeof(req),
                             resp, sizeof(resp), &resp_len);
    printf("[connidentity] txrx_msg rc=%d (%s)  resp_len=%u\n",
           rc, bp_strerror(rc), resp_len);
    int ok = 0;
    if (rc == BP_OK && resp_len > 0) {
        printf("response: ");
        for (uint16_t i = 0; i < resp_len; i++) printf("%02x ", resp[i]);
        printf("\n");
        print_id(resp, resp_len);
        ok = (resp_len >= 4 && resp[0] == 0x81 && resp[2] == 0x00);
    }

    int crc = bp_client_txrx_close(cl, &spec);
    printf("[connidentity] txrx_close rc=%d (%s)\n", crc, bp_strerror(crc));

    bp_client_close(cl);
    return ok ? 0 : 1;
}
