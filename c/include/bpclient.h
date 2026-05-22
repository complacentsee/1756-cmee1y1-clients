/*
 * bpclient.h — outbound tag I/O against the 1756-CMEE1Y1 EEC card
 *
 * SPDX-License-Identifier: MIT
 *
 * Public API for the C client library.  All functions are
 * thread-safe (each call holds a slot for the duration of one
 * request/response round-trip).
 *
 * See docs/protocol.md for the wire-protocol spec this implements.
 * See docs/container-plumbing.md for the required Docker flags.
 */
#ifndef BPCLIENT_H
#define BPCLIENT_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================
 * Opaque handles
 * ============================================================ */
typedef struct bp_client bp_client_t;
typedef struct bp_tagdb  bp_tagdb_t;

/* ============================================================
 * Error codes — match the vendor OCX_ERR_* numbering
 * ============================================================ */
#define BP_OK                    0
#define BP_ERR_GENERIC          -1
#define BP_ERR_SEND_REQUEST   -200
#define BP_ERR_RECV_ANSWER    -201
#define BP_ERR_NULL_ARG       -300
#define BP_ERR_PENDING        -301
#define BP_ERR_NOT_OPEN       -303
#define BP_ERR_PARAM_RANGE    -305
#define BP_ERR_SLOT_TOO_LARGE -311
#define BP_ERR_CLIENT_OPEN -101802
#define BP_ERR_NO_FREE_SLOT -103001

/* Return a human-readable string for a BP_ERR_* code. Always returns
 * a pointer to static storage; never NULL.  Unknown codes return
 * "unknown error". */
const char *bp_strerror(int rc);

/* ============================================================
 * CIP atomic type codes (low 13 bits of data_type field)
 * ============================================================ */
#define BP_TYPE_BOOL  0xC1
#define BP_TYPE_SINT  0xC2
#define BP_TYPE_INT   0xC3
#define BP_TYPE_DINT  0xC4
#define BP_TYPE_LINT  0xC5
#define BP_TYPE_USINT 0xC6
#define BP_TYPE_UINT  0xC7
#define BP_TYPE_UDINT 0xC8
#define BP_TYPE_ULINT 0xC9
#define BP_TYPE_REAL  0xCA
#define BP_TYPE_LREAL 0xCB

/* ============================================================
 * Client lifecycle
 * ============================================================ */

/* bp_client_open
 *   Opens the IPC: maps /dev/shm/bpShmem and opens all required
 *   POSIX named semaphores (33 of them: /bpShm + 16×/bpReqNN +
 *   16×/bpRespNN).  Does NOT send any wire request.
 *
 *   Container plumbing required:
 *     --ipc=host --pid=host -v /dev/shm:/dev/shm
 *
 *   Returns BP_OK on success; *out_client is set.
 *   Returns BP_ERR_CLIENT_OPEN if shm_open/mmap/sem_open fails.
 */
int  bp_client_open(bp_client_t **out_client);

/* bp_client_close
 *   Releases the IPC.  Idempotent; passing NULL is a no-op. */
void bp_client_close(bp_client_t *client);

/* bp_client_open_session
 *   Sends an OCXcip_Open request.  Required before any tag DB or
 *   tag-access call.  Returns the server-assigned session handle
 *   (opaque; you don't pass it back to other calls — the wrapper
 *   tracks state via slot ownership).
 */
int  bp_client_open_session(bp_client_t *client, uint32_t *out_handle);

/* ============================================================
 * Tag database
 * ============================================================ */

/* bp_tagdb_open
 *   Sends OCXcip_CreateTagDbHandle for the given OldI CIP path
 *   (e.g. "P:1,S:2" for the ControlLogix in backplane slot 2).
 *   Returns BP_OK and sets *out_db on success.
 *
 *   path MUST follow the <letter>:<num> format, joined with commas.
 *   Plain Rockwell "1,2" notation will return BP_ERR_PARAM_RANGE.
 *
 *   Caller must bp_tagdb_close() the result. */
int  bp_tagdb_open(bp_client_t *client, const char *path, bp_tagdb_t **out_db);

/* bp_tagdb_close
 *   Sends OCXcip_DeleteTagDbHandle and frees the local handle.
 *   Idempotent; passing NULL is a no-op. */
void bp_tagdb_close(bp_tagdb_t *db);

/* bp_tagdb_build
 *   Walks the PLC's symbol table.  Typically takes ~200 ms for a
 *   few thousand symbols.  Returns the symbol count via
 *   *out_symbol_count if non-NULL. */
int  bp_tagdb_build(bp_tagdb_t *db, uint16_t *out_symbol_count);

/* Symbol descriptor.  Returned by bp_tagdb_symbol_at(). */
typedef struct {
    char     name[100];      /* NUL-terminated, up to 99 chars */
    uint16_t data_type;      /* CIP type code (mask with 0x1FFF for low bits) */
    uint16_t struct_type;    /* 0 for atomic scalars */
    uint32_t field1;
    uint32_t field2;
    uint32_t field3;
    uint32_t instance_id;
    uint16_t flags;
} bp_symbol_info_t;

/* bp_tagdb_symbol_at
 *   Fetches one symbol's descriptor by zero-based index.
 *   Valid indices: 0 to (symbol_count - 1) inclusive. */
int  bp_tagdb_symbol_at(bp_tagdb_t *db, uint16_t index,
                        bp_symbol_info_t *out_info);

/* Symbol-info accessors — convenience wrappers around the bit math. */
int      bp_symbol_is_array (const bp_symbol_info_t *info);  /* 1 = array,  0 = scalar */
int      bp_symbol_is_struct(const bp_symbol_info_t *info);  /* 1 = UDT,   0 = atomic */
uint16_t bp_symbol_type_code(const bp_symbol_info_t *info);  /* data_type & 0x1FFF */

/* ============================================================
 * Tag access (read/write)
 * ============================================================ */

#define BP_ACTION_READ  1
#define BP_ACTION_WRITE 2

/* One tag request in a batch (passed to bp_tagdb_access).
 *
 * For ActionRead:  data points to a buffer big enough for
 *                  elem_count * elem_byte_size bytes.  Server fills it.
 * For ActionWrite: data points to the bytes to send to the PLC.
 *
 * After the call, `result` holds the CIP General Status (0 = ok).
 */
typedef struct {
    const char *tag_name;       /* NUL-terminated, up to 254 chars */
    uint16_t    data_type;      /* CIP type code (BP_TYPE_*) */
    uint16_t    elem_byte_size; /* bytes per element (e.g. 4 for DINT) */
    uint16_t    action;         /* BP_ACTION_READ or BP_ACTION_WRITE */
    uint16_t    elem_count;     /* number of elements (1 for scalar) */
    void       *data;           /* read: out-buffer / write: in-bytes */
    uint32_t    result;         /* server-written: 0 on success, CIP general status on failure */
} bp_tag_request_t;

/* bp_tagdb_access
 *   Performs one OCXcip_AccessTagData call with `count` requests.
 *   The slot-level errorcode is the return value; per-request
 *   results are in each entry's `result` field. */
int  bp_tagdb_access(bp_tagdb_t *db,
                     bp_tag_request_t *requests,
                     size_t count);

/* ============================================================
 * Convenience helpers — single-tag scalar read/write
 *
 * Each one does one round-trip.  For batched access, use
 * bp_tagdb_access() with multiple requests in a single call.
 * ============================================================ */
/* Signed scalars */
int  bp_tagdb_read_sint  (bp_tagdb_t *db, const char *tag,  int8_t  *out_value);
int  bp_tagdb_write_sint (bp_tagdb_t *db, const char *tag,  int8_t   value);
int  bp_tagdb_read_int   (bp_tagdb_t *db, const char *tag, int16_t  *out_value);
int  bp_tagdb_write_int  (bp_tagdb_t *db, const char *tag, int16_t   value);
int  bp_tagdb_read_dint  (bp_tagdb_t *db, const char *tag, int32_t  *out_value);
int  bp_tagdb_write_dint (bp_tagdb_t *db, const char *tag, int32_t   value);
int  bp_tagdb_read_lint  (bp_tagdb_t *db, const char *tag, int64_t  *out_value);
int  bp_tagdb_write_lint (bp_tagdb_t *db, const char *tag, int64_t   value);

/* Unsigned scalars */
int  bp_tagdb_read_usint (bp_tagdb_t *db, const char *tag, uint8_t  *out_value);
int  bp_tagdb_write_usint(bp_tagdb_t *db, const char *tag, uint8_t   value);
int  bp_tagdb_read_uint  (bp_tagdb_t *db, const char *tag, uint16_t *out_value);
int  bp_tagdb_write_uint (bp_tagdb_t *db, const char *tag, uint16_t  value);
int  bp_tagdb_read_udint (bp_tagdb_t *db, const char *tag, uint32_t *out_value);
int  bp_tagdb_write_udint(bp_tagdb_t *db, const char *tag, uint32_t  value);
int  bp_tagdb_read_ulint (bp_tagdb_t *db, const char *tag, uint64_t *out_value);
int  bp_tagdb_write_ulint(bp_tagdb_t *db, const char *tag, uint64_t  value);

/* Floats */
int  bp_tagdb_read_real  (bp_tagdb_t *db, const char *tag, float    *out_value);
int  bp_tagdb_write_real (bp_tagdb_t *db, const char *tag, float     value);
int  bp_tagdb_read_lreal (bp_tagdb_t *db, const char *tag, double   *out_value);
int  bp_tagdb_write_lreal(bp_tagdb_t *db, const char *tag, double    value);

/* BOOL — Logix BOOL scalars are 1 byte on the wire (0 or 1).
 *
 * Note: BOOL[] ARRAYS are bit-packed in DWORDs (32 bits per word).  We do
 * NOT auto-handle that; if you need a BOOL array, read it as
 * uint32_t-array and unpack bits caller-side, or address members of a
 * struct's BOOL field by name. */
int  bp_tagdb_read_bool  (bp_tagdb_t *db, const char *tag, int *out_value);
int  bp_tagdb_write_bool (bp_tagdb_t *db, const char *tag, int  value);

/* ============================================================
 * Array helpers — one round-trip reads/writes `count` elements.
 *
 * The tag_name can include an index (e.g. "MyArr[5]") to read a
 * slice starting at that offset.  If you pass the bare array name
 * (e.g. "MyArr") with elem_count = N, you get [0..N-1].
 *
 * The caller-supplied buffer must hold count * sizeof(element)
 * bytes (e.g. uint32_t buf[N] for DINT/UDINT/REAL).
 *
 * Max practical count: ~75,000 32-bit elements (slot data area is
 * ~305 KB) — for larger arrays, batch with multiple calls.
 * ============================================================ */

int  bp_tagdb_read_sint_array  (bp_tagdb_t *db, const char *tag,
                                  int8_t *out_array, uint16_t count);
int  bp_tagdb_write_sint_array (bp_tagdb_t *db, const char *tag,
                                  const  int8_t *in_array, uint16_t count);
int  bp_tagdb_read_usint_array (bp_tagdb_t *db, const char *tag,
                                  uint8_t *out_array, uint16_t count);
int  bp_tagdb_write_usint_array(bp_tagdb_t *db, const char *tag,
                                  const uint8_t *in_array, uint16_t count);

int  bp_tagdb_read_int_array   (bp_tagdb_t *db, const char *tag,
                                  int16_t *out_array, uint16_t count);
int  bp_tagdb_write_int_array  (bp_tagdb_t *db, const char *tag,
                                  const int16_t *in_array, uint16_t count);
int  bp_tagdb_read_uint_array  (bp_tagdb_t *db, const char *tag,
                                  uint16_t *out_array, uint16_t count);
int  bp_tagdb_write_uint_array (bp_tagdb_t *db, const char *tag,
                                  const uint16_t *in_array, uint16_t count);

int  bp_tagdb_read_dint_array  (bp_tagdb_t *db, const char *tag,
                                  int32_t *out_array, uint16_t count);
int  bp_tagdb_write_dint_array (bp_tagdb_t *db, const char *tag,
                                  const int32_t *in_array, uint16_t count);
int  bp_tagdb_read_udint_array (bp_tagdb_t *db, const char *tag,
                                  uint32_t *out_array, uint16_t count);
int  bp_tagdb_write_udint_array(bp_tagdb_t *db, const char *tag,
                                  const uint32_t *in_array, uint16_t count);

int  bp_tagdb_read_lint_array  (bp_tagdb_t *db, const char *tag,
                                  int64_t *out_array, uint16_t count);
int  bp_tagdb_write_lint_array (bp_tagdb_t *db, const char *tag,
                                  const int64_t *in_array, uint16_t count);
int  bp_tagdb_read_ulint_array (bp_tagdb_t *db, const char *tag,
                                  uint64_t *out_array, uint16_t count);
int  bp_tagdb_write_ulint_array(bp_tagdb_t *db, const char *tag,
                                  const uint64_t *in_array, uint16_t count);

int  bp_tagdb_read_real_array  (bp_tagdb_t *db, const char *tag,
                                  float *out_array, uint16_t count);
int  bp_tagdb_write_real_array (bp_tagdb_t *db, const char *tag,
                                  const float *in_array, uint16_t count);
int  bp_tagdb_read_lreal_array (bp_tagdb_t *db, const char *tag,
                                  double *out_array, uint16_t count);
int  bp_tagdb_write_lreal_array(bp_tagdb_t *db, const char *tag,
                                  const double *in_array, uint16_t count);

/* ============================================================
 * STRING — Allen-Bradley Logix STRING (LEN:DINT + DATA:SINT[82]).
 *
 * bp_tagdb_read_string reads tag.LEN (DINT) and tag.DATA (SINT[]),
 * writes up to (out_size - 1) bytes of DATA into out_buf, NUL-
 * terminates, and stores the original LEN value in *out_len if
 * non-NULL.  If the string is longer than out_size, the output is
 * truncated (the on-PLC LEN is unchanged).
 *
 * bp_tagdb_write_string writes in_len bytes of in_data to tag.DATA
 * and sets tag.LEN to in_len.  Max in_len is 82 (Logix default
 * STRING length).
 *
 * Both calls do TWO IPC roundtrips internally (LEN and DATA are
 * accessed separately).
 * ============================================================ */
int  bp_tagdb_read_string (bp_tagdb_t *db, const char *tag,
                           char *out_buf, size_t out_size,
                           size_t *out_len);
int  bp_tagdb_write_string(bp_tagdb_t *db, const char *tag,
                           const char *in_data, size_t in_len);

#ifdef __cplusplus
}
#endif

#endif /* BPCLIENT_H */
