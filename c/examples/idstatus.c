/*
 * idstatus.c — v0.10.0 Phase C validator for bp_client_get_device_id_status.
 *
 * Sweeps slots 0..3 and prints each device's Identity status word,
 * cross-checking against the full GetDeviceIdObject.status field.
 *
 * Output is byte-identical across the three SDKs; cross-language gate
 * diffs the per-slot lines.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[idstatus] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    int hits = 0, mismatches = 0;
    for (int slot = 0; slot < 4; slot++) {
        char path[16];
        snprintf(path, sizeof(path), "P:1,S:%d", slot);

        uint16_t status_lite = 0;
        int rc_lite = bp_client_get_device_id_status(cl, path, 1, &status_lite);

        bp_id_object_t id;
        int rc_full = bp_client_get_device_id(cl, path, 1, &id);

        if (rc_lite == BP_OK && rc_full == BP_OK) {
            int match = (status_lite == id.status);
            printf("[idstatus] slot=%d  lite=0x%04x  full=0x%04x  match=%s\n",
                   slot, status_lite, id.status, match ? "YES" : "NO");
            hits++;
            if (!match) mismatches++;
        } else {
            printf("[idstatus] slot=%d  lite_rc=%d full_rc=%d  (empty or error)\n",
                   slot, rc_lite, rc_full);
        }
    }
    int pass = hits >= 1 && mismatches == 0;
    printf("[idstatus] SUMMARY hits=%d mismatches=%d\n", hits, mismatches);
    printf("[idstatus] %s\n", pass ? "PASS" : "FAIL");
    bp_client_close(cl);
    return pass ? 0 : 1;
}
