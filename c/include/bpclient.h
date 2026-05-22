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
#define BP_ERR_CIP_STATUS     -400   /* CIP-layer rejection (general_status != 0).
                                       * Transport succeeded; the PLC's CIP target
                                       * returned a non-zero general_status.  Fetch
                                       * the structured fields with
                                       * bp_client_last_cip_error(). */
#define BP_ERR_CLIENT_OPEN -101802
#define BP_ERR_NO_FREE_SLOT -103001

/* Return a human-readable string for a BP_ERR_* code. Always returns
 * a pointer to static storage; never NULL.  Unknown codes return
 * "unknown error". */
const char *bp_strerror(int rc);

/* ============================================================
 * Structured CIP-layer errors
 *
 * Some SDK calls (the TxRx* connected-messaging family in v0.7.0+,
 * and any future call that wraps a CIP service request) succeed at
 * the transport layer but receive a non-zero CIP general_status from
 * the PLC.  In that case the call returns BP_ERR_CIP_STATUS and the
 * structured fields can be retrieved by calling
 * bp_client_last_cip_error() before the next CIP-layer operation on
 * this client.
 *
 * The "service" field is the reply service byte (e.g. 0xDB for an
 * LFO reply, 0xCE for an FC reply, request_service | 0x80 for any
 * other CIP service).  "status" is the CIP General Status byte
 * (table in docs/error-codes.md).  "ext_status" is the first 16-bit
 * extended-status word (0 if the reply carried no ext-status).
 * "slot" is the backplane slot the request targeted.
 * ============================================================ */
typedef struct {
    uint8_t  service;     /* reply service byte */
    uint8_t  status;      /* CIP general_status */
    uint16_t ext_status;  /* CIP extended_status (0 if absent) */
    uint8_t  slot;        /* backplane slot that received the request */
    uint8_t  _reserved[3];
} bp_cip_status_t;

/* bp_client_last_cip_error
 *   Fills *out with the most recently recorded CIP-layer rejection
 *   on this client and returns BP_OK.  Returns BP_ERR_GENERIC if no
 *   CIP error has been recorded since the last call to this function
 *   (reading clears the recorded value).
 *
 *   Recording is per-client and not thread-local: if two threads call
 *   different TxRx functions concurrently and both fail at the CIP
 *   layer, only one of the two CIP errors will be observable.  For
 *   that case, the BP_ERR_CIP_STATUS return value is still
 *   distinguishable; callers needing per-call detail should
 *   serialize CIP-layer calls in that thread or copy the result
 *   immediately. */
int bp_client_last_cip_error(bp_client_t *client, bp_cip_status_t *out);

/* bp_cip_status_string
 *   Returns a human-readable string for a (status, ext_status) pair.
 *   Returns a pointer to static storage; never NULL.  Unknown pairs
 *   return "unknown CIP status".  Mirrors the table in
 *   docs/error-codes.md "Forward_Open / Forward_Close failure modes". */
const char *bp_cip_status_string(uint8_t status, uint16_t ext_status);

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
#define BP_TYPE_BIT_ARRAY 0xD3   /* Logix BOOL[] packed 32 bits per DWORD */

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
 * Unconnected (UCMM) CIP messaging — bp_client_message_send
 *
 * Sends one UCMM CIP request to a chosen backplane slot and returns
 * the raw CIP response.  Equivalent to apex2_asic_send_ucmm in the
 * sibling apex2d daemon — the cm1756 chip firmware does the wire
 * framing; the host just supplies (slot, full CIP request, timeout).
 *
 * Wire format (RE'd from OC_bpMessageSend in libocxbpeng.so.2.3 at
 * 0x19bf84 + um_ProcessClientRequest at 0x18c518; corroborated by the
 * historianupdate apex2d daemon's apex2_asic_send_ucmm):
 *
 *   - `slot`        is the BACKPLANE SLOT NUMBER (not a CIP service);
 *                   engine validates < 0x14 (20).  The chip writes
 *                   it to CB+0x1C and the ASIC firmware does the
 *                   UCMM routing.  Slot 0 = first device in chassis
 *                   (may be a Historian / power supply / etc.).
 *   - `cip_request` is the COMPLETE CIP request body byte stream:
 *                       [service, path_size_words, path..., body...]
 *                   It is copied verbatim into the UCMM transmit
 *                   buffer.  No port segments are prepended.
 *   - `timeout_ms`  is the per-attempt timeout in milliseconds
 *                   (engine clamps min to 26 ms).
 *
 * Routing note: there is NO "default target" state on the chip — the
 * slot byte is the only routing input.  Earlier confusion (T9/T13
 * returning L73 for "any" path) was caused by passing the CIP service
 * 0x01 in the field we now call `slot`, which addressed slot 1 = L73.
 *
 * CIP response: parse resp_data as
 *     [reply_service (= req_service | 0x80), reserved, general_status,
 *      ext_status_size_words, ext_status[ext_size*2], body...]
 *
 * For routing OFF-chassis (DH+, ControlNet, EtherNet/IP), embed an
 * Unconnected_Send (service 0x52) in cip_request with the route path
 * inside the embedded message — the chip just forwards the bytes.
 * ============================================================ */

#define BP_MSG_MAX_SLOT     0x13    /* engine validates < 0x14 (20) */
#define BP_MSG_MAX_REQ      500     /* engine validates path_size <= 500 */
#define BP_MSG_MIN_TIMEOUT  26      /* engine clamps below this (ms) */

typedef struct {
    uint8_t        slot;           /* IN: backplane slot 0..0x13 */
    const uint8_t *cip_request;    /* IN: full CIP request bytes */
    uint16_t       req_size;       /* IN: byte count of cip_request, 1..500 */
    uint16_t       timeout_ms;     /* IN: per-attempt timeout (0 → engine min 26 ms) */
    void          *resp_data;      /* IN: caller-allocated response buffer */
    uint16_t       resp_capacity;  /* IN: capacity of resp_data (MUST be > 0) */
    uint16_t       resp_len;       /* OUT: actual bytes server wrote */
    uint32_t       status;         /* OUT: wrapper status field (0 on success) */
} bp_message_t;

/* bp_client_message_send
 *   Returns BP_OK if the message round-tripped, regardless of CIP
 *   General Status — caller must inspect the first ~4 bytes of
 *   resp_data for the CIP response header.  Negative return =
 *   transport / parameter error.  Positive returns are engine codes:
 *     1  = bad param (e.g. slot >= 0x14)
 *     3  = engine refused / target unresponsive (empty slot)
 *     14 = retry budget exhausted (raise timeout_ms)
 */
int bp_client_message_send(bp_client_t *client, bp_message_t *msg);

/* ============================================================
 * CIP Identity / device queries
 * ============================================================ */

/* CIP Identity Object (class 0x01).  48 bytes / 6 qwords on the wire. */
typedef struct {
    uint16_t vendor_id;
    uint16_t device_type;
    uint16_t product_code;
    uint8_t  major_rev;
    uint8_t  minor_rev;
    uint16_t status;
    uint32_t serial_number;
    uint8_t  product_name[32];   /* SHORT_STRING padded with NULs */
} bp_id_object_t;

/* bp_client_get_id_local
 *   Returns the LOCAL cm1756 module's Identity object.  Wraps
 *   OCXcip_GetIdObject — no path, no class word, just dispatches
 *   and reads back the 48-byte struct. */
int bp_client_get_id_local(bp_client_t *client, bp_id_object_t *out);

/* bp_client_get_device_id
 *   Returns the Identity of the device addressed by `text_path`
 *   (OldI format, e.g. "P:1,S:2").  Wraps OCXcip_GetDeviceIdObject;
 *   the OEM library does the path parsing internally, so this is
 *   the safest way to address a remote device when you're not sure
 *   about CIP EPATH encoding edge cases.
 *   `instance` is normally 1 (Identity instance 1). */
int bp_client_get_device_id(bp_client_t *client,
                             const char *text_path,
                             uint16_t instance,
                             bp_id_object_t *out);

/* bp_client_get_active_nodes
 *   Wraps OCXcip_GetActiveNodeTable: returns a 64-bit bitmap of
 *   nodes the wrapper sees as responsive.  bit N in (mask_low |
 *   (mask_high << 32)) corresponds to node N. */
int bp_client_get_active_nodes(bp_client_t *client,
                                uint32_t *out_mask_low,
                                uint32_t *out_mask_high);

/* ============================================================
 * Module-side utilities — LED / Display / Switch position
 *
 * These act on the local cm1756 (the EEC card itself).  They do
 * NOT route to other backplane devices.
 * ============================================================ */

/* bp_client_get_switch_position
 *   Returns the front-panel rotary switch reading (or 0 if no
 *   switch / firmware doesn't expose it). */
int bp_client_get_switch_position(bp_client_t *client, uint32_t *out_value);

/* bp_client_get_led / bp_client_set_led
 *   LED identifier and state are vendor-defined uint32 codes.
 *   Refer to ASEM/Rockwell EEC docs for the LED IDs available on
 *   your hardware revision. */
int bp_client_get_led(bp_client_t *client, uint32_t led_id, uint32_t *out_state);
int bp_client_set_led(bp_client_t *client, uint32_t led_id, uint32_t  state);

/* bp_client_get_display / bp_client_set_display
 *   Read or write the 4-character module display.  Pass a 4-byte
 *   buffer in `four_chars`; for set, all 4 bytes are written
 *   verbatim (the engine appends a NUL on the wire).  For get,
 *   the result buffer must be at least 5 bytes (4 chars + NUL).
 *
 *   Note: on some firmware revisions Set is silently ignored by
 *   the engine — call Get afterward to verify if exactness matters. */
int bp_client_get_display(bp_client_t *client, char out_five_chars[5]);
int bp_client_set_display(bp_client_t *client, const char four_chars[4]);

/* ============================================================
 * Connected (class-3) CIP messaging — TxRxOpenConn/Msg/CloseConn
 *
 * v0.7.0+: FUNCTIONAL.  Internally drives a Large Forward Open
 * (CIP service 0x5B) + bare CIP request + Forward_Close (0x4E),
 * all sent via bp_client_message_send (chip mailbox 0x200, UCMM
 * transport).  The OCXcip_TxRx* OEM entry points are bypassed —
 * they dispatch to OCXCN_OpenClass3Connection in an external
 * library that is not present on the cm1756 image (Ghidra RE of
 * libocxbpapi.so.2.3 @ 0x106f44).
 *
 * Wire format documented in docs/protocol.md "Connected messaging
 * — wire format".  The CIP request body passed to txrx_msg is sent
 * to the PLC byte-for-byte — no sequence-number prepending, no
 * connection-ID embedding.  The PLC's connection state machine
 * tracks the connection lifecycle on its end.
 *
 * STATE LIFECYCLE
 *   txrx_open(spec)  → Forward_Open, caches state keyed by
 *                       spec->app_handle inside the bp_client.
 *   txrx_msg(spec)   → looks up the cached state, sends request
 *                       via UCMM transport.  Returns BP_ERR_NOT_OPEN
 *                       if no matching open.
 *   txrx_close(spec) → Forward_Close, frees the cached state.
 *
 * Concurrent open() with the same app_handle on the same client
 * returns BP_ERR_GENERIC; close() then re-open() is the correct
 * pattern.  Up to BP_TXRX_MAX_CONNS (16) connections per client.
 *
 * KNOWN LIMITATION (v0.7.0)
 *
 * The transport routes through mailbox 0x200 (UCMM), capped at
 * BP_MSG_MAX_REQ (500 bytes).  The chip's mailbox 0x204 connected-
 * data path (4002-byte packets) is not reachable through
 * OCXcip_MessageSend — see docs/v0.8-large-buffer-re.md for the
 * future-work plan.  Callers needing larger payloads must chunk.
 *
 * VALIDATED ROUTES (v0.7.0)
 *
 * Only the canonical backplane-direct route is supported:
 *   spec->encoded_path = { 0x01, slot }      port 1 = backplane
 *   spec->path_size    = 2
 * Multi-hop routes are not in v0.7.0; off-chassis targeting needs
 * Unconnected_Send (svc 0x52) embedded inside the txrx_msg request
 * body, not the route bytes.
 * ============================================================ */

/* Connection spec — caller-managed, passed to every TxRx call.
 * The same fields go to OpenConn, TxRxMsg, CloseConn unchanged
 * (the OEM API duplicates them across calls — we follow suit). */
typedef struct {
    uint16_t       app_handle;     /* caller-assigned ID; reuse the same value across the lifecycle */
    uint32_t       options;        /* vendor flags, normally 0 */
    const uint8_t *encoded_path;   /* route to target — e.g. {0x01, 0x02} for backplane slot 2 */
    uint16_t       path_size;      /* byte count of encoded_path (< 256) */
    uint16_t       conn_params;    /* vendor conn params; normally 0 (engine uses defaults) */
} bp_conn_spec_t;

/* bp_client_txrx_open
 *   Sends a Large Forward Open (CIP svc 0x5B) to the slot encoded
 *   in spec->encoded_path (must be {0x01, slot}; see "VALIDATED
 *   ROUTES" above).  On success, caches the connection state on
 *   the bp_client keyed by spec->app_handle and returns:
 *     *out_conn_id     = low 16 of the PLC-assigned O→T conn ID
 *                        (e.g. 0x0435 for an 0x80020435 conn ID)
 *     *out_conn_serial = the random 16-bit serial we sent in the LFO
 *                        (echoed by the PLC's Forward_Close reply)
 *   Either pointer may be NULL.  spec->conn_params, if non-zero,
 *   sets the O→T / T→O size in BYTES (LFO 32-bit conn-params
 *   low 16); 0 → 4000 (matches OCX).  Hardware max 4002.  Values
 *   above 4002 are capped with a warning — this protects against
 *   legacy callers that still pass a stale OEM-format 16-bit
 *   conn_params (e.g. 0x43E8) where the meaningful fields no
 *   longer map cleanly into the 32-bit LFO layout. */
int bp_client_txrx_open(bp_client_t *client, const bp_conn_spec_t *spec,
                         uint16_t *out_conn_id, uint16_t *out_conn_serial);

/* bp_client_txrx_msg
 *   Sends one CIP request over the connection identified by
 *   spec->app_handle (must have been opened — else BP_ERR_NOT_OPEN).
 *   req points to the CIP request bytes (sent byte-for-byte, no
 *   sequence-number prepending — see docs/protocol.md):
 *       [service_code, path_size_words, EPATH..., body...]
 *   The response in resp[] is the raw CIP reply:
 *       [service_reply, reserved, general_status, ext_status_size,
 *        ext_status..., body...]
 *   *out_resp_size is set to the actual byte count on success.
 *
 *   v0.7.0 cap: req_size ≤ BP_MSG_MAX_REQ (500 bytes). */
int bp_client_txrx_msg(bp_client_t *client, const bp_conn_spec_t *spec,
                        const void *req, uint16_t req_size,
                        void *resp, uint16_t resp_capacity,
                        uint16_t *out_resp_size);

/* bp_client_txrx_close
 *   Sends a Forward_Close (CIP svc 0x4E) using the conn serial /
 *   vendor / orig serial cached from the matching txrx_open.
 *   Frees the cached state regardless of FC outcome — so the
 *   slot becomes available for re-open even if the FC reply
 *   indicates the PLC had already cleaned up by idle timeout.
 *   Returns BP_ERR_NOT_OPEN if no entry matches spec->app_handle. */
int bp_client_txrx_close(bp_client_t *client, const bp_conn_spec_t *spec);

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

/* bp_tagdb_test_version
 *   Asks the PLC whether the tag database has changed since the
 *   last bp_tagdb_build() on this handle.  Cheap (~5 ms) compared
 *   to a full Build (~200 ms for a few thousand symbols), so the
 *   recommended pattern is:
 *
 *      bp_tagdb_build(db, NULL);            // once after open
 *      ...
 *      int changed = 0;
 *      if (bp_tagdb_test_version(db, &changed) == BP_OK && changed) {
 *          bp_tagdb_build(db, NULL);        // refresh only when needed
 *      }
 *
 *   On BP_OK return, *out_changed is 0 if the version matches the
 *   one captured by the last Build, or 1 if it differs OR if no
 *   Build has been done on this handle yet (so the caller should
 *   Build either way).
 *
 *   Returns BP_OK + sets *out_changed on success.  Negative values
 *   are surfaced unchanged for IPC / system errors. */
int  bp_tagdb_test_version(bp_tagdb_t *db, int *out_changed);

/* Symbol descriptor.  Returned by bp_tagdb_symbol_at(). */
typedef struct {
    char     name[100];      /* NUL-terminated, up to 99 chars */
    uint16_t data_type;      /* CIP type code (mask with 0x1FFF for low bits) */
    uint16_t struct_type;    /* 0 for atomic scalars; non-zero = UDT template id */
    uint32_t elem_byte_size; /* bytes per element (or struct byte size for UDTs) */
    uint32_t dim0;           /* outer dimension; 0 if scalar.  For DINT[5,3] this is 5. */
    uint32_t dim1;           /* second dimension; 0 if 1-D or scalar.  For DINT[5,3] this is 3. */
    uint32_t dim2;           /* third dimension; 0 if rank < 3.  For DINT[5,10,30] this is 30.
                              * (Earlier revisions of this struct named this field "instance_id"
                              * because the wrapper docs assumed only 2-D arrays were possible;
                              * empirical 3-D dump showed this slot holds dim2.) */
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

/* Array rank: 0 for scalar, 1 for DINT[N], 2 for DINT[N,M], 3 for
 * DINT[N,M,K].  Logix supports a maximum of 3 dimensions in tag
 * declarations. */
static inline int bp_symbol_rank(const bp_symbol_info_t *info) {
    if (!info)            return 0;
    if (info->dim2 != 0)  return 3;
    if (info->dim1 != 0)  return 2;
    if (info->dim0 != 0)  return 1;
    return 0;
}

/* Total element count: 1 for scalar, dim0 for 1-D, dim0*dim1 for 2-D,
 * dim0*dim1*dim2 for 3-D. */
static inline uint32_t bp_symbol_total_elements(const bp_symbol_info_t *info) {
    if (!info)            return 0;
    uint32_t d0 = info->dim0 ? info->dim0 : 1u;
    uint32_t d1 = info->dim1 ? info->dim1 : 1u;
    uint32_t d2 = info->dim2 ? info->dim2 : 1u;
    return d0 * d1 * d2;
}

/* ============================================================
 * UDT (Structure) discovery
 *
 * For a tag with bp_symbol_is_struct(info) == 1, info->struct_type
 * is the index into the PLC's struct template table.  Use these
 * functions to enumerate the template's members so you can build
 * dotted accessors at runtime.
 *
 * Members are addressed by index 0..(n_members - 1).  Each member's
 * `name` field (combined with the parent tag name as
 * "parent.member") can be passed directly to ReadDINT/ReadString
 * etc.
 *
 * Nested UDTs: if a member's struct_id is non-zero, it's itself a
 * UDT; recurse with that struct_id to enumerate its members.
 * ============================================================ */

typedef struct {
    char     name[40];      /* UDT name, NUL-terminated */
    uint32_t data_type;     /* wire-level data_type for tags of this struct
                             * (e.g. 0x4527 for a 5-member user UDT;
                             * 0x0fce for the standard STRING family) */
    uint32_t byte_size;     /* total struct size in bytes */
    uint32_t n_members;     /* number of members */
} bp_struct_info_t;

typedef struct {
    char     name[44];      /* member name, NUL-terminated */
    uint16_t data_type;     /* CIP type code (BP_TYPE_*) for atomic
                             * members; 0x0fxx for struct-typed members */
    uint16_t struct_id;     /* if non-zero, this member is a UDT —
                             * recurse with this id for its layout */
    uint32_t byte_size;     /* member size in bytes (per element if array) */
    uint32_t offset;        /* offset within parent struct */
    uint32_t array_count;   /* 0 if scalar; N if SINT[N], DINT[N], etc.
                             * For 2+ dim arrays only the first dim is
                             * surfaced; we haven't characterized higher
                             * dims yet — see docs/udt.md */
    uint8_t  flags;         /* observed values:
                             *   0x45 = atomic scalar
                             *   0x41 = struct member
                             *   0x49 = atomic array
                             * Bit 0x08 in flags = "array" indicator. */
} bp_struct_member_info_t;

/* Member-info accessors */
static inline int bp_member_is_array (const bp_struct_member_info_t *m) {
    return m ? (int)((m->flags & 0x08) != 0) : 0;
}
static inline int bp_member_is_struct(const bp_struct_member_info_t *m) {
    return m ? (int)(m->struct_id != 0) : 0;
}

/* bp_tagdb_get_struct_info — fetch a UDT template descriptor by id. */
int bp_tagdb_get_struct_info(bp_tagdb_t *db, uint16_t struct_id,
                              bp_struct_info_t *out_info);

/* bp_tagdb_get_struct_member — fetch one member descriptor by index. */
int bp_tagdb_get_struct_member(bp_tagdb_t *db, uint16_t struct_id,
                                uint16_t member_index,
                                bp_struct_member_info_t *out_member);

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

/* BOOL — Logix BOOL scalars are 1 byte on the wire (0 or 1). */
int  bp_tagdb_read_bool  (bp_tagdb_t *db, const char *tag, int *out_value);
int  bp_tagdb_write_bool (bp_tagdb_t *db, const char *tag, int  value);

/* BOOL arrays — Logix packs BOOL[N] as ceil(N/32) DWORDs on the wire,
 * exposed as CIP type 0xD3 (BIT_ARRAY).  These helpers convert between
 * that wire form and a caller-supplied uint8_t array (one byte per
 * BOOL, 0 or 1).
 *
 * `count` must be the array's declared dimension (e.g. 32 for
 * BOOL[32]).  The wire transfer is ceil(count/32) DWORDs internally.
 *
 * Write note: if count is not a multiple of 32, the trailing bits in
 * the last DWORD are written as zeros.  This means writes to BOOL[33]
 * will set bits 33..63 of the underlying DWORD#1 to zero.  If you
 * only want to change SOME bits, read first, modify, then write back. */
int  bp_tagdb_read_bool_array (bp_tagdb_t *db, const char *tag,
                                uint8_t *out_array, uint16_t count);
int  bp_tagdb_write_bool_array(bp_tagdb_t *db, const char *tag,
                                const uint8_t *in_array, uint16_t count);

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
 * STRING — Allen-Bradley Logix STRING family (any LEN:DINT +
 * DATA:SINT[N] UDT).  Works with the default STRING (N=82),
 * STRING_32, STRING_512, and custom STRING-shaped UDTs.
 *
 * bp_tagdb_read_string reads tag.LEN (DINT) and tag.DATA (SINT[]),
 * writes up to min(LEN, out_size - 1) bytes of DATA into out_buf,
 * NUL-terminates, and stores the actual on-PLC LEN value (which may
 * exceed out_size, indicating the result was truncated) in
 * *out_len if non-NULL.
 *
 * bp_tagdb_write_string writes in_len bytes of in_data to tag.DATA
 * and sets tag.LEN to in_len.  If in_len exceeds the destination
 * struct's DATA[] capacity, the engine returns a CIP General Status
 * error (typically 0x13 "Not enough data" or 0x15 "Too much data"),
 * surfaced as BP_ERR_GENERIC from this function.
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
