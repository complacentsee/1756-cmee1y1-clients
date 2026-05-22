/*
 * tagcache.c — per-client symbol cache (v0.9.0 Phase 1).
 *
 * Keyed by PLC path (the same string passed to bp_tagdb_open).
 * Multiple bp_tagdb_t handles to the same path share one cache.
 *
 * Cache lifecycle:
 *   - bp_tagdb_build invalidates the cache for db->path, then writes
 *     total_count from the engine's response and allocates symbols[].
 *   - bp_tagdb_lookup_symbol does a linear search in symbols[]; on
 *     miss it walks bp_tagdb_symbol_at(known_count..total_count-1)
 *     until it finds a match, appending each examined symbol to the
 *     cache.  Subsequent lookups of those names are O(N) array scans
 *     instead of IPC round-trips.
 *   - bp_tagdb_preload_symbols walks the entire table eagerly.
 *
 * Concurrency: per-cache pthread_mutex_t protects symbols + known_count.
 * The client-level tag_cache_mu only protects the small in_use array
 * (add/remove/find).  Lookups don't take the client mutex on the hot
 * path — only the cache mutex.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

/* ────────── slot allocation / lookup ────────────────────────── */

/* Find the cache slot matching `path`, or NULL if none.  Caller holds
 * cl->tag_cache_mu. */
static struct bp_tag_cache *find_locked(bp_client_t *cl, const char *path) {
    for (int i = 0; i < BP_TAG_CACHE_MAX; i++) {
        if (cl->tag_caches[i].in_use &&
            strcmp(cl->tag_caches[i].path, path) == 0) {
            return &cl->tag_caches[i];
        }
    }
    return NULL;
}

/* Find an empty slot.  Caller holds cl->tag_cache_mu. */
static struct bp_tag_cache *alloc_locked(bp_client_t *cl) {
    for (int i = 0; i < BP_TAG_CACHE_MAX; i++) {
        if (!cl->tag_caches[i].in_use) return &cl->tag_caches[i];
    }
    return NULL;
}

struct bp_tag_cache *bp_tag_cache_find(bp_client_t *cl, const char *path) {
    if (!cl || !path) return NULL;
    pthread_mutex_lock(&cl->tag_cache_mu);
    struct bp_tag_cache *c = find_locked(cl, path);
    pthread_mutex_unlock(&cl->tag_cache_mu);
    return c;
}

struct bp_tag_cache *bp_tag_cache_find_or_alloc(bp_client_t *cl, const char *path) {
    if (!cl || !path) return NULL;
    size_t plen = strlen(path);
    if (plen > 254) return NULL;

    pthread_mutex_lock(&cl->tag_cache_mu);
    struct bp_tag_cache *c = find_locked(cl, path);
    if (!c) {
        c = alloc_locked(cl);
        if (c) {
            memset(c, 0, sizeof(*c));
            memcpy(c->path, path, plen);
            c->path[plen] = 0;
            pthread_mutex_init(&c->mu, NULL);
            c->in_use = 1;
        }
    }
    pthread_mutex_unlock(&cl->tag_cache_mu);
    return c;
}

void bp_tag_cache_invalidate(bp_client_t *cl, const char *path) {
    if (!cl || !path) return;
    pthread_mutex_lock(&cl->tag_cache_mu);
    struct bp_tag_cache *c = find_locked(cl, path);
    if (c) {
        pthread_mutex_lock(&c->mu);
        free(c->symbols);
        c->symbols     = NULL;
        c->cap_count   = 0;
        c->known_count = 0;
        c->total_count = 0;
        pthread_mutex_unlock(&c->mu);
    }
    pthread_mutex_unlock(&cl->tag_cache_mu);
}

void bp_tag_cache_free_all(bp_client_t *cl) {
    if (!cl) return;
    pthread_mutex_lock(&cl->tag_cache_mu);
    for (int i = 0; i < BP_TAG_CACHE_MAX; i++) {
        struct bp_tag_cache *c = &cl->tag_caches[i];
        if (!c->in_use) continue;
        pthread_mutex_lock(&c->mu);
        free(c->symbols);
        c->symbols = NULL;
        pthread_mutex_unlock(&c->mu);
        pthread_mutex_destroy(&c->mu);
        memset(c, 0, sizeof(*c));
    }
    pthread_mutex_unlock(&cl->tag_cache_mu);
}

/* ────────── lookup_symbol ───────────────────────────────────── */

int bp_tagdb_lookup_symbol(bp_tagdb_t *db, const char *name,
                             bp_symbol_info_t *out_info) {
    if (!db || !name || !out_info) return BP_ERR_NULL_ARG;

    struct bp_tag_cache *c = bp_tag_cache_find_or_alloc(db->client, db->path);
    if (!c) return BP_ERR_CLIENT_OPEN;

    pthread_mutex_lock(&c->mu);

    /* Cache initialised? (total_count gets set by bp_tagdb_build via the
     * helper below.)  If no Build has populated the cache yet, return
     * PARAM_RANGE — caller's mistake to look up before Build. */
    if (c->cap_count == 0) {
        pthread_mutex_unlock(&c->mu);
        return BP_ERR_PARAM_RANGE;
    }

    /* Linear scan known entries first. */
    for (uint16_t i = 0; i < c->known_count; i++) {
        if (strcmp(c->symbols[i].name, name) == 0) {
            *out_info = c->symbols[i];
            pthread_mutex_unlock(&c->mu);
            return BP_OK;
        }
    }

    /* Cache miss + room to grow: walk symbol_at incrementally.  We
     * release the cache mutex around each IPC call so other lookups
     * for already-cached names don't block on us. */
    while (c->known_count < c->total_count) {
        uint16_t idx = c->known_count;
        pthread_mutex_unlock(&c->mu);

        bp_symbol_info_t sym;
        int rc = bp_tagdb_symbol_at(db, idx, &sym);
        if (rc != BP_OK) return rc;

        pthread_mutex_lock(&c->mu);
        /* Another thread may have advanced known_count past idx while
         * we were in the IPC call.  Be defensive: append only if our
         * index is still the next slot.  Otherwise discard our copy
         * and re-scan. */
        if (idx == c->known_count && c->known_count < c->cap_count) {
            c->symbols[c->known_count++] = sym;
        }
        /* Check the just-appended entry (or the one a parallel thread
         * inserted) for a name match. */
        for (uint16_t i = idx; i < c->known_count; i++) {
            if (strcmp(c->symbols[i].name, name) == 0) {
                *out_info = c->symbols[i];
                pthread_mutex_unlock(&c->mu);
                return BP_OK;
            }
        }
    }

    pthread_mutex_unlock(&c->mu);
    return BP_ERR_PARAM_RANGE;   /* not found in the PLC's table */
}

int bp_tagdb_preload_symbols(bp_tagdb_t *db) {
    if (!db) return BP_ERR_NULL_ARG;
    struct bp_tag_cache *c = bp_tag_cache_find_or_alloc(db->client, db->path);
    if (!c) return BP_ERR_CLIENT_OPEN;

    pthread_mutex_lock(&c->mu);
    if (c->cap_count == 0) {
        pthread_mutex_unlock(&c->mu);
        return BP_ERR_PARAM_RANGE;     /* Build hasn't populated the size */
    }
    while (c->known_count < c->total_count) {
        uint16_t idx = c->known_count;
        pthread_mutex_unlock(&c->mu);

        bp_symbol_info_t sym;
        int rc = bp_tagdb_symbol_at(db, idx, &sym);
        if (rc != BP_OK) return rc;

        pthread_mutex_lock(&c->mu);
        if (idx == c->known_count && c->known_count < c->cap_count) {
            c->symbols[c->known_count++] = sym;
        }
    }
    pthread_mutex_unlock(&c->mu);
    return BP_OK;
}

/* ────────── build hook ──────────────────────────────────────── */

/* Called from bp_tagdb_build after a successful walk.  Resizes the
 * cache for this path to hold N entries and clears known_count so the
 * lazy fill rebuilds.  total_count==0 is allowed (PLC has no tags). */
int bp_tag_cache_reset_after_build(bp_client_t *cl, const char *path,
                                    uint16_t total_count) {
    struct bp_tag_cache *c = bp_tag_cache_find_or_alloc(cl, path);
    if (!c) return BP_ERR_CLIENT_OPEN;

    pthread_mutex_lock(&c->mu);
    free(c->symbols);
    c->symbols     = NULL;
    c->cap_count   = 0;
    c->known_count = 0;
    c->total_count = total_count;
    if (total_count > 0) {
        c->symbols = calloc((size_t)total_count, sizeof(*c->symbols));
        if (!c->symbols) {
            pthread_mutex_unlock(&c->mu);
            return BP_ERR_CLIENT_OPEN;
        }
        c->cap_count = total_count;
    }
    pthread_mutex_unlock(&c->mu);
    return BP_OK;
}
