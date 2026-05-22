/*
 * msgprobe_routed.c — like msgprobe, but opens a TagDb handle on a
 *                     given path first.  Tests whether MessageSend
 *                     picks up its routing from the most recently
 *                     opened tagdb handle.
 *
 * Usage:
 *   msgprobe_routed --path "P:1,S:2" --service 0x01 --class 1 --epath "20 01 24 01"
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
    int service = -1;
    int class_or_misc = 0;
    const char *epath_hex = NULL;
    static struct option opts[] = {
        {"path",    required_argument, 0, 'p'},
        {"service", required_argument, 0, 's'},
        {"class",   required_argument, 0, 'c'},
        {"epath",   required_argument, 0, 'e'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "p:s:c:e:", opts, &idx)) != -1) {
        if      (oc == 'p') tagdb_path    = optarg;
        else if (oc == 's') service       = (int)strtol(optarg, NULL, 0);
        else if (oc == 'c') class_or_misc = (int)strtol(optarg, NULL, 0);
        else if (oc == 'e') epath_hex     = optarg;
    }
    if (!tagdb_path || service < 0 || !epath_hex) {
        fprintf(stderr, "Usage: --path P:1,S:N --service 0xNN --epath \"20 01 ...\" [--class NNN]\n");
        return 2;
    }
    uint8_t epath[256];
    size_t epath_len = parse_hex(epath_hex, epath, sizeof(epath));

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);

    /* Open a tagdb handle on the path — establishes routing? */
    bp_tagdb_t *db = NULL;
    int rc = bp_tagdb_open(cl, tagdb_path, &db);
    printf("[msgprobe_routed] tagdb_open('%s')  rc=%d (%s)\n", tagdb_path, rc, bp_strerror(rc));
    if (rc != BP_OK) { bp_client_close(cl); return 2; }

    uint8_t respbuf[256];
    bp_message_t msg = {
        .encoded_path  = epath,
        .path_size     = (uint16_t)epath_len,
        .service       = (uint8_t)service,
        .class_or_misc = (uint16_t)class_or_misc,
        .resp_data     = respbuf,
        .resp_capacity = sizeof(respbuf),
    };
    rc = bp_client_message_send(cl, &msg);
    printf("MessageSend  rc=%d  resp_len=%u  status=0x%08x\n",
           rc, msg.resp_len, msg.status);
    printf("response: ");
    for (uint16_t i = 0; i < msg.resp_len; i++) printf("%02x ", respbuf[i]);
    printf("\n");

    /* Parse if it looks like a standard CIP response */
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
