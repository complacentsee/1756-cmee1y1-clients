/*
 * multitag.c — bp_tagdb_read_tags (v0.9.0 Phase 2).
 *
 * Resolves each name via the per-client symbol cache, batches every
 * request into one OCXcip_AccessTagData round-trip, and decodes per-tag
 * results into the variant bp_value_t.
 *
 * Scope (v0.9.0):
 *   - Scalars only.  Arrays (dim0 != 0), structs / UDTs (struct_type
 *     != 0; covers the Logix STRING family) → BP_ERR_PARAM_RANGE.
 *
 * Whole-batch error semantics: if any per-tag CIP General Status comes
 * back non-zero, bp_tagdb_read_tags returns BP_ERR_GENERIC.  Callers
 * inspect each out_values[i].cip_status to find which tag(s) failed.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdlib.h>
#include <string.h>

#include "../include/bpclient.h"
#include "proto.h"

void bp_value_clear(bp_value_t *v) {
    if (!v) return;
    if (v->kind == BP_VAL_STRING && v->v.string.data) {
        free(v->v.string.data);
    }
    memset(v, 0, sizeof(*v));
}

/* Map an atomic data_type to its variant kind + bytes.  Returns
 * BP_VAL_NONE if the type isn't supported in v0.9.0. */
static bp_value_kind_t kind_for_atomic(uint16_t data_type, uint32_t elem_byte_size) {
    switch (data_type & 0x1FFFu) {
    case BP_TYPE_BOOL:  return elem_byte_size == 1 ? BP_VAL_BOOL  : BP_VAL_NONE;
    case BP_TYPE_SINT:  return elem_byte_size == 1 ? BP_VAL_SINT  : BP_VAL_NONE;
    case BP_TYPE_INT:   return elem_byte_size == 2 ? BP_VAL_INT   : BP_VAL_NONE;
    case BP_TYPE_DINT:  return elem_byte_size == 4 ? BP_VAL_DINT  : BP_VAL_NONE;
    case BP_TYPE_LINT:  return elem_byte_size == 8 ? BP_VAL_LINT  : BP_VAL_NONE;
    case BP_TYPE_USINT: return elem_byte_size == 1 ? BP_VAL_USINT : BP_VAL_NONE;
    case BP_TYPE_UINT:  return elem_byte_size == 2 ? BP_VAL_UINT  : BP_VAL_NONE;
    case BP_TYPE_UDINT: return elem_byte_size == 4 ? BP_VAL_UDINT : BP_VAL_NONE;
    case BP_TYPE_ULINT: return elem_byte_size == 8 ? BP_VAL_ULINT : BP_VAL_NONE;
    case BP_TYPE_REAL:  return elem_byte_size == 4 ? BP_VAL_REAL  : BP_VAL_NONE;
    case BP_TYPE_LREAL: return elem_byte_size == 8 ? BP_VAL_LREAL : BP_VAL_NONE;
    }
    return BP_VAL_NONE;
}

/* Decode `nbytes` of raw data into `out` according to `kind`. */
static void decode_scalar(bp_value_t *out, bp_value_kind_t kind,
                           const uint8_t *bytes) {
    out->kind = kind;
    switch (kind) {
    case BP_VAL_BOOL:  out->v.boolean = bytes[0] != 0; break;
    case BP_VAL_SINT:  out->v.sint    = (int8_t)bytes[0]; break;
    case BP_VAL_USINT: out->v.usint   = bytes[0]; break;
    case BP_VAL_INT:   out->v.int_    = (int16_t)bp_ld_u16(bytes); break;
    case BP_VAL_UINT:  out->v.uint    = bp_ld_u16(bytes); break;
    case BP_VAL_DINT:  out->v.dint    = (int32_t)bp_ld_u32(bytes); break;
    case BP_VAL_UDINT: out->v.udint   = bp_ld_u32(bytes); break;
    case BP_VAL_REAL: {
        uint32_t u = bp_ld_u32(bytes);
        memcpy(&out->v.real, &u, 4);
        break;
    }
    case BP_VAL_LINT: {
        uint64_t lo = bp_ld_u32(bytes);
        uint64_t hi = bp_ld_u32(bytes + 4);
        out->v.lint = (int64_t)(lo | (hi << 32));
        break;
    }
    case BP_VAL_ULINT: {
        uint64_t lo = bp_ld_u32(bytes);
        uint64_t hi = bp_ld_u32(bytes + 4);
        out->v.ulint = lo | (hi << 32);
        break;
    }
    case BP_VAL_LREAL: {
        uint64_t lo = bp_ld_u32(bytes);
        uint64_t hi = bp_ld_u32(bytes + 4);
        uint64_t u = lo | (hi << 32);
        memcpy(&out->v.lreal, &u, 8);
        break;
    }
    default:
        out->kind = BP_VAL_NONE;
        break;
    }
}

int bp_tagdb_read_tags(bp_tagdb_t *db,
                        const char *const *names, size_t count,
                        bp_value_t *out_values) {
    if (!db || !names || !out_values) return BP_ERR_NULL_ARG;
    if (count == 0) return BP_OK;

    /* Per-name working state: resolved symbol + the kind we'll decode
     * into.  Allocated once; we use it to size the per-request data
     * buffer in the AccessTagData batch. */
    bp_tag_request_t *requests = calloc(count, sizeof(*requests));
    bp_value_kind_t  *kinds    = calloc(count, sizeof(*kinds));
    uint8_t         **bufs     = calloc(count, sizeof(*bufs));
    if (!requests || !kinds || !bufs) {
        free(requests); free(kinds); free(bufs);
        return BP_ERR_CLIENT_OPEN;
    }

    int rc = BP_OK;
    for (size_t i = 0; i < count; i++) {
        memset(&out_values[i], 0, sizeof(out_values[i]));

        bp_symbol_info_t sym;
        rc = bp_tagdb_lookup_symbol(db, names[i], &sym);
        if (rc != BP_OK) goto cleanup;

        /* v0.9.0 supports scalars only.  Arrays + UDTs reject early. */
        if (sym.dim0 != 0 || sym.struct_type != 0) {
            rc = BP_ERR_PARAM_RANGE;
            goto cleanup;
        }
        bp_value_kind_t kind = kind_for_atomic(sym.data_type, sym.elem_byte_size);
        if (kind == BP_VAL_NONE) {
            rc = BP_ERR_PARAM_RANGE;
            goto cleanup;
        }
        kinds[i] = kind;

        bufs[i] = calloc(1, sym.elem_byte_size);
        if (!bufs[i]) { rc = BP_ERR_CLIENT_OPEN; goto cleanup; }

        requests[i].tag_name       = names[i];
        requests[i].data_type      = sym.data_type;
        requests[i].elem_byte_size = (uint16_t)sym.elem_byte_size;
        requests[i].action         = BP_ACTION_READ;
        requests[i].elem_count     = 1;
        requests[i].data           = bufs[i];
        requests[i].result         = 0;
    }

    /* Single batched AccessTagData call.  rc is the slot-level error
     * (0 = the engine processed the batch; per-tag rc lives in
     * requests[i].result). */
    rc = bp_tagdb_access(db, requests, count);
    if (rc != BP_OK) goto cleanup;

    /* Decode + populate out_values.  Track whether any per-tag CIP
     * status was non-zero so we can return whole-batch failure. */
    int any_failed = 0;
    for (size_t i = 0; i < count; i++) {
        out_values[i].cip_status = requests[i].result;
        if (requests[i].result != 0) {
            any_failed = 1;
            continue;
        }
        decode_scalar(&out_values[i], kinds[i], bufs[i]);
    }
    rc = any_failed ? BP_ERR_GENERIC : BP_OK;

cleanup:
    for (size_t i = 0; i < count; i++) free(bufs[i]);
    free(requests); free(kinds); free(bufs);
    return rc;
}
