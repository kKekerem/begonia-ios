/*
 * Polling Novatek NT36672A touchscreen driver over MT6785 SPI5.
 */
#ifndef TOUCH_H
#define TOUCH_H

#include <stdint.h>

struct touch_state {
    int ready;
    int fw_uploaded;
    int down;
    uint32_t x;
    uint32_t y;
    uint16_t pid;
    uint8_t fw_ver;
    uint8_t chip_id[6];
};

/*
 * Initialize clocks/pinmux/SPI5 and probe NT36672A. If touch SRAM firmware is not
 * active after LK handoff, locates the built-in Novatek firmware inside the chained
 * Android Image (when non-zero) and performs HW-CRC host download.
 * Never fails hard: sets out->ready = 0 if touch hardware is unavailable.
 */
void touch_init(uintptr_t android_img_pa, uint64_t android_img_len, struct touch_state *out);

/* Poll current primary finger contact; updates out->down, out->x, out->y. */
void touch_poll(struct touch_state *out);

#endif /* TOUCH_H */
