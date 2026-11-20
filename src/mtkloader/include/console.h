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

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t argb);
void fb_fill_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t argb);
void fb_stroke_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t thick, uint32_t argb);
void fb_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t argb);
uint32_t fb_text_width(const char *s, uint32_t scale);
void fb_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t scale, uint32_t fg, uint32_t bg, int transparent_bg);

/*
 * Persistent log: everything the console prints is also written into a ramoops console zone.
 * begonia's MIUI 4.14 ramoops (fs/pstore/ram.c, 0x4d010000 + 0xe0000): 0x4f000 of 4 KiB dump
 * records, the kernel console zone "cprz" (0x4d05f000, 0x40000), the MTK "bconsole" zone "bprz"
 * (0x4d09f000, 0x40000; nothing in Linux writes it after boot) = PLOG zone, ftrace, pmsg.
 * DRAM survives begonia's watchdog reset (a reboot), so the next Android boot shows the loader's
 * output (and the XNU console after it) in /sys/fs/pstore/console-ramoops-2.
 */
#define PLOG_ZONE_PA    0x4D09F000ULL
#define PLOG_ZONE_SIZE  0x40000ULL
void plog_init(void);
void plog_putc(char c);
int plog_contains(const char *needle);

/*
 * One-shot boot request from Android through the kernel console zone (cprz), see console.c:
 *   echo 'LOXY_BOOT_IOS_ONCE{extra xnu boot-args}' > /dev/kmsg; reboot
 */
#define PSTORE_CONSOLE_PA       0x4D05F000ULL
#define PSTORE_CONSOLE_SIZE     0x40000ULL
#define BOOT_REQUEST_IOS        "LOXY_BOOT_IOS_ONCE"
#define BOOT_REQUEST_ARGS_MAX   200U
int pstore_request_take(const char *needle, char *args, uint32_t args_cap);

void con_putc(char c);
void con_puts(const char *s);
void con_hex(uint64_t v);
void con_dec(uint64_t v);

#endif /* CONSOLE_H */
