/*
 * msgprobe_routed.c — like msgprobe, but opens a TagDb handle on a
 *                     given path first.  Historically tested whether
 *                     MessageSend inherits routing from CreateTagDbHandle.
 *
 *                     Result: it does NOT.  MessageSend routes purely
 *                     by the `slot` byte; the tagdb handle is irrelevant
 *                     to the UCMM send path (the chip uses the same
 *                     UCMM MBOX_LOOPBACK regardless).  Kept for
 *                     regression coverage.
 *
 * Usage:
 *   msgprobe_routed --path "P:1,S:2" --slot 2 --req "01 02 20 01 24 01"
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

int main(int argc, char *argv[]) {
    const char *tagdb_path = NULL;
    int slot = -1;
    int timeout_ms = 0;
    const char *req_hex = NULL;
    static struct option opts[] = {
        {"path",       required_argument, 0, 'p'},
        {"slot",       required_argument, 0, 's'},
        {"req",        required_argument, 0, 'r'},
        {"timeout-ms", required_argument, 0, 't'},
        /* legacy aliases */
        {"service",    required_argument, 0, 's'},
        {"epath",      required_argument, 0, 'r'},
        {"class",      required_argument, 0, 't'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "p:s:r:t:", opts, &idx)) != -1) {
        if      (oc == 'p') tagdb_path = optarg;
        else if (oc == 's') slot       = (int)strtol(optarg, NULL, 0);
        else if (oc == 'r') req_hex    = optarg;
        else if (oc == 't') timeout_ms = (int)strtol(optarg, NULL, 0);
    }
    if (!tagdb_path || slot < 0 || !req_hex) {
        fprintf(stderr, "Usage: --path P:1,S:N --slot N --req \"01 02 ...\" [--timeout-ms 1000]\n");
        return 2;
    }
    uint8_t req[256];
    size_t req_len = parse_hex(req_hex, req, sizeof(req));

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);

    bp_tagdb_t *db = NULL;
    int rc = bp_tagdb_open(cl, tagdb_path, &db);
    printf("[msgprobe_routed] tagdb_open('%s')  rc=%d (%s)\n", tagdb_path, rc, bp_strerror(rc));
    if (rc != BP_OK) { bp_client_close(cl); return 2; }

    uint8_t respbuf[256];
    bp_message_t msg = {
        .slot          = (uint8_t)slot,
        .cip_request   = req,
        .req_size      = (uint16_t)req_len,
        .timeout_ms    = (uint16_t)timeout_ms,
        .resp_data     = respbuf,
        .resp_capacity = sizeof(respbuf),
    };
    rc = bp_client_message_send(cl, &msg);
    printf("MessageSend  rc=%d  resp_len=%u  status=0x%08x\n",
           rc, msg.resp_len, msg.status);
    printf("response: ");
    for (uint16_t i = 0; i < msg.resp_len; i++) printf("%02x ", respbuf[i]);
    printf("\n");

    if (msg.resp_len >= 4) {
        uint8_t  svc_reply  = respbuf[0];
        uint8_t  gen_status = respbuf[2];
        printf("  reply_svc=0x%02x  general_status=0x%02x\n", svc_reply, gen_status);
        if (gen_status == 0 && msg.resp_len >= 18) {
            uint16_t vendor   = (uint16_t)respbuf[4]  | ((uint16_t)respbuf[5]  << 8);
            uint16_t devtype  = (uint16_t)respbuf[6]  | ((uint16_t)respbuf[7]  << 8);
            uint16_t prodcode = (uint16_t)respbuf[8]  | ((uint16_t)respbuf[9]  << 8);
            uint8_t  major    = respbuf[10];
            uint8_t  minor    = respbuf[11];
            printf("  identity: vendor=0x%04x  device_type=0x%04x  product_code=0x%04x  fw=%u.%u\n",
                   vendor, devtype, prodcode, major, minor);
            if (msg.resp_len > 18) {
                uint8_t namelen = respbuf[18];
                if (namelen > 0 && msg.resp_len >= 19u + namelen) {
                    printf("  name='%.*s'\n", (int)namelen, &respbuf[19]);
                }
            }
        }
    }

    bp_tagdb_close(db);
    bp_client_close(cl);
    return 0;
}
