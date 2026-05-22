/*
 * identity.c — query a CIP device's Identity object via the public
 *              bp_client_message_send() API.  Uses CIP service 0x01
 *              (Get_Attribute_All) on class 1 instance 1; parses the
 *              response into vendor/device-type/product-code/revision/
 *              serial/product-name.
 *
 * Usage:
 *   identity                       # LOCAL cm1756 module
 *   identity --slot 2              # backplane slot 2 (PLC)
 *   identity --slot 0              # backplane slot 0
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
        case 0x0030: return "Online Development Inc. (Rockwell EEC)";
        default:     return "(unknown)";
    }
}

static const char *device_type_name(uint16_t dt) {
    switch (dt) {
        case 0x000C: return "Communications Adapter";
        case 0x000E: return "Programmable Logic Controller";
        case 0x002B: return "Generic Device";
        default:     return "(see CIP Vol 1 Appendix B)";
    }
}

int main(int argc, char *argv[]) {
    int slot = -1;  /* -1 = local (no backplane routing) */

    static struct option opts[] = {
        {"slot", required_argument, 0, 's'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "s:", opts, &idx)) != -1) {
        if (c == 's') slot = atoi(optarg);
    }

    /* Build encoded EPATH: optional [port=1, link=slot] then class/instance. */
    uint8_t epath[16];
    uint16_t epath_len = 0;
    if (slot >= 0) {
        epath[epath_len++] = 0x01;          /* port 1 (backplane) */
        epath[epath_len++] = (uint8_t)slot; /* link addr */
    }
    epath[epath_len++] = 0x20;              /* logical 8-bit class */
    epath[epath_len++] = 0x01;              /* class 1 (Identity) */
    epath[epath_len++] = 0x24;              /* logical 8-bit instance */
    epath[epath_len++] = 0x01;              /* instance 1 */

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "client open failed\n"); return 2;
    }
    bp_client_open_session(cl, NULL);

    uint8_t respbuf[256];
    bp_message_t msg = {
        .encoded_path  = epath,
        .path_size     = epath_len,
        .service       = 0x01,       /* Get_Attribute_All */
        .class_or_misc = 0x0001,     /* Identity class */
        .resp_data     = respbuf,
        .resp_capacity = sizeof(respbuf),
    };
    int rc = bp_client_message_send(cl, &msg);
    bp_client_close(cl);

    if (slot >= 0) {
        printf("[identity] target = backplane slot %d (path %02x %02x)\n",
               slot, epath[0], epath[1]);
    } else {
        printf("[identity] target = LOCAL cm1756 (no backplane routing)\n");
    }
    printf("  rc=%d (%s)\n  resp_len=%u  status=0x%08x\n",
           rc, bp_strerror(rc), msg.resp_len, msg.status);

    if (rc != BP_OK)       return 1;
    if (msg.resp_len < 4)  { printf("  (response too short to be CIP)\n"); return 1; }

    /* CIP response header: */
    const uint8_t *r = respbuf;
    uint8_t  reply_svc      = r[0];
    /* r[1] = reserved */
    uint8_t  general_status = r[2];
    uint8_t  ext_status_sz  = r[3];
    printf("  CIP reply: service=0x%02x general_status=0x%02x ext_status_sz=%u\n",
           reply_svc, general_status, ext_status_sz);
    if (general_status != 0) {
        printf("  CIP error %s.\n",
               general_status == 0x05 ? "0x05 Path destination unknown"
             : general_status == 0x14 ? "0x14 Attribute not supported"
             : "(see CIP Vol 1 Appendix B)");
        return 1;
    }
    const uint8_t *body = r + 4 + ext_status_sz * 2;
    size_t  body_len    = msg.resp_len - (4 + ext_status_sz * 2);
    if (body_len < 14) {
        printf("  (response body too short for full Identity attrs)\n");
        return 1;
    }

    uint16_t vendor   = (uint16_t)(body[0]  | (body[1]  << 8));
    uint16_t devtype  = (uint16_t)(body[2]  | (body[3]  << 8));
    uint16_t prodcode = (uint16_t)(body[4]  | (body[5]  << 8));
    uint8_t  major    = body[6];
    uint8_t  minor    = body[7];
    uint16_t status_w = (uint16_t)(body[8]  | (body[9]  << 8));
    uint32_t serial   = (uint32_t)body[10] | ((uint32_t)body[11] << 8)
                       | ((uint32_t)body[12] << 16) | ((uint32_t)body[13] << 24);
    uint8_t  name_len = body_len > 14 ? body[14] : 0;
    const uint8_t *name = name_len ? body + 15 : NULL;
    if (name && (size_t)(15 + name_len) > body_len) name_len = (uint8_t)(body_len - 15);

    printf("\n  Vendor ID       : 0x%04x  (%s)\n", vendor, vendor_name(vendor));
    printf("  Device Type     : 0x%04x  (%s)\n", devtype, device_type_name(devtype));
    printf("  Product Code    : 0x%04x  (%u)\n",   prodcode, prodcode);
    printf("  Revision        : %u.%u\n",          major, minor);
    printf("  Status          : 0x%04x\n",         status_w);
    printf("  Serial Number   : 0x%08x  (%u)\n",   serial, serial);
    if (name) {
        printf("  Product Name    : '%.*s' (len=%u)\n", (int)name_len, name, name_len);
    } else {
        printf("  Product Name    : (none reported)\n");
    }
    return 0;
}
