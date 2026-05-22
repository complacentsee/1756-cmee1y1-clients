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
#define BP_SYM_FIELD1_OFF      0x6Cu   /* uint32 */
#define BP_SYM_FIELD2_OFF      0x70u   /* uint32 */
#define BP_SYM_FIELD3_OFF      0x74u   /* uint32 */
#define BP_SYM_INSTID_OFF      0x78u   /* uint32 */
#define BP_SYM_FLAGS_OFF       0x7Cu   /* uint16 */

/* ----- Internal Client + TagDB structs (private to the library) ----- */
struct bp_client {
    int        shm_fd;
    uint8_t   *shm;                  /* mmap'd BP_SHM_TOTAL_SIZE */
    sem_t     *sem_shmlock;
    sem_t     *sem_req[BP_SLOT_COUNT];
    sem_t     *sem_resp[BP_SLOT_COUNT];
    pid_t      pid;                  /* getpid() */
    pthread_mutex_t scan_mu;         /* serializes the slot scan within this process */
};

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
