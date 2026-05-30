/*
 * structrwtest — live validator for the SDK's atomic structured tag
 *                access (bp_client_read_struct / bp_client_write_struct,
 *                CIP 0x4C/0x4D over MessageSend).  READ-ONLY unless
 *                --write, where it writes the just-read bytes back (no
 *                value change) and confirms the round-trip is
 *                byte-identical.
 *
 * Mirrors the Go validator go/ocxbp/cmd/structrwtest.  Run with the SDK
 * IPC flags (--ipc=host --pid=host -v /dev/shm:/dev/shm) on the module;
 * bpServer is single-client, so stop any gateway first.
 *
 * Usage:
 *   structrwtest [--slot N] [--tag NAME] [--write]
 *     --slot N    controller backplane slot (N in P:1,S:N); default 2
 *     --tag NAME  structured tag to access; default Tran_From_iSeries_Register
 *     --write     write the just-read bytes back and verify round-trip
 *
 * Exit codes: 0 PASS, 1 FAIL (CIP / round-trip), 2 FATAL (IPC error).
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

int main(int argc, char *argv[]) {
    int slot = 2;
    const char *tag = "Tran_From_iSeries_Register";
    int do_write = 0;

    static struct option opts[] = {
        {"slot",  required_argument, 0, 's'},
        {"tag",   required_argument, 0, 't'},
        {"write", no_argument,       0, 'w'},
        {"help",  no_argument,       0, 'h'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "s:t:wh", opts, &idx)) != -1) {
        if      (oc == 's') slot = (int)strtol(optarg, NULL, 0);
        else if (oc == 't') tag = optarg;
        else if (oc == 'w') do_write = 1;
        else if (oc == 'h') {
            printf("usage: %s [--slot N] [--tag NAME] [--write]\n"
                   "  --slot N    controller backplane slot (default 2)\n"
                   "  --tag NAME  structured tag (default Tran_From_iSeries_Register)\n"
                   "  --write     write the just-read bytes back and verify round-trip\n",
                   argv[0]);
            return 0;
        }
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "FATAL: bp_client_open failed (is bpServer running? --ipc=host set?)\n");
        return 2;
    }
    if (bp_client_open_session(cl, NULL) != BP_OK) {
        fprintf(stderr, "FATAL: bp_client_open_session failed\n");
        bp_client_close(cl);
        return 2;
    }

    uint8_t data[600];
    uint16_t data_len = 0, handle = 0;
    int rc = bp_client_read_struct(cl, (uint8_t)slot, tag,
                                   data, sizeof(data), &data_len, &handle);
    if (rc != BP_OK) {
        fprintf(stderr, "FAIL: bp_client_read_struct rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_client_close(cl);
        return rc <= BP_ERR_CLIENT_OPEN ? 2 : 1;
    }
    printf("read_struct OK: tag=\"%s\" handle=0x%04x bytes=%u\n", tag, handle, data_len);

    if (!do_write) {
        printf("VERDICT: SDK read_struct works (read-only).\n");
        bp_client_close(cl);
        return 0;
    }

    rc = bp_client_write_struct(cl, (uint8_t)slot, tag, handle, data, data_len);
    if (rc != BP_OK) {
        fprintf(stderr, "FAIL: bp_client_write_struct rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_client_close(cl);
        return rc <= BP_ERR_CLIENT_OPEN ? 2 : 1;
    }

    uint8_t back[600];
    uint16_t back_len = 0, back_handle = 0;
    rc = bp_client_read_struct(cl, (uint8_t)slot, tag,
                               back, sizeof(back), &back_len, &back_handle);
    if (rc != BP_OK) {
        fprintf(stderr, "FAIL: bp_client_read_struct (verify) rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_client_close(cl);
        return rc <= BP_ERR_CLIENT_OPEN ? 2 : 1;
    }
    if (back_len != data_len || memcmp(data, back, data_len) != 0) {
        fprintf(stderr, "FAIL: round-trip mismatch (%u vs %u bytes / contents differ)\n",
                data_len, back_len);
        bp_client_close(cl);
        return 1;
    }
    printf("VERDICT: SDK write_struct accepted (CIP 0) + round-trip byte-identical "
           "— ATOMIC UDT I/O works.\n");

    bp_client_close(cl);
    return 0;
}
