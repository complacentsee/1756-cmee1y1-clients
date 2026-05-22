/*
 * modutil.c — exercise the local cm1756 module utilities:
 *   * Print the current rotary switch position
 *   * Print the current 4-char display contents
 *   * Optionally set the display via --display "ABCD"
 *   * Optionally toggle an LED via --led <id> --state <0|1>
 *   * Print the current LED state of a single LED via --led-get <id>
 *
 * SPDX-License-Identifier: MIT
 */
#include <getopt.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bpclient.h"

int main(int argc, char *argv[]) {
    const char *set_display = NULL;
    int led_set_id = -1, led_set_state = -1;
    int led_get_id = -1;

    static struct option opts[] = {
        {"display",  required_argument, 0, 'd'},
        {"led",      required_argument, 0, 'l'},
        {"state",    required_argument, 0, 's'},
        {"led-get",  required_argument, 0, 'g'},
        {0,0,0,0}
    };
    int c, idx;
    while ((c = getopt_long(argc, argv, "d:l:s:g:", opts, &idx)) != -1) {
        if      (c == 'd') set_display   = optarg;
        else if (c == 'l') led_set_id    = (int)strtol(optarg, NULL, 0);
        else if (c == 's') led_set_state = (int)strtol(optarg, NULL, 0);
        else if (c == 'g') led_get_id    = (int)strtol(optarg, NULL, 0);
    }

    bp_client_t *cl = NULL;
    if (bp_client_open(&cl) != BP_OK) {
        fprintf(stderr, "client open failed\n"); return 2;
    }
    bp_client_open_session(cl, NULL);

    uint32_t sw = 0;
    int rc = bp_client_get_switch_position(cl, &sw);
    printf("[switch] rc=%d (%s)  position=%u (0x%08x)\n", rc, bp_strerror(rc), sw, sw);

    char disp[5] = {0};
    rc = bp_client_get_display(cl, disp);
    printf("[display read] rc=%d (%s)  text='%s' (hex %02x %02x %02x %02x)\n",
           rc, bp_strerror(rc), disp,
           (uint8_t)disp[0], (uint8_t)disp[1], (uint8_t)disp[2], (uint8_t)disp[3]);

    if (set_display) {
        char four[4] = {' ',' ',' ',' '};
        size_t n = strlen(set_display);
        if (n > 4) n = 4;
        memcpy(four, set_display, n);
        rc = bp_client_set_display(cl, four);
        printf("[display set]  rc=%d (%s)  wrote '%c%c%c%c'\n", rc, bp_strerror(rc),
               four[0], four[1], four[2], four[3]);
        char back[5] = {0};
        if (bp_client_get_display(cl, back) == BP_OK)
            printf("[display read-after-write]  text='%s'\n", back);
    }

    if (led_get_id >= 0) {
        uint32_t s = 0;
        rc = bp_client_get_led(cl, (uint32_t)led_get_id, &s);
        printf("[led get] rc=%d (%s)  id=%d  state=%u (0x%08x)\n",
               rc, bp_strerror(rc), led_get_id, s, s);
    }

    if (led_set_id >= 0 && led_set_state >= 0) {
        rc = bp_client_set_led(cl, (uint32_t)led_set_id, (uint32_t)led_set_state);
        printf("[led set] rc=%d (%s)  id=%d -> state=%d\n", rc, bp_strerror(rc),
               led_set_id, led_set_state);
        uint32_t s = 0;
        if (bp_client_get_led(cl, (uint32_t)led_set_id, &s) == BP_OK)
            printf("[led read-after-write] id=%d  state=%u\n", led_set_id, s);
    }

    bp_client_close(cl);
    return 0;
}
