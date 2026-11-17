/*
 * Early UART0 + LK framebuffer text console + TOPRGU watchdog control.
 */
#ifndef CONSOLE_H
#define CONSOLE_H

#include <stdint.h>

struct fb_info {
    uint64_t base;
    uint32_t size;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint32_t ovl_active;
};

void wdt_disable(void);
void __attribute__((noreturn)) wdt_warm_reset(void);

uint64_t timer_ticks(void);
uint64_t timer_freq(void);
void delay_ms(uint32_t ms);

void console_init(uintptr_t fdt, uint32_t scale);
const struct fb_info *console_fb(void);
void console_strip_ovl_alpha(void);
void console_set_fg(uint32_t argb);
void console_clear(uint32_t argb);

void con_putc(char c);
void con_puts(const char *s);
void con_hex(uint64_t v);
void con_dec(uint64_t v);

#endif /* CONSOLE_H */
