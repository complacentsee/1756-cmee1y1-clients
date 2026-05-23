/*
 * pathprobe.c — invoke bp_client_parse_path and dump the encoded
 *               EPATH bytes it produces.  Useful when hand-building
 *               the route_path inside a routed Unconnected_Send
 *               (svc 0x52) carried in bp_client_message_send().cip_request.
 *
 * Usage:
 *   pathprobe "P:1,S:2"
 *   pathprobe "P:1,S:1"
 *   pathprobe "1,2,3"      (Rockwell-style — likely rejected)
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

int main(int argc, char *argv[]) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s <text-path>\n", argv[0]);
        return 2;
    }
    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "client open failed\n"); return 2;
    }
    bp_client_open_session(cl, NULL);

    bp_parsed_path_t parsed;
    int rc = bp_client_parse_path(cl, argv[1], &parsed);
    bp_client_close(cl);

    printf("[pathprobe] text='%s'  rc=%d  encoded_len=%u\n",
           argv[1], rc, parsed.encoded_size);
    if (rc != BP_OK) return 1;

    printf("  out_class      = 0x%04x\n", parsed.cip_class);
    printf("  out_seg_flags  = 0x%02x\n", parsed.segment_flags);
    printf("  out_instance   = 0x%08x  (%u)\n", parsed.instance, parsed.instance);
    printf("  out_attr_flags = 0x%02x\n", parsed.attr_flags);
    printf("  encoded path   = ");
    for (uint16_t i = 0; i < parsed.encoded_size; i++)
        printf("%02x ", parsed.encoded[i]);
    printf("\n");
    return 0;
}
