/*
 * routedident.c — v0.8.0 multi-hop route validator.
 *
 * Sends Identity GetAttributesAll to a target device via
 * Unconnected_Send (service 0x52) targeting the L85's Connection
 * Manager, with the route_path describing the hops past the L85.
 *
 * Defaults exercise the simplest multi-hop case: route through the
 * L85 (slot 2) back to itself via the backplane (port 1, slot 2) —
 * the L85's ConnMgr accepts the routed request, walks back through
 * the backplane to itself, and returns its own Identity.  That
 * verifies the wire format end-to-end without needing an off-chassis
 * EtherNet/IP target.
 *
 * For a real multi-hop test, pass `--port 2 --link N` to target
 * EtherNet/IP node `N` on the L85's front Ethernet port.
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static int hex_dump(const uint8_t *p, int n) {
    for (int i = 0; i < n; i++) printf("%02x ", p[i]);
    printf("\n");
    return 0;
}

int main(int argc, char *argv[]) {
    int router_slot = 2;     /* slot hosting the L85 (the router) */
    int port = 1;            /* default hop: port 1 = backplane */
    int link = 2;            /* default link = slot 2 (back to L85) */

    static struct option opts[] = {
        {"router-slot", required_argument, 0, 'r'},
        {"port",        required_argument, 0, 'p'},
        {"link",        required_argument, 0, 'l'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "r:p:l:", opts, &idx)) != -1) {
        switch (oc) {
        case 'r': router_slot = (int)strtol(optarg, NULL, 0); break;
        case 'p': port        = (int)strtol(optarg, NULL, 0); break;
        case 'l': link        = (int)strtol(optarg, NULL, 0); break;
        }
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[routedident] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    /* Embedded: Identity GetAttributesAll on class 1 instance 1. */
    const uint8_t identity_req[] = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };
    /* Route path: one hop = {port, link}. */
    uint8_t route[8];
    size_t route_len = 0;
    if (bp_route_append_port(route, sizeof(route), &route_len,
                              (uint8_t)port, (uint8_t)link) != BP_OK) {
        fprintf(stderr, "[routedident] route encoding failed\n");
        bp_client_close(cl);
        return 1;
    }

    uint8_t wrapped[256];
    int wlen = bp_build_unconnected_send(wrapped, sizeof(wrapped),
                                          identity_req, sizeof(identity_req),
                                          route, route_len, 5000);
    if (wlen <= 0) {
        fprintf(stderr, "[routedident] build_unconnected_send rc=%d\n", wlen);
        bp_client_close(cl);
        return 1;
    }

    printf("[routedident] router_slot=%d  port=%d  link=%d  wrapped_len=%d\n",
           router_slot, port, link, wlen);
    printf("[routedident] wrapped bytes: ");
    hex_dump(wrapped, wlen);

    uint8_t resp[256];
    bp_message_t msg = {
        .slot          = (uint8_t)router_slot,
        .cip_request   = wrapped,
        .req_size      = (uint16_t)wlen,
        .timeout_ms    = 5000,
        .resp_data     = resp,
        .resp_capacity = sizeof(resp),
    };
    int rc = bp_client_message_send(cl, &msg);
    if (rc != BP_OK) {
        fprintf(stderr, "[routedident] message_send rc=%d (%s)\n",
                rc, bp_strerror(rc));
        bp_client_close(cl);
        return 1;
    }

    /* The reply is the embedded service's reply (svc 0x81 for Identity
     * GAA), wrapped in the standard CIP reply envelope. */
    printf("[routedident] resp_len=%u  bytes: ", msg.resp_len);
    hex_dump(resp, msg.resp_len);

    if (msg.resp_len < 4 || resp[0] != 0x81) {
        fprintf(stderr, "[routedident] reply not Identity GAA "
                        "(svc=0x%02x status=0x%02x)\n",
                msg.resp_len ? resp[0] : 0,
                msg.resp_len >= 3 ? resp[2] : 0xFF);
        bp_client_close(cl);
        return 1;
    }
    if (resp[2] != 0x00) {
        uint16_t ext = (msg.resp_len >= 6 && resp[3])
                        ? (uint16_t)(resp[4] | (resp[5] << 8)) : 0;
        fprintf(stderr, "[routedident] routed CIP failure: "
                        "status=0x%02x ext=0x%04x (%s)\n",
                resp[2], ext, bp_cip_status_string(resp[2], ext));
        bp_client_close(cl);
        return 1;
    }

    /* Body starts at +4 + (ext_size_words * 2); typically ext_size=0. */
    int body_off = 4 + resp[3] * 2;
    if (msg.resp_len < body_off + 14) {
        fprintf(stderr, "[routedident] body too short\n");
        bp_client_close(cl);
        return 1;
    }
    const uint8_t *body = resp + body_off;
    uint16_t vendor = body[0] | (body[1] << 8);
    uint16_t dev    = body[2] | (body[3] << 8);
    uint16_t prod   = body[4] | (body[5] << 8);
    uint8_t  major  = body[6];
    uint8_t  minor  = body[7];
    uint8_t  name_len = (msg.resp_len > body_off + 14) ? body[14] : 0;
    if (body_off + 15 + name_len > msg.resp_len) name_len = 0;

    printf("[routedident] Identity: Vendor=0x%04x DevType=0x%04x "
           "Product=0x%04x fw=%u.%u Name='%.*s'\n",
           vendor, dev, prod, major, minor, (int)name_len, body + 15);
    printf("[routedident] PASS\n");

    bp_client_close(cl);
    return 0;
}
