/*
 * Polled FIFO driver for MediaTek MT6785 SPI5 (mt6765-spi compatible) and GPIO helpers.
 */
#ifndef SPI5_H
#define SPI5_H

#include <stdint.h>

void spi5_gpio_set_mode(uint32_t pin, uint32_t mode);
void spi5_gpio_set_dir(uint32_t pin, int out);
void spi5_gpio_set_dout(uint32_t pin, int val);

/* Enable TOPCKGEN/INFRA SPI5 clocks, configure SPI5_A + TP GPIOs, and init 4.33 MHz mode0. */
void spi5_hw_init(void);

/* Execute a single continuous CS-low SPI transfer (multi-chunk PAUSE_EN when len > 32). */
int spi5_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len);

/* 25 Nov 2026: transfers that timed out, and the last one ([31:24] STATUS1, [23:16] CMD, [15:0] length) */
extern uint32_t spi5_fail_count;
extern uint32_t spi5_last_fail;

#endif /* SPI5_H */
