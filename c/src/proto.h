/*
 * proto.h — internal protocol layout constants.
 *
 * SPDX-License-Identifier: MIT
 *
 * Mirrors docs/protocol.md byte-for-byte.  Do not change without
 * updating that file (which is the spec).
 */
#ifndef BPCLIENT_PROTO_H
#define BPCLIENT_PROTO_H

#include <stddef.h>
#include <stdint.h>
#include <pthread.h>
#include <semaphore.h>

/* ----- Shared memory geometry ----- */
#define BP_SHM_NAME       "/bpShmem"
#define BP_SHM_TOTAL_SIZE 0x4B0000u   /* 16 slots × 0x4B000 */
#define BP_SLOT_COUNT     16
#define BP_SLOT_STRIDE    0x4B000u

/* ----- Named semaphores ----- */
#define BP_SEM_SHMLOCK    "/bpShm"     /* slot-scan mutex */
/* /bpReq00 .. /bpReq15 */
/* /bpResp00 .. /bpResp15 */

/* ----- Slot header offsets (from slot start) ----- */
#define BP_HDR_OPCODE         0x00
#define BP_HDR_PAYLOAD_SIZE   0x04
#define BP_HDR_FN_NAME        0x08    /* 63 bytes + NUL at 0x47 */
#define BP_HDR_CLIENT_PID     0x48
#define BP_HDR_IS_DOCKER      0x4C
#define BP_HDR_ERRORCODE      0x50
#define BP_HDR_SLOT_OWNER     0x58
#define BP_HDR_SLOT_NUMBER    0x60
#define BP_HDR_PAYLOAD_START  0x78

/* ----- Header constants ----- */
#define BP_OPCODE_CIP         0x00CAu
#define BP_PENDING_ERROR_BITS 0xFFFFFED3u  /* int32(-301) bit pattern */

/* ----- AccessTagData layout (within a slot, after the header) ----- */
#define BP_TAGDATA_SERVICE_OFF  0x178u  /* uint16 service (unused) */
#define BP_TAGDATA_COUNT_OFF    0x17Au  /* uint16 request count */
#define BP_TAGDATA_REQ0_START   0x180u  /* first request descriptor */
#define BP_TAGDATA_REQ_STRIDE   0x120u  /* bytes per descriptor */

/* Within a request descriptor (relative to descriptor start) */
#define BP_REQ_TAGNAME_OFF        0x000u  /* char[256] */
#define BP_REQ_DATATYPE_OFF       0x100u  /* uint16 */
#define BP_REQ_ELEM_BYTE_SIZE_OFF 0x102u  /* uint16 */
#define BP_REQ_ACTION_OFF         0x104u  /* uint16: 1=read 2=write */
#define BP_REQ_ELEM_COUNT_OFF     0x106u  /* uint16 */
#define BP_REQ_HAS_EXTRA_OFF      0x108u  /* uint8 — always 0 in this SDK */
#define BP_REQ_DATA_PTR_OFF       0x110u  /* uint64 — unused */
#define BP_REQ_RESULT_OFF         0x118u  /* uint32 — server-written */

/* ----- Symbol-info descriptor offsets (within the 128-byte struct) ----- */
#define BP_SYM_NAME_OFF        0x00u   /* char[100] NUL-terminated */
#define BP_SYM_DATATYPE_OFF    0x64u   /* uint16 */
#define BP_SYM_STRUCTTYPE_OFF  0x68u   /* uint16 */
#define BP_SYM_ELEMSIZE_OFF    0x6Cu   /* uint32 elem_byte_size */
#define BP_SYM_DIM0_OFF        0x70u   /* uint32 dim0 (0 if scalar) */
#define BP_SYM_DIM1_OFF        0x74u   /* uint32 dim1 (0 if rank < 2) */
#define BP_SYM_DIM2_OFF        0x78u   /* uint32 dim2 (0 if rank < 3) */
#define BP_SYM_FLAGS_OFF       0x7Cu   /* uint16 */

/* ----- Per-connection state (v0.7.0+ small-buffer TxRx) ----- */
/* See docs/protocol.md "Connected messaging — wire format".  Each
 * bp_client_txrx_open occupies one slot; bp_client_txrx_close frees
 * it.  Lookup is by app_handle, which the caller passes through
 * bp_conn_spec_t to every TxRx call. */
#define BP_TXRX_MAX_CONNS  16

struct bp_txrx_conn {
    int      in_use;
    uint16_t app_handle;       /* cache key — from bp_conn_spec_t */
    uint8_t  slot;             /* derived from spec->encoded_path */
    uint16_t conn_serial;      /* random, sent in LFO; echoed by FC */
    uint16_t vendor_id;        /* 0x0001 for Rockwell */
    uint32_t orig_serial;      /* random per-conn, must match for FC */
    uint32_t ot_conn_id;       /* PLC-chosen, returned by LFO */
    uint32_t to_conn_id;       /* originator-chosen, echoed by PLC */
    uint16_t sequence;         /* diagnostic only — NOT on the wire */
};

/* ----- Per-slot connection pool (v0.8.0 Phase 2) -------------- */
/* Internal app_handle range used by pools: 0x8000..0xFFFF.  Caller
 * app_handles for bp_client_txrx_* must stay < 0x8000.  Encoding:
 *
 *   app_handle = 0x8000 | (slot << 8) | entry_index
 *
 * — so given an app_handle in the txrx_conns table we can identify
 * (pool slot, entry index) without an extra lookup. */
#define BP_POOL_APP_HANDLE_BASE  0x8000
#define BP_POOL_MAX_SLOTS        20      /* 0..0x13 — matches BP_MSG_MAX_SLOT + 1 */

struct bp_pool_entry {
    int      in_use;            /* 1 = currently borrowed by a thread */
    uint16_t app_handle;        /* matches entry in txrx_conns table */
    time_t   last_used;         /* updated on every txrx; used by keepalive */
    int      dead;              /* 1 = LFO failed during pool_open recovery,
                                 *     or keepalive saw a fatal transport error;
                                 *     keepalive auto-reopens (v0.9.0 Phase 4) */
    /* Auto-reopen bookkeeping (v0.9.0 Phase 4).  Keepalive retries a
     * dead entry with exponential backoff (1s → 2s → 4s → ... cap 30s);
     * on success: dead=0, backoff_ms resets to initial, cv broadcast
     * so a waiting pool_txrx caller can pick up the revived entry. */
    time_t   last_reopen_attempt;
    int      reopen_backoff_ms;
};

struct bp_pool {
    int      initialized;
    uint8_t  slot;
    uint8_t  size;
    uint16_t keepalive_ms;
    uint16_t conn_params;
    struct bp_client *client;    /* back-pointer for the keepalive thread */
    pthread_mutex_t mu;
    pthread_cond_t  cv;          /* signalled when an entry returns to free */
    struct bp_pool_entry entries[BP_POOL_MAX_SIZE];
    /* Keepalive thread bookkeeping */
    pthread_t        ka_thread;
    int              ka_active;  /* 1 = thread is running; 0 = thread joined */
    int              ka_stop;    /* set to 1 to request keepalive thread exit */
};

/* ----- Per-PLC symbol cache (v0.9.0 Phase 1) ----- */
/* One slot per distinct PLC path seen by this client.  Eight is
 * generous — most callers connect to one PLC; the labelverification
 * gateway pattern is one-process / one-PLC.  Bumping this requires
 * recompile only (no public API change). */
#define BP_TAG_CACHE_MAX 8

struct bp_tag_cache {
    int                in_use;
    char               path[256];    /* keyed on this; 0-init when in_use=0 */
    pthread_mutex_t    mu;           /* protects symbols + counts */
    bp_symbol_info_t  *symbols;      /* dynamic; len = cap_count */
    uint16_t           cap_count;    /* allocated capacity (= total_count after build) */
    uint16_t           known_count;  /* number of valid entries in symbols[] */
    uint16_t           total_count;  /* from BuildTagDb's status field */
    /* parent client back-pointer is implicit via array offset; we don't
     * store it because clients only ever look up by-path on themselves. */
};

/* ----- Internal Client + TagDB structs (private to the library) ----- */
struct bp_client {
    int        shm_fd;
    uint8_t   *shm;                  /* mmap'd BP_SHM_TOTAL_SIZE */
    sem_t     *sem_shmlock;
    sem_t     *sem_req[BP_SLOT_COUNT];
    sem_t     *sem_resp[BP_SLOT_COUNT];
    pid_t      pid;                  /* getpid() */
    pthread_mutex_t scan_mu;         /* serializes the slot scan within this process */
    pthread_mutex_t txrx_mu;         /* protects txrx_conns table */
    struct bp_txrx_conn txrx_conns[BP_TXRX_MAX_CONNS];
    pthread_mutex_t cip_err_mu;      /* protects cip_err* below */
    int             cip_err_present; /* 1 if cip_err carries a value */
    bp_cip_status_t cip_err;         /* last CIP-layer rejection on this client */
    pthread_mutex_t pools_mu;        /* protects pools[] open/close lifecycle */
    struct bp_pool  pools[BP_POOL_MAX_SLOTS];
    pthread_mutex_t   tag_cache_mu;  /* protects tag_caches[] add/remove */
    struct bp_tag_cache tag_caches[BP_TAG_CACHE_MAX];
};

/* Internal helpers exposed across translation units. */
struct bp_tag_cache *bp_tag_cache_find_or_alloc(bp_client_t *cl, const char *path);
struct bp_tag_cache *bp_tag_cache_find(bp_client_t *cl, const char *path);
void                 bp_tag_cache_invalidate(bp_client_t *cl, const char *path);
void                 bp_tag_cache_free_all(bp_client_t *cl);
int                  bp_tag_cache_reset_after_build(bp_client_t *cl,
                                                     const char *path,
                                                     uint16_t total_count);

/* Internal helper exposed across translation units — records a
 * structured CIP-layer error on the client.  Used by conn.c on
 * LFO/FC rejections. */
void bp_record_cip_error(bp_client_t *cl, uint8_t svc, uint8_t status,
                          uint16_t ext_status, uint8_t slot);

/* Force-close a txrx_conn slot locally without sending Forward_Close.
 * Used by pool auto-reopen (v0.9.0 Phase 4) when the PLC has already
 * dropped the connection (keepalive ping failed).  Returns BP_OK if
 * a slot matched; BP_ERR_NOT_OPEN if none. */
int bp_txrx_force_close_local(bp_client_t *cl, uint16_t app_handle);

struct bp_tagdb {
    bp_client_t *client;
    uint32_t     handle;
    char         path[256];          /* NUL-terminated, original from open */
};

/* ----- Internal call dispatch ----- */
typedef struct {
    const char *fn_name;
    uint32_t    payload_size;
    /* Called with a pointer to the slot's first byte to fill the
     * request payload.  May be NULL for opcodes with no input. */
    void      (*fill_payload)(uint8_t *slot, void *user);
    /* Called with the slot ptr after a successful response (errorcode==0).
     * May be NULL if no reply parsing is needed. */
    void      (*read_reply)  (uint8_t *slot, void *user);
    int         timeout_ms;          /* sem_timedwait deadline */
    void       *user;
} bp_call_spec_t;

int  bp_client_call(bp_client_t *c, const bp_call_spec_t *spec);

/* Helper inline utilities used by marshal.c */
static inline uint16_t bp_ld_u16(const uint8_t *p) {
    return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}
static inline uint32_t bp_ld_u32(const uint8_t *p) {
    return  (uint32_t)p[0]        | ((uint32_t)p[1] << 8)
         | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
static inline void bp_st_u16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)(v); p[1] = (uint8_t)(v >> 8);
}
static inline void bp_st_u32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v);        p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);  p[3] = (uint8_t)(v >> 24);
}
static inline void bp_st_u64(uint8_t *p, uint64_t v) {
    bp_st_u32(p, (uint32_t)v);
    bp_st_u32(p + 4, (uint32_t)(v >> 32));
}

#endif /* BPCLIENT_PROTO_H */
