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

/* Per-device epoch table (empirical, validated 2026-05-22):
 * different PLC families use different epochs.  The L73 (slot 1)
 * uses 2000/1998; the L85 (slot 2) uses 1972/Unix.  Others TBD. */
static bp_wctime_epoch_t epoch_for(int slot, int is_utc) {
    if (slot == 2) return is_utc ? BP_WCTIME_EPOCH_UNIX : BP_WCTIME_EPOCH_1972;
    /* Default + slot 1 (L73) */
    return is_utc ? BP_WCTIME_EPOCH_1998 : BP_WCTIME_EPOCH_2000;
}

static int g_raw = 0;

static void print_wctime(const char *label, const char *path,
                          const bp_wctime_t *wc, bp_wctime_epoch_t epoch,
                          int try_tz, int try_local) {
    int64_t unix_us = bp_wctime_to_unix_us(wc, epoch);
    time_t unix_sec = (time_t)(unix_us / 1000000LL);
    struct tm tm_utc;
    gmtime_r(&unix_sec, &tm_utc);
    char buf[32];
    strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_utc);
    char tz[40] = {0};
    if (try_tz) bp_wctime_tz_name(wc, tz, sizeof(tz));
    printf("[wctime] %s %s: %s UTC  tz=\"%s\"\n", label, path, buf, tz);
    if (try_local) {
        bp_wctime_local_t loc;
        if (bp_wctime_decode_local(wc, &loc) == BP_OK) {
            printf("[wctime] %s %s: aux2-decoded local d=%u h=%u m=%u s=%u\n",
                   label, path, loc.day, loc.hour, loc.minute, loc.second);
        }
    }
    if (g_raw) {
        printf("[wctime] %s %s: raw sec=0x%016" PRIx64 " nsec=0x%016" PRIx64 "\n",
               label, path, wc->sec, wc->nsec);
        printf("[wctime] %s %s: raw aux0=0x%016" PRIx64 " aux1=0x%016" PRIx64 "\n",
               label, path, wc->aux0, wc->aux1);
        printf("[wctime] %s %s: raw aux2=0x%016" PRIx64 " aux3=0x%016" PRIx64 "\n",
               label, path, wc->aux2, wc->aux3);
    }
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--raw") == 0) g_raw = 1;
    }

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
        if (rcL == BP_OK) { print_wctime("LOCAL", path, &local, epoch_for(s, 0), 0, 1); any_ok = 1; }
        else              { printf("[wctime] LOCAL %s: rc=%d\n", path, rcL); }
        if (rcU == BP_OK) { print_wctime("UTC  ", path, &utc, epoch_for(s, 1), 1, 0); any_ok = 1; }
        else              { printf("[wctime] UTC   %s: rc=%d\n", path, rcU); }
    }
    printf("[wctime] %s\n", any_ok ? "PASS" : "FAIL");
    bp_client_close(cl);
    return any_ok ? 0 : 1;
}
