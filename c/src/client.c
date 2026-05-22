/*
 * client.c — Open/Close + slot dispatcher.
 *
 * SPDX-License-Identifier: MIT
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <semaphore.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

#include "../include/bpclient.h"
#include "proto.h"

/* gettid() helper — glibc may or may not expose it depending on
 * version.  Use the syscall directly for portability. */
static pid_t gettid_(void) { return (pid_t)syscall(SYS_gettid); }

int bp_client_open(bp_client_t **out_client) {
    if (!out_client) return BP_ERR_NULL_ARG;
    *out_client = NULL;

    bp_client_t *c = calloc(1, sizeof(*c));
    if (!c) return BP_ERR_CLIENT_OPEN;
    c->pid = getpid();
    c->shm_fd = -1;

    /* shm_open the bpShmem region */
    c->shm_fd = shm_open(BP_SHM_NAME, O_RDWR, 0);
    if (c->shm_fd < 0) {
        free(c);
        return BP_ERR_CLIENT_OPEN;
    }

    void *m = mmap(NULL, BP_SHM_TOTAL_SIZE, PROT_READ | PROT_WRITE,
                   MAP_SHARED, c->shm_fd, 0);
    if (m == MAP_FAILED) {
        close(c->shm_fd);
        free(c);
        return BP_ERR_CLIENT_OPEN;
    }
    c->shm = m;

    /* Local scan mutex */
    pthread_mutex_init(&c->scan_mu, NULL);

    /* Open the shm-lock semaphore */
    c->sem_shmlock = sem_open(BP_SEM_SHMLOCK, 0);
    if (c->sem_shmlock == SEM_FAILED) {
        c->sem_shmlock = NULL;
        munmap(c->shm, BP_SHM_TOTAL_SIZE);
        close(c->shm_fd);
        pthread_mutex_destroy(&c->scan_mu);
        free(c);
        return BP_ERR_CLIENT_OPEN;
    }

    /* Open all 16 req + 16 resp semaphores */
    for (int i = 0; i < BP_SLOT_COUNT; i++) {
        char name[32];
        snprintf(name, sizeof(name), "/bpReq%02d", i);
        c->sem_req[i] = sem_open(name, 0);
        if (c->sem_req[i] == SEM_FAILED) { c->sem_req[i] = NULL; goto fail; }

        snprintf(name, sizeof(name), "/bpResp%02d", i);
        c->sem_resp[i] = sem_open(name, 0);
        if (c->sem_resp[i] == SEM_FAILED) { c->sem_resp[i] = NULL; goto fail; }
    }

    *out_client = c;
    return BP_OK;

fail:
    bp_client_close(c);
    return BP_ERR_CLIENT_OPEN;
}

void bp_client_close(bp_client_t *c) {
    if (!c) return;
    for (int i = 0; i < BP_SLOT_COUNT; i++) {
        if (c->sem_req[i])  { sem_close(c->sem_req[i]);  c->sem_req[i]  = NULL; }
        if (c->sem_resp[i]) { sem_close(c->sem_resp[i]); c->sem_resp[i] = NULL; }
    }
    if (c->sem_shmlock) { sem_close(c->sem_shmlock); c->sem_shmlock = NULL; }
    if (c->shm)        { munmap(c->shm, BP_SHM_TOTAL_SIZE); c->shm = NULL; }
    if (c->shm_fd >= 0) { close(c->shm_fd); c->shm_fd = -1; }
    pthread_mutex_destroy(&c->scan_mu);
    free(c);
}

/* Drain any pending posts on a semaphore (best effort). */
static void sem_drain(sem_t *s) {
    while (sem_trywait(s) == 0) { /* drain */ }
}

/* Reserve a free slot for our tid.  Returns slot index 0..15 or BP_ERR_NO_FREE_SLOT. */
static int reserve_slot(bp_client_t *c, pid_t tid) {
    if (sem_wait(c->sem_shmlock) != 0) return BP_ERR_GENERIC;
    pthread_mutex_lock(&c->scan_mu);

    int found = -1;
    for (int i = 0; i < BP_SLOT_COUNT; i++) {
        uint8_t *slot = c->shm + (size_t)i * BP_SLOT_STRIDE;
        uint64_t owner =
              (uint64_t)bp_ld_u32(slot + BP_HDR_SLOT_OWNER)
            | ((uint64_t)bp_ld_u32(slot + BP_HDR_SLOT_OWNER + 4) << 32);
        if (owner == 0) {
            uint64_t new_owner = ((uint64_t)tid << 32) | (uint64_t)c->pid;
            bp_st_u64(slot + BP_HDR_SLOT_OWNER, new_owner);
            sem_drain(c->sem_req[i]);
            sem_drain(c->sem_resp[i]);
            found = i;
            break;
        }
    }

    pthread_mutex_unlock(&c->scan_mu);
    sem_post(c->sem_shmlock);
    return found >= 0 ? found : BP_ERR_NO_FREE_SLOT;
}

static void release_slot(bp_client_t *c, int slot) {
    if (sem_wait(c->sem_shmlock) != 0) return;
    uint8_t *p = c->shm + (size_t)slot * BP_SLOT_STRIDE;
    bp_st_u64(p + BP_HDR_SLOT_OWNER, 0);
    sem_drain(c->sem_req[slot]);
    sem_drain(c->sem_resp[slot]);
    sem_post(c->sem_shmlock);
}

int bp_client_call(bp_client_t *c, const bp_call_spec_t *spec) {
    if (!c || !spec || !spec->fn_name) return BP_ERR_NULL_ARG;

    pid_t tid = gettid_();
    int slot = reserve_slot(c, tid);
    if (slot < 0) return slot;

    uint8_t *p = c->shm + (size_t)slot * BP_SLOT_STRIDE;
    int rc = BP_OK;

    /* Header */
    bp_st_u16(p + BP_HDR_OPCODE,       BP_OPCODE_CIP);
    bp_st_u16(p + BP_HDR_OPCODE + 2,   0);
    bp_st_u32(p + BP_HDR_PAYLOAD_SIZE, spec->payload_size);

    /* fn_name: 63 bytes + NUL at +0x47, padded with NUL */
    memset(p + BP_HDR_FN_NAME, 0, 64);
    size_t fn_len = strlen(spec->fn_name);
    if (fn_len > 63) fn_len = 63;
    memcpy(p + BP_HDR_FN_NAME, spec->fn_name, fn_len);

    bp_st_u32(p + BP_HDR_CLIENT_PID,    (uint32_t)c->pid);
    bp_st_u16(p + BP_HDR_IS_DOCKER,     1);   /* always 1 from this SDK */
    bp_st_u16(p + BP_HDR_IS_DOCKER + 2, 0);
    bp_st_u32(p + BP_HDR_ERRORCODE,     BP_PENDING_ERROR_BITS);
    bp_st_u32(p + BP_HDR_ERRORCODE + 4, 0);
    /* slot_owner already set by reserve_slot() */
    bp_st_u32(p + BP_HDR_SLOT_NUMBER,    (uint32_t)slot);
    bp_st_u32(p + BP_HDR_SLOT_NUMBER + 4, 0);
    /* Clear +0x68..+0x77 (16 reserved bytes) */
    memset(p + 0x68, 0, 0x10);

    /* Payload */
    if (spec->fill_payload) spec->fill_payload(p, spec->user);

    /* Kick the server */
    if (sem_post(c->sem_req[slot]) != 0) {
        rc = BP_ERR_SEND_REQUEST;
        goto out;
    }

    /* Wait for reply */
    int timeout_ms = spec->timeout_ms > 0 ? spec->timeout_ms : 30000;
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long total_ns = ts.tv_nsec + (long)timeout_ms * 1000000L;
    ts.tv_sec  += total_ns / 1000000000L;
    ts.tv_nsec  = total_ns % 1000000000L;

    int wait_rc = sem_timedwait(c->sem_resp[slot], &ts);
    if (wait_rc != 0) {
        /* Fallback: poll the errorcode field for 200ms */
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += 200 * 1000000L;
        if (deadline.tv_nsec >= 1000000000L) {
            deadline.tv_sec += 1;
            deadline.tv_nsec -= 1000000000L;
        }
        int got_reply = 0;
        while (1) {
            uint32_t ec_bits = bp_ld_u32(p + BP_HDR_ERRORCODE);
            if (ec_bits != BP_PENDING_ERROR_BITS) { got_reply = 1; break; }
            struct timespec now;
            clock_gettime(CLOCK_REALTIME, &now);
            if (now.tv_sec > deadline.tv_sec ||
                (now.tv_sec == deadline.tv_sec && now.tv_nsec >= deadline.tv_nsec))
                break;
            struct timespec sleep_dur = { 0, 2 * 1000000L };
            nanosleep(&sleep_dur, NULL);
        }
        if (!got_reply) { rc = BP_ERR_RECV_ANSWER; goto out; }
    }

    /* Check error code */
    int32_t errcode = (int32_t)bp_ld_u32(p + BP_HDR_ERRORCODE);
    if (errcode != 0) { rc = (int)errcode; goto out; }

    /* Parse reply */
    if (spec->read_reply) spec->read_reply(p, spec->user);
    rc = BP_OK;

out:
    release_slot(c, slot);
    return rc;
}

/* ============================================================
 * OCXcip_Open — session
 * ============================================================ */
static void open_read_reply(uint8_t *slot, void *user) {
    uint32_t *h = (uint32_t *)user;
    *h = bp_ld_u32(slot + BP_HDR_PAYLOAD_START);
}

int bp_client_open_session(bp_client_t *c, uint32_t *out_handle) {
    if (!c) return BP_ERR_NULL_ARG;
    uint32_t handle = 0;
    bp_call_spec_t spec = {
        .fn_name      = "OCXcip_Open",
        .payload_size = 0x80,
        .fill_payload = NULL,
        .read_reply   = open_read_reply,
        .timeout_ms   = 5000,
        .user         = &handle,
    };
    int rc = bp_client_call(c, &spec);
    if (rc == BP_OK && out_handle) *out_handle = handle;
    return rc;
}
