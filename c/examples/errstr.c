/*
 * errstr.c — v0.10.0 Phase A validator for bp_client_error_string.
 *
 * Dispatches OCXcip_ErrorString for a battery of known codes and
 * prints the engine's description alongside the SDK's local
 * bp_strerror table.  Useful for surfacing engine-internal codes
 * we haven't characterized (the leak-discovery code 8, etc.).
 *
 * Output is byte-identical (modulo dt= timings) across the three
 * SDKs; cross-language gate diffs the per-code lines.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

static const int32_t CODES[] = {
    0, -1, -200, -201, -300, -301, -303, -305, -311, -400,
    -101802, -103001,
    1, 3, 8, 14, 0x14, 0x15,
};
#define N_CODES (sizeof(CODES) / sizeof(CODES[0]))

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[errstr] open failed\n");
        return 2;
    }
    if (bp_client_open_session(cl, NULL) != BP_OK) {
        fprintf(stderr, "[errstr] open_session failed\n");
        bp_client_close(cl);
        return 2;
    }

    /* The engine's lookup table only contains POSITIVE engine codes;
     * negative OCX_ERR_* are wrapper-side and the engine returns
     * rc=1 ("Bad parameter") for them.  That's by design — PASS
     * just verifies the dispatch path works on the codes the engine
     * does know. */
    int engine_hits = 0;
    for (size_t i = 0; i < N_CODES; i++) {
        char engine[79] = {0};
        int rc = bp_client_error_string(cl, CODES[i], engine);
        const char *local = bp_strerror((int)CODES[i]);
        if (rc == BP_OK) {
            printf("[errstr] code=%-8d local=\"%s\"  engine=\"%s\"\n",
                   CODES[i], local, engine);
            if (engine[0]) engine_hits++;
        } else if (rc == 1) {
            /* Engine doesn't have this code — expected for wrapper-side
             * negative codes.  Display the local string only. */
            printf("[errstr] code=%-8d local=\"%s\"  engine=<not in table>\n",
                   CODES[i], local);
        } else {
            printf("[errstr] code=%-8d local=\"%s\"  engine=<rc=%d>\n",
                   CODES[i], local, rc);
        }
    }
    int pass = engine_hits >= 1;   /* at least one positive code translated */
    printf("[errstr] engine table hits: %d/%zu\n", engine_hits, N_CODES);
    printf("[errstr] %s\n", pass ? "PASS" : "FAIL");
    bp_client_close(cl);
    return pass ? 0 : 1;
}
