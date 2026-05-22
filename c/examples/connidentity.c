/*
 * connidentity.c — query a device's Identity via class-3 connected
 *                  messaging (Phase 5).  Demonstrates that the
 *                  connected path DOES route to arbitrary backplane
 *                  slots, unlike bp_client_message_send.
 *
 * Usage:
 *   connidentity              # default: slot 2 (L85)
 *   connidentity --slot 1     # explicit
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

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
    int conn_params = 0x43E8;  /* P2P class-3, variable, 1000 bytes */
    static struct option opts[] = {
        {"slot",        required_argument, 0, 's'},
        {"conn-params", required_argument, 0, 'p'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "s:p:", opts, &idx)) != -1) {
        if      (c == 's') slot = atoi(optarg);
        else if (c == 'p') conn_params = (int)strtol(optarg, NULL, 0);
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) { fprintf(stderr, "open failed\n"); return 2; }
    bp_client_open_session(cl, NULL);

    /* Build route — port 1 (backplane), link = slot */
    uint8_t epath[2] = { 0x01, (uint8_t)slot };
    bp_conn_spec_t spec = {
        .app_handle   = 1,
        .options      = 0,
        .encoded_path = epath,
        .path_size    = 2,
        .conn_params  = (uint16_t)conn_params,
    };

    int rc;
    uint16_t conn_id = 0, conn_serial = 0;
    rc = bp_client_txrx_open(cl, &spec, &conn_id, &conn_serial);
    printf("[connidentity] slot=%d  conn_params=0x%04x  txrx_open rc=%d (%s)  "
           "conn_id=0x%04x  serial=0x%04x\n",
           slot, (uint16_t)conn_params, rc, bp_strerror(rc), conn_id, conn_serial);

    /* Try TxRxMsg whether or not OpenConn succeeded — some OEM
     * connection models auto-open on first Msg. */
    uint8_t req[]   = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };
    uint8_t resp[256];
    uint16_t resp_len = 0;
    rc = bp_client_txrx_msg(cl, &spec, req, sizeof(req),
                             resp, sizeof(resp), &resp_len);
    printf("[connidentity] txrx_msg rc=%d (%s)  resp_len=%u\n",
           rc, bp_strerror(rc), resp_len);
    if (rc == BP_OK && resp_len > 0) {
        printf("response: ");
        for (uint16_t i = 0; i < resp_len; i++) printf("%02x ", resp[i]);
        printf("\n");
        print_id(resp, resp_len);
    }

    int crc = bp_client_txrx_close(cl, &spec);
    printf("[connidentity] txrx_close rc=%d (%s)\n", crc, bp_strerror(crc));

    bp_client_close(cl);
    return 0;
}
