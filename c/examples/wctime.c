/*
 * wctime.c — v0.10.0 Phase E live validator.
 *
 * Sweeps slots 1..3 and prints each device's wall-clock object.
 * Both local-time (GetWCTime) and UTC (GetWCTimeUTC) variants.
 *
 * SPDX-License-Identifier: MIT
 */
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "bpclient.h"

static void print_wctime(const char *label, const char *path,
                          const bp_wctime_t *wc) {
    time_t sec = (time_t)wc->sec;
    struct tm tm_utc;
    gmtime_r(&sec, &tm_utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    printf("[wctime] %s %s: sec=%" PRIu64 " nsec=%" PRIu64 " -> %s "
           "aux=(0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ",0x%" PRIx64 ")\n",
           label, path, wc->sec, wc->nsec, buf,
           wc->aux0, wc->aux1, wc->aux2, wc->aux3);
}

int main(int argc, char *argv[]) {
    (void)argc; (void)argv;

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[wctime] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    int any_ok = 0;
    for (int s = 1; s <= 3; s++) {
        char path[16];
        snprintf(path, sizeof(path), "P:1,S:%d", s);
        bp_wctime_t local, utc;
        int rcL = bp_client_get_wctime(cl, path, 1, &local);
        int rcU = bp_client_get_wctime_utc(cl, path, 1, &utc);
        if (rcL == BP_OK) { print_wctime("LOCAL", path, &local); any_ok = 1; }
        else              { printf("[wctime] LOCAL %s: rc=%d\n", path, rcL); }
        if (rcU == BP_OK) { print_wctime("UTC  ", path, &utc); any_ok = 1; }
        else              { printf("[wctime] UTC   %s: rc=%d\n", path, rcU); }
    }
    printf("[wctime] %s\n", any_ok ? "PASS" : "FAIL");
    bp_client_close(cl);
    return any_ok ? 0 : 1;
}
