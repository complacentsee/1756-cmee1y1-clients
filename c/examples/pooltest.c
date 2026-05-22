/*
 * pooltest.c — v0.8.0 connection-pool validator.
 *
 * Opens a pool of N class-3 connections to a slot (default 2 = L85)
 * with keepalive enabled, fires K Identity GetAttributesAll requests
 * from M concurrent threads, closes the pool, prints PASS/FAIL.
 *
 * Output is byte-identical (modulo dt= timings and thread interleave
 * ordering) to go/cmd/pooltest and python/examples/pooltest.py — the
 * cross-language gate is the three diffs of the summary line.
 *
 * --keepalive-test sleeps for 30 s after the initial fan-out so the
 * keepalive thread has time to fire on idle entries (verify with
 * `[pool keepalive] ...` lines on stderr).  Useful manually; the
 * cross-language test runner skips this flag.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "bpclient.h"

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1.0e6;
}

static const uint8_t IDENTITY_REQ[] = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };

static int validate_identity_reply(const uint8_t *resp, uint16_t len) {
    return (len >= 4 && resp[0] == 0x81 && resp[2] == 0x00);
}

struct worker_args {
    bp_client_t       *cl;
    int                slot;
    int                requests;
    int                worker_id;
    _Atomic int        success;
    _Atomic int        failed;
};

static void *worker_thread(void *arg) {
    struct worker_args *w = (struct worker_args *)arg;
    for (int i = 0; i < w->requests; i++) {
        uint8_t resp[256];
        uint16_t got = 0;
        int rc = bp_client_pool_txrx(w->cl, (uint8_t)w->slot,
                                       IDENTITY_REQ, sizeof(IDENTITY_REQ),
                                       resp, sizeof(resp), &got);
        if (rc == BP_OK && validate_identity_reply(resp, got)) {
            atomic_fetch_add(&w->success, 1);
        } else {
            atomic_fetch_add(&w->failed, 1);
            fprintf(stderr, "[pooltest] worker=%d req[%d] rc=%d (%s)\n",
                    w->worker_id, i, rc, bp_strerror(rc));
        }
    }
    return NULL;
}

int main(int argc, char *argv[]) {
    int slot = 2;
    int pool_size = 4;
    int workers = 8;
    int requests = 25;        /* per worker — total = workers * requests */
    int keepalive_ms = 10000;
    int keepalive_test = 0;

    static struct option opts[] = {
        {"slot",            required_argument, 0, 's'},
        {"size",            required_argument, 0, 'z'},
        {"workers",         required_argument, 0, 'w'},
        {"requests",        required_argument, 0, 'n'},
        {"keepalive-ms",    required_argument, 0, 'k'},
        {"keepalive-test",  no_argument,       0, 'K'},
        {0,0,0,0}
    };
    int oc, idx;
    while ((oc = getopt_long(argc, argv, "s:z:w:n:k:K", opts, &idx)) != -1) {
        switch (oc) {
        case 's': slot         = (int)strtol(optarg, NULL, 0); break;
        case 'z': pool_size    = (int)strtol(optarg, NULL, 0); break;
        case 'w': workers      = (int)strtol(optarg, NULL, 0); break;
        case 'n': requests     = (int)strtol(optarg, NULL, 0); break;
        case 'k': keepalive_ms = (int)strtol(optarg, NULL, 0); break;
        case 'K': keepalive_test = 1; break;
        }
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "[pooltest] open failed\n");
        return 2;
    }
    bp_client_open_session(cl, NULL);

    printf("[pooltest] slot=%d size=%d workers=%d requests/worker=%d keepalive_ms=%d\n",
           slot, pool_size, workers, requests, keepalive_ms);

    bp_pool_spec_t spec = {
        .slot         = (uint8_t)slot,
        .size         = (uint8_t)pool_size,
        .keepalive_ms = (uint16_t)keepalive_ms,
        .conn_params  = 0,
    };
    double t_open0 = now_ms();
    int rc = bp_client_pool_open(cl, &spec);
    double t_open1 = now_ms();
    if (rc != BP_OK) {
        fprintf(stderr, "[pooltest] pool_open rc=%d (%s)\n", rc, bp_strerror(rc));
        bp_client_close(cl);
        return 1;
    }
    printf("[pooltest] pool_open  dt=%.2fms\n", t_open1 - t_open0);

    struct worker_args *args = calloc((size_t)workers, sizeof(*args));
    pthread_t *threads = calloc((size_t)workers, sizeof(*threads));
    for (int w = 0; w < workers; w++) {
        args[w].cl        = cl;
        args[w].slot      = slot;
        args[w].requests  = requests;
        args[w].worker_id = w;
        atomic_store(&args[w].success, 0);
        atomic_store(&args[w].failed,  0);
    }

    double t0 = now_ms();
    for (int w = 0; w < workers; w++) {
        pthread_create(&threads[w], NULL, worker_thread, &args[w]);
    }
    for (int w = 0; w < workers; w++) {
        pthread_join(threads[w], NULL);
    }
    double t1 = now_ms();

    int total_success = 0, total_failed = 0;
    for (int w = 0; w < workers; w++) {
        total_success += atomic_load(&args[w].success);
        total_failed  += atomic_load(&args[w].failed);
    }
    int total = workers * requests;
    printf("[pooltest] fanout %d workers × %d req = %d total  dt=%.2fms  "
           "(%.0f req/s)  success=%d failed=%d\n",
           workers, requests, total, t1 - t0,
           (double)total / ((t1 - t0) / 1000.0),
           total_success, total_failed);

    if (keepalive_test) {
        printf("[pooltest] keepalive test: sleeping 30 s — watch stderr for ping lines\n");
        sleep(30);
    }

    double t_close0 = now_ms();
    int crc = bp_client_pool_close(cl, (uint8_t)slot);
    double t_close1 = now_ms();
    printf("[pooltest] pool_close dt=%.2fms rc=%d\n", t_close1 - t_close0, crc);

    int pass = (total_success == total) && (crc == BP_OK);
    printf("[pooltest] %s\n", pass ? "PASS" : "FAIL");

    free(args); free(threads);
    bp_client_close(cl);
    return pass ? 0 : 1;
}
