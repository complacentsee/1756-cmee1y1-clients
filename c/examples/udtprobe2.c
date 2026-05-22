/*
 * udtprobe2.c — exercise dot-notation member access against known
 * UDTs (CONTROL has standard members) and try a few guesses against
 * an unknown UDT.
 *
 * SPDX-License-Identifier: MIT
 */
#include <stdio.h>
#include <string.h>
#include "bpclient.h"

static void try_dint(bp_tagdb_t *db, const char *tag) {
    int32_t v;
    int rc = bp_tagdb_read_dint(db, tag, &v);
    if (rc == BP_OK) printf("  %-60s = %d  (0x%08x)\n", tag, v, (uint32_t)v);
    else             printf("  %-60s -> %s (%d)\n", tag, bp_strerror(rc), rc);
}
static void try_bool(bp_tagdb_t *db, const char *tag) {
    int v;
    int rc = bp_tagdb_read_bool(db, tag, &v);
    if (rc == BP_OK) printf("  %-60s = %d\n", tag, v);
    else             printf("  %-60s -> %s (%d)\n", tag, bp_strerror(rc), rc);
}
static void try_sint(bp_tagdb_t *db, const char *tag) {
    int8_t v;
    int rc = bp_tagdb_read_sint(db, tag, &v);
    if (rc == BP_OK) printf("  %-60s = %d\n", tag, v);
    else             printf("  %-60s -> %s (%d)\n", tag, bp_strerror(rc), rc);
}

int main(void) {
    bp_client_t *cl;
    if (bp_client_open(&cl) != BP_OK) return 2;
    bp_client_open_session(cl, NULL);
    bp_tagdb_t *db;
    if (bp_tagdb_open(cl, "P:1,S:2", &db) != BP_OK) { bp_client_close(cl); return 2; }
    uint16_t n; bp_tagdb_build(db, &n);
    printf("path=P:1,S:2 symbols=%u\n\n", n);

    printf("=== CONTROL members on Tran_To_iSeries_FIFO_Load_Control "
           "(known Logix struct) ===\n");
    try_dint(db, "Tran_To_iSeries_FIFO_Load_Control.LEN");
    try_dint(db, "Tran_To_iSeries_FIFO_Load_Control.POS");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.EN");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.DN");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.ER");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.EM");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.UL");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.UN");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.IN");
    try_bool(db, "Tran_To_iSeries_FIFO_Load_Control.FD");

    printf("\n=== Custom UDT 0x23 — guess common member names ===\n");
    const char *guesses[] = {
        ".Data", ".DATA", ".data",
        ".Index", ".INDEX", ".index",
        ".Count", ".COUNT", ".Length", ".Len",
        ".Status", ".STATUS",
        ".Active", ".Ready", ".Done", ".Valid",
        ".Header", ".Type", ".Id",
        NULL,
    };
    for (int i = 0; guesses[i]; i++) {
        char full[256];
        snprintf(full, sizeof(full), "Tran_To_iSeries_FIFO_Loader%s", guesses[i]);
        try_dint(db, full);
    }

    /* If we know AB-style FIFO instructions: maybe the UDT has STRING members */
    printf("\n=== Try reading byte 0..7 of the UDT as individual SINTs ===\n");
    try_sint(db, "Tran_To_iSeries_FIFO_Loader[0]");  /* maybe array of SINTs if it's actually a STRING-like? */
    try_dint(db, "Tran_To_iSeries_FIFO_Loader[0]");

    bp_tagdb_close(db); bp_client_close(cl);
    return 0;
}
