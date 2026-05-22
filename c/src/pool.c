/*
 * pool.c — class-3 connection pool with idle keepalive (v0.8.0 Phase 2).
 *
 * Public API: bp_client_pool_open / pool_txrx / pool_close.
 *
 * Lifecycle:
 *   pool_open(slot, size, keepalive_ms)
 *       → opens N class-3 conns via bp_client_txrx_open (each gets an
 *         internal app_handle in the 0x8000..0xFFFF range so it can't
 *         collide with caller-managed TxRx connections).
 *       → spawns a single keepalive thread (if keepalive_ms > 0) that
 *         pings Identity GetAttributesAll on entries idle ≥
 *         keepalive_ms.  Mirrors the sibling apex2d daemon's
 *         slot_pool_keepalive_idle (apex2_cip_connection.c:3287).
 *
 *   pool_txrx(slot, req)
 *       → acquires the next free pool entry (round-robin), blocking
 *         on a condvar if all are in flight, then routes the request
 *         through bp_client_txrx_msg using the entry's internal
 *         app_handle.  Releases the entry on return and signals the
 *         condvar so any waiters can proceed.
 *
 *   pool_close(slot)
 *       → stops the keepalive thread, sends Forward_Close on every
 *         entry, frees the state.  Blocks until in-flight calls have
 *         returned their entries.
 *
 * Concurrency model: per-pool pthread_mutex + pthread_cond_t.  Acquire
 * spins for an entry with in_use=0; if none free, cond_wait on the
 * pool's condvar.  Release sets in_use=0 and signals.  The keepalive
 * thread acquires under the same mutex but only when an entry is idle
 * AND free, so it never blocks pool_txrx.
 *
 * SPDX-License-Identifier: MIT
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "../include/bpclient.h"
#include "proto.h"

/* Identity Object GetAttributeAll on class 0x01 instance 1.  Same
 * bytes the sibling apex2d daemon uses for its slot_pool_keepalive
 * pings (apex2_cip_connection.c:3301-3303). */
static const uint8_t POOL_KEEPALIVE_REQ[] = { 0x01, 0x02, 0x20, 0x01, 0x24, 0x01 };

/* ────────── helpers ─────────────────────────────────────────── */

static uint16_t pool_app_handle(uint8_t slot, int index) {
    return (uint16_t)(BP_POOL_APP_HANDLE_BASE | ((uint16_t)slot << 8) | (uint16_t)index);
}

/* Compute a deadline `delta_ms` in the future for cond_timedwait. */
static void deadline_add_ms(struct timespec *ts, long delta_ms) {
    clock_gettime(CLOCK_REALTIME, ts);
    ts->tv_sec  += delta_ms / 1000;
    ts->tv_nsec += (delta_ms % 1000) * 1000000L;
    if (ts->tv_nsec >= 1000000000L) {
        ts->tv_sec  += 1;
        ts->tv_nsec -= 1000000000L;
    }
}

/* ────────── keepalive thread ────────────────────────────────── */

static void *pool_keepalive_loop(void *arg);

/* ────────── bp_client_pool_open ─────────────────────────────── */

int bp_client_pool_open(bp_client_t *cl, const bp_pool_spec_t *spec) {
    if (!cl || !spec) return BP_ERR_NULL_ARG;
    if (spec->slot > BP_MSG_MAX_SLOT) return BP_ERR_SLOT_TOO_LARGE;
    if (spec->size < 1 || spec->size > BP_POOL_MAX_SIZE) return BP_ERR_PARAM_RANGE;

    pthread_mutex_lock(&cl->pools_mu);
    struct bp_pool *pool = &cl->pools[spec->slot];
    if (pool->initialized) {
        pthread_mutex_unlock(&cl->pools_mu);
        fprintf(stderr, "[bp_client_pool_open] slot=%u already has a pool open "
                        "(call pool_close first)\n", spec->slot);
        return BP_ERR_GENERIC;
    }

    /* Mark the slot reserved while we open the conns; we hold pools_mu
     * during the whole open so another open(same slot) blocks. */
    pool->slot         = spec->slot;
    pool->size         = spec->size;
    pool->keepalive_ms = spec->keepalive_ms;
    pool->conn_params  = spec->conn_params;
    pool->client       = cl;
    pthread_mutex_init(&pool->mu, NULL);
    pthread_cond_init(&pool->cv, NULL);
    for (int i = 0; i < BP_POOL_MAX_SIZE; i++) {
        pool->entries[i].in_use     = 0;
        pool->entries[i].app_handle = 0;
        pool->entries[i].last_used  = 0;
        pool->entries[i].dead       = 0;
        pool->entries[i].last_reopen_attempt = 0;
        pool->entries[i].reopen_backoff_ms   = 0;
    }
    pool->ka_active = 0;
    pool->ka_stop   = 0;

    uint8_t epath[2] = { 0x01, spec->slot };
    time_t now = time(NULL);
    int opened = 0;
    int last_rc = BP_OK;

    for (int i = 0; i < spec->size; i++) {
        bp_conn_spec_t cs = {
            .app_handle   = pool_app_handle(spec->slot, i),
            .options      = 0,
            .encoded_path = epath,
            .path_size    = 2,
            .conn_params  = spec->conn_params,
        };
        int rc = bp_client_txrx_open(cl, &cs, NULL, NULL);
        if (rc != BP_OK) {
            fprintf(stderr, "[bp_client_pool_open] slot=%u entry %d/%u open failed: rc=%d (%s)\n",
                    spec->slot, i, spec->size, rc, bp_strerror(rc));
            last_rc = rc;
            break;
        }
        pool->entries[i].app_handle = cs.app_handle;
        pool->entries[i].last_used  = now;
        opened++;
    }

    if (opened != spec->size) {
        /* Roll back partial open. */
        for (int i = 0; i < opened; i++) {
            bp_conn_spec_t cs = {
                .app_handle   = pool->entries[i].app_handle,
                .options      = 0,
                .encoded_path = epath,
                .path_size    = 2,
                .conn_params  = spec->conn_params,
            };
            (void)bp_client_txrx_close(cl, &cs);
        }
        pthread_cond_destroy(&pool->cv);
        pthread_mutex_destroy(&pool->mu);
        memset(pool, 0, sizeof(*pool));
        pthread_mutex_unlock(&cl->pools_mu);
        return last_rc;
    }

    pool->initialized = 1;

    if (pool->keepalive_ms > 0) {
        int trc = pthread_create(&pool->ka_thread, NULL, pool_keepalive_loop, pool);
        if (trc != 0) {
            fprintf(stderr, "[bp_client_pool_open] slot=%u keepalive pthread_create rc=%d "
                            "(continuing without keepalive)\n", spec->slot, trc);
            pool->keepalive_ms = 0;  /* visible to callers in pool->keepalive_ms */
        } else {
            pool->ka_active = 1;
        }
    }

    pthread_mutex_unlock(&cl->pools_mu);
    return BP_OK;
}

/* ────────── pool entry acquire / release ────────────────────── */

/* Find a free entry; mark it in_use and return its index.  Returns -1
 * if every entry is in_use.  Caller must hold pool->mu.
 *
 * Round-robin: we remember the next-to-try index in a static (which
 * makes the pool weakly fair without adding more state to the
 * structure).  Two pools in different slots use the same counter,
 * which biases the start point slightly — fine for fairness in
 * practice. */
static int pool_acquire_locked(struct bp_pool *pool) {
    static unsigned rr_seed = 0;
    int start = (int)(rr_seed++ % pool->size);
    for (int step = 0; step < pool->size; step++) {
        int i = (start + step) % pool->size;
        struct bp_pool_entry *e = &pool->entries[i];
        if (!e->in_use && !e->dead) {
            e->in_use = 1;
            return i;
        }
    }
    return -1;
}

/* ────────── bp_client_pool_txrx ─────────────────────────────── */

int bp_client_pool_txrx(bp_client_t *cl, uint8_t slot,
                         const void *req, uint16_t req_size,
                         void *resp, uint16_t resp_capacity,
                         uint16_t *out_resp_size) {
    if (!cl || !req || !resp || req_size == 0 || resp_capacity == 0)
        return BP_ERR_NULL_ARG;
    if (slot > BP_MSG_MAX_SLOT) return BP_ERR_SLOT_TOO_LARGE;

    struct bp_pool *pool = &cl->pools[slot];

    pthread_mutex_lock(&pool->mu);
    if (!pool->initialized) {
        pthread_mutex_unlock(&pool->mu);
        return BP_ERR_NOT_OPEN;
    }
    int idx;
    while ((idx = pool_acquire_locked(pool)) < 0) {
        pthread_cond_wait(&pool->cv, &pool->mu);
        if (!pool->initialized) {
            pthread_mutex_unlock(&pool->mu);
            return BP_ERR_NOT_OPEN;
        }
    }
    uint16_t app_handle = pool->entries[idx].app_handle;
    pthread_mutex_unlock(&pool->mu);

    uint8_t epath[2] = { 0x01, slot };
    bp_conn_spec_t cs = {
        .app_handle   = app_handle,
        .options      = 0,
        .encoded_path = epath,
        .path_size    = 2,
        .conn_params  = pool->conn_params,
    };
    int rc = bp_client_txrx_msg(cl, &cs, req, req_size,
                                  resp, resp_capacity, out_resp_size);

    pthread_mutex_lock(&pool->mu);
    pool->entries[idx].in_use    = 0;
    pool->entries[idx].last_used = time(NULL);
    pthread_cond_signal(&pool->cv);
    pthread_mutex_unlock(&pool->mu);

    return rc;
}

/* ────────── keepalive thread ────────────────────────────────── */

static void *pool_keepalive_loop(void *arg) {
    struct bp_pool *pool = (struct bp_pool *)arg;
    bp_client_t    *cl   = pool->client;

    long interval_ms = pool->keepalive_ms / 2;
    if (interval_ms < 500) interval_ms = 500;     /* don't spin */
    if (interval_ms > 5000) interval_ms = 5000;   /* check at least every 5 s */

    while (1) {
        struct timespec deadline;
        deadline_add_ms(&deadline, interval_ms);
        pthread_mutex_lock(&pool->mu);
        while (!pool->ka_stop) {
            int rc = pthread_cond_timedwait(&pool->cv, &pool->mu, &deadline);
            if (rc == ETIMEDOUT) break;
            /* Otherwise we got a normal signal — loop and re-check. */
        }
        int stop = pool->ka_stop;
        pthread_mutex_unlock(&pool->mu);
        if (stop) break;

        /* Ping any entry idle ≥ keepalive_ms.  We don't hold pool->mu
         * during the network round-trip; we set in_use=1 first, then
         * release the mutex, do the round-trip, then re-acquire to
         * mark the entry free. */
        time_t now = time(NULL);
        for (int i = 0; i < pool->size; i++) {
            pthread_mutex_lock(&pool->mu);
            if (pool->ka_stop) { pthread_mutex_unlock(&pool->mu); goto out; }
            struct bp_pool_entry *e = &pool->entries[i];
            long idle_ms = (long)(now - e->last_used) * 1000;
            if (e->in_use || e->dead || idle_ms < pool->keepalive_ms) {
                pthread_mutex_unlock(&pool->mu);
                continue;
            }
            e->in_use = 1;
            uint16_t app_handle = e->app_handle;
            pthread_mutex_unlock(&pool->mu);

            uint8_t epath[2] = { 0x01, pool->slot };
            bp_conn_spec_t cs = {
                .app_handle   = app_handle,
                .options      = 0,
                .encoded_path = epath,
                .path_size    = 2,
                .conn_params  = pool->conn_params,
            };
            uint8_t resp[64];
            uint16_t got = 0;
            int prc = bp_client_txrx_msg(cl, &cs,
                                          POOL_KEEPALIVE_REQ,
                                          (uint16_t)sizeof(POOL_KEEPALIVE_REQ),
                                          resp, sizeof(resp), &got);

            pthread_mutex_lock(&pool->mu);
            e->in_use    = 0;
            e->last_used = time(NULL);
            if (prc != BP_OK) {
                /* v0.9.0+: mark dead AND schedule auto-reopen on the
                 * next keepalive tick.  Backoff starts at 1 s. */
                e->dead = 1;
                e->last_reopen_attempt = time(NULL);
                if (e->reopen_backoff_ms == 0) e->reopen_backoff_ms = 1000;
                fprintf(stderr, "[pool keepalive] slot=%u entry %d ping failed rc=%d — entry marked dead\n",
                        pool->slot, i, prc);
            }
            pthread_cond_signal(&pool->cv);
            pthread_mutex_unlock(&pool->mu);
        }

        /* v0.9.0 Phase 4: walk dead entries and attempt auto-reopen.
         * Each dead entry has its own backoff (1s → 2s → 4s → ...
         * cap 30s).  On success: dead=0, backoff resets, signal cv so
         * a waiting pool_txrx picks it up.  On failure: double backoff. */
        for (int i = 0; i < pool->size; i++) {
            pthread_mutex_lock(&pool->mu);
            if (pool->ka_stop) { pthread_mutex_unlock(&pool->mu); goto out; }
            struct bp_pool_entry *e = &pool->entries[i];
            if (!e->dead || e->in_use) {
                pthread_mutex_unlock(&pool->mu);
                continue;
            }
            time_t t_now = time(NULL);
            long since_attempt_ms = (long)(t_now - e->last_reopen_attempt) * 1000;
            if (since_attempt_ms < e->reopen_backoff_ms) {
                pthread_mutex_unlock(&pool->mu);
                continue;
            }
            /* Claim the entry so a parallel pool_txrx doesn't try to
             * use it while we re-open. */
            e->in_use = 1;
            e->last_reopen_attempt = t_now;
            uint16_t app_handle = e->app_handle;
            pthread_mutex_unlock(&pool->mu);

            /* Force-close the stale local txrx_conn slot (PLC has
             * already dropped the conn — we wipe our cached state to
             * free the app_handle for re-open). */
            (void)bp_txrx_force_close_local(cl, app_handle);

            uint8_t epath2[2] = { 0x01, pool->slot };
            bp_conn_spec_t cs2 = {
                .app_handle   = app_handle,
                .options      = 0,
                .encoded_path = epath2,
                .path_size    = 2,
                .conn_params  = pool->conn_params,
            };
            int orc = bp_client_txrx_open(cl, &cs2, NULL, NULL);

            pthread_mutex_lock(&pool->mu);
            e->in_use = 0;
            if (orc == BP_OK) {
                e->dead = 0;
                e->last_used = time(NULL);
                e->reopen_backoff_ms = 1000;       /* reset for next time */
                fprintf(stderr, "[pool keepalive] slot=%u entry %d auto-reopen OK\n",
                        pool->slot, i);
                pthread_cond_broadcast(&pool->cv);
            } else {
                /* Double the backoff; cap at 30 s. */
                int next = e->reopen_backoff_ms * 2;
                if (next > 30000) next = 30000;
                e->reopen_backoff_ms = next;
                fprintf(stderr, "[pool keepalive] slot=%u entry %d auto-reopen rc=%d "
                                "— next attempt in %d ms\n",
                        pool->slot, i, orc, next);
            }
            pthread_mutex_unlock(&pool->mu);
        }
    }
out:
    return NULL;
}

/* ────────── bp_client_pool_batch ────────────────────────────── */

struct batch_ctx {
    bp_client_t      *cl;
    uint8_t           slot;
    bp_batch_item_t  *items;
    size_t            count;
    pthread_mutex_t   idx_mu;     /* protects next_idx */
    size_t            next_idx;
};

static void *pool_batch_worker(void *arg) {
    struct batch_ctx *ctx = (struct batch_ctx *)arg;
    while (1) {
        pthread_mutex_lock(&ctx->idx_mu);
        size_t i = ctx->next_idx++;
        pthread_mutex_unlock(&ctx->idx_mu);
        if (i >= ctx->count) return NULL;

        bp_batch_item_t *it = &ctx->items[i];
        it->resp_len = 0;
        it->rc = bp_client_pool_txrx(ctx->cl, ctx->slot,
                                       it->req, it->req_size,
                                       it->resp, it->resp_capacity,
                                       &it->resp_len);
    }
}

int bp_client_pool_batch(bp_client_t *cl, uint8_t slot,
                          bp_batch_item_t *items, size_t count) {
    if (!cl || !items) return BP_ERR_NULL_ARG;
    if (slot > BP_MSG_MAX_SLOT) return BP_ERR_SLOT_TOO_LARGE;
    if (count == 0) return BP_OK;

    /* Read pool size for worker-thread count.  Pool may close
     * concurrently — handle that via pool_txrx's NOT_OPEN return. */
    pthread_mutex_lock(&cl->pools_mu);
    struct bp_pool *pool = &cl->pools[slot];
    if (!pool->initialized) {
        pthread_mutex_unlock(&cl->pools_mu);
        return BP_ERR_NOT_OPEN;
    }
    size_t worker_count = pool->size;
    pthread_mutex_unlock(&cl->pools_mu);
    if (worker_count > count) worker_count = count;

    struct batch_ctx ctx = {
        .cl    = cl,
        .slot  = slot,
        .items = items,
        .count = count,
        .next_idx = 0,
    };
    pthread_mutex_init(&ctx.idx_mu, NULL);

    pthread_t *threads = calloc(worker_count, sizeof(*threads));
    if (!threads) {
        pthread_mutex_destroy(&ctx.idx_mu);
        return BP_ERR_GENERIC;
    }
    for (size_t i = 0; i < worker_count; i++) {
        int rc = pthread_create(&threads[i], NULL, pool_batch_worker, &ctx);
        if (rc != 0) {
            /* Mark remaining items as failed so caller doesn't see
             * uninitialized rc, then join workers we managed to
             * spawn and return. */
            for (size_t j = i; j < worker_count; j++) threads[j] = 0;
            fprintf(stderr, "[bp_client_pool_batch] pthread_create rc=%d "
                            "(spawned %zu/%zu workers)\n", rc, i, worker_count);
            break;
        }
    }
    for (size_t i = 0; i < worker_count; i++) {
        if (threads[i] != 0) pthread_join(threads[i], NULL);
    }
    free(threads);

    /* Items past next_idx (if we failed to spawn all workers) — mark
     * as generic-rc so caller can see they didn't run. */
    pthread_mutex_lock(&ctx.idx_mu);
    size_t consumed = ctx.next_idx > count ? count : ctx.next_idx;
    pthread_mutex_unlock(&ctx.idx_mu);
    for (size_t i = consumed; i < count; i++) {
        items[i].rc = BP_ERR_GENERIC;
        items[i].resp_len = 0;
    }
    pthread_mutex_destroy(&ctx.idx_mu);

    int overall = BP_OK;
    for (size_t i = 0; i < count; i++) {
        if (items[i].rc != BP_OK) { overall = BP_ERR_GENERIC; break; }
    }
    return overall;
}

/* ────────── bp_client_pool_close ────────────────────────────── */

int bp_client_pool_close(bp_client_t *cl, uint8_t slot) {
    if (!cl) return BP_ERR_NULL_ARG;
    if (slot >= BP_POOL_MAX_SLOTS) return BP_ERR_SLOT_TOO_LARGE;

    pthread_mutex_lock(&cl->pools_mu);
    struct bp_pool *pool = &cl->pools[slot];
    if (!pool->initialized) {
        pthread_mutex_unlock(&cl->pools_mu);
        return BP_OK;        /* idempotent */
    }

    /* Mark uninitialized FIRST so new pool_txrx arrivals see NOT_OPEN
     * and exit; existing waiters re-check after our cv broadcast. */
    pthread_mutex_lock(&pool->mu);
    pool->initialized = 0;
    pool->ka_stop = 1;
    pthread_cond_broadcast(&pool->cv);
    int ka_active = pool->ka_active;
    pthread_mutex_unlock(&pool->mu);
    if (ka_active) {
        pthread_join(pool->ka_thread, NULL);
        pool->ka_active = 0;
    }

    /* Wait for any in-flight pool_txrx calls to return their entries. */
    pthread_mutex_lock(&pool->mu);
    while (1) {
        int any_busy = 0;
        for (int i = 0; i < pool->size; i++) {
            if (pool->entries[i].in_use) { any_busy = 1; break; }
        }
        if (!any_busy) break;
        pthread_cond_wait(&pool->cv, &pool->mu);
    }
    pthread_mutex_unlock(&pool->mu);

    uint8_t epath[2] = { 0x01, slot };
    for (int i = 0; i < pool->size; i++) {
        if (pool->entries[i].app_handle == 0) continue;
        bp_conn_spec_t cs = {
            .app_handle   = pool->entries[i].app_handle,
            .options      = 0,
            .encoded_path = epath,
            .path_size    = 2,
            .conn_params  = pool->conn_params,
        };
        (void)bp_client_txrx_close(cl, &cs);
    }

    pthread_cond_destroy(&pool->cv);
    pthread_mutex_destroy(&pool->mu);
    memset(pool, 0, sizeof(*pool));

    pthread_mutex_unlock(&cl->pools_mu);
    return BP_OK;
}
