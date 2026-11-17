/*
 * Early output for begonia: APUART0 + MediaTek LK continuous-splash framebuffer.
 * Built without SIMD and with strict alignment (-mgeneral-regs-only -mstrict-align).
 */
#include "console.h"
#include "begonia_hw.h"
#include "fdt.h"
#include "font8x16.h"

static struct fb_info g_fb;
static uint32_t g_scale = 2;
static uint32_t g_col_x;
static uint32_t g_row_y;
static uint32_t g_fg = 0xffffffffU;
static uint32_t g_bg = 0xff0d1117U;

void wdt_disable(void)
{
    mmio_write32(TOPRGU_RESTART, TOPRGU_RESTART_KEY);
    mmio_write32(TOPRGU_MODE, TOPRGU_MODE_KEY);
}

void __attribute__((noreturn)) wdt_warm_reset(void)
{
    mmio_write32(TOPRGU_SWRST, TOPRGU_SWRST_KEY);
    for (;;) {
        __asm__ volatile("wfe");
    }
}

uint64_t timer_ticks(void)
{
    uint64_t v;
    __asm__ volatile("isb; mrs %0, cntpct_el0" : "=r"(v));
    return v;
}

uint64_t timer_freq(void)
{
    uint64_t f;
    __asm__ volatile("mrs %0, cntfrq_el0" : "=r"(f));
    return f ? f : MT6785_CNTFRQ_FALLBACK;
}

void delay_ms(uint32_t ms)
{
    uint64_t end = timer_ticks() + (timer_freq() / 1000ULL) * ms;
    while (timer_ticks() < end) {
    }
}

static void uart_putc_raw(char c)
{
    uint32_t timeout = 20000U;
    while (!(mmio_read32(MT6785_UART0_BASE + UART_LSR) & UART_LSR_THRE) && --timeout) {
    }
    mmio_write32(MT6785_UART0_BASE + UART_THR, (uint8_t)c);
}

static void fb_px(uint32_t x, uint32_t y, uint32_t c)
{
    *(volatile uint32_t *)(g_fb.base + (uint64_t)y * g_fb.pitch + (uint64_t)x * 4ULL) = c;
}

static void fb_fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
    if (!g_fb.base) {
        return;
    }
    for (uint32_t y = y0; y < y0 + h && y < g_fb.height; y++) {
        for (uint32_t x = x0; x < x0 + w && x < g_fb.width; x++) {
            fb_px(x, y, c);
        }
    }
}

/*
 * Probe live framebuffer from LK's /chosen atag,videolfb-* properties first, then check
 * OVL0 layer 0 address/pitch, falling back to xiaomi-mt6785-common.dtsi defaults.
 */
void console_init(uintptr_t fdt, uint32_t scale)
{
    uint64_t fb_lo = 0, fb_hi = 0, vram_sz = 0;

    g_scale = scale ? scale : 1U;
    g_fb.width = BEGONIA_PANEL_W;
    g_fb.height = BEGONIA_PANEL_H;
    g_fb.pitch = BEGONIA_PANEL_W * 4U;
    g_fb.base = BEGONIA_FB_FALLBACK;
    g_fb.size = (uint32_t)BEGONIA_FB_FALLBACK_SZ;
    g_fb.ovl_active = 0;

    if (fdt_chosen_u64(fdt, "atag,videolfb-fb_base_l", &fb_lo) && fb_lo != 0) {
        (void)fdt_chosen_u64(fdt, "atag,videolfb-fb_base_h", &fb_hi);
        g_fb.base = (fb_hi << 32) | (fb_lo & 0xffffffffULL);
    }
    if (fdt_chosen_u64(fdt, "atag,videolfb-vramSize", &vram_sz) && vram_sz >= (uint64_t)g_fb.pitch * g_fb.height) {
        g_fb.size = (uint32_t)vram_sz;
    }

    uint32_t ovl_addr = mmio_read32(MT6785_DISP_OVL0_BASE + OVL_L0_ADDR);
    uint32_t ovl_pitch = mmio_read32(MT6785_DISP_OVL0_BASE + OVL_L0_PITCH) & 0xffffU;
    if (ovl_addr >= 0x40000000U && ovl_pitch >= g_fb.width * 4U && ovl_pitch <= 8192U) {
        g_fb.ovl_active = 1;
        if (ovl_addr == (uint32_t)g_fb.base) {
            g_fb.pitch = ovl_pitch;
        }
    }

    g_col_x = 0;
    g_row_y = 0;
    console_clear(g_bg);
}

const struct fb_info *console_fb(void)
{
    return &g_fb;
}

void console_strip_ovl_alpha(void)
{
    if (!g_fb.ovl_active) {
        return;
    }
    uint32_t con = mmio_read32(MT6785_DISP_OVL0_BASE + OVL_L0_CON);
    uint32_t src = mmio_read32(MT6785_DISP_OVL0_BASE + OVL_SRC_CON);
    mmio_write32(MT6785_DISP_OVL0_BASE + OVL_L0_CON, con & ~(1U << 8));
    mmio_write32(MT6785_DISP_OVL0_BASE + OVL_SRC_CON, src & 1U);
    mmio_write32(DISP_MUTEX0_EN, 1U);
}

void console_set_fg(uint32_t argb)
{
    g_fg = argb;
}

void console_clear(uint32_t argb)
{
    g_bg = argb;
    g_col_x = 0;
    g_row_y = 0;
    fb_fill_rect(0, 0, g_fb.width, g_fb.height, argb);
}

static void fb_newline(void)
{
    uint32_t lh = 16U * g_scale;
    g_col_x = 0;
    g_row_y += lh;
    if (g_row_y + lh > g_fb.height) {
        g_row_y = 0;
    }
    fb_fill_rect(0, g_row_y, g_fb.width, lh, g_bg);
    if (g_row_y + 2U * lh <= g_fb.height) {
        fb_fill_rect(0, g_row_y + lh, g_fb.width, 2U, 0xff2f81f7U);
    }
}

void con_putc(char c)
{
    if (c == '\n') {
        uart_putc_raw('\r');
        uart_putc_raw('\n');
        fb_newline();
        return;
    }
    uart_putc_raw(c);
    if (!g_fb.base) {
        return;
    }
    uint32_t cw = 8U * g_scale;
    if ((unsigned char)c < 0x20U || (unsigned char)c > 0x7eU) {
        c = '?';
    }
    if (g_col_x + cw > g_fb.width) {
        fb_newline();
    }
    const unsigned char *glyph = font8x16[(unsigned char)c - 0x20U];
    for (uint32_t r = 0; r < 16U * g_scale; r++) {
        unsigned char bits = glyph[r / g_scale];
        for (uint32_t col = 0; col < cw; col++) {
            uint32_t color = (bits & (0x80U >> (col / g_scale))) ? g_fg : g_bg;
            fb_px(g_col_x + col, g_row_y + r, color);
        }
    }
    g_col_x += cw;
}

void con_puts(const char *s)
{
    while (*s) {
        con_putc(*s++);
    }
}

void con_hex(uint64_t v)
{
    static const char hex[] = "0123456789abcdef";
    int i = 60;
    con_puts("0x");
    while (i > 0 && !((v >> i) & 0xfU)) {
        i -= 4;
    }
    for (; i >= 0; i -= 4) {
        con_putc(hex[(v >> i) & 0xfU]);
    }
}

void con_dec(uint64_t v)
{
    char buf[21];
    int n = 0;
    do {
        buf[n++] = (char)('0' + (v % 10ULL));
        v /= 10ULL;
    } while (v);
    while (n > 0) {
        con_putc(buf[--n]);
    }
}
