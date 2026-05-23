/*
 * dummy.c — v0.10.3 OCXcip_Dummy liveness probe validator.
 *
 * Calls bp_client_dummy N times (default 100), measures the average
 * round-trip latency, and asserts rc == BP_OK for every call.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "bpclient.h"

static double now_us(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

int main(int argc, char *argv[]) {
    int n = 100;
    if (argc >= 2) n = atoi(argv[1]);
    if (n <= 0) n = 100;

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[dummy] open failed\n");
        return 2;
    }

    int fail = 0;
    double t0 = now_us();
    for (int i = 0; i < n; i++) {
        int rc = bp_client_dummy(cl);
        if (rc != BP_OK) {
            fprintf(stderr, "[dummy] call %d rc=%d (%s)\n", i, rc, bp_strerror(rc));
            fail++;
        }
    }
    double dt = now_us() - t0;
    printf("[dummy] %d calls, %.0f us total, %.1f us/call, %d failures\n",
           n, dt, dt / (double)n, fail);
    printf("[dummy] %s\n", fail == 0 ? "PASS" : "FAIL");
    bp_client_close(cl);
    return fail == 0 ? 0 : 1;
}
