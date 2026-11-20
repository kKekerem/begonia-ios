/*
 * Early output for begonia: APUART0 + MediaTek LK continuous-splash framebuffer.
 * Built without SIMD and with strict alignment (-mgeneral-regs-only -mstrict-align).
 */
#include "console.h"
#include "begonia_hw.h"
#include "fdt.h"
#include "font8x16.h"

static const uint8_t k_fmts[8][6] = {
    { 4U, 16U,  8U,  0U, 24U, 0U }, /* 0: BGRA8888 */
    { 4U,  0U,  8U, 16U, 24U, 0U }, /* 1: RGBA8888 */
    { 4U, 24U, 16U,  8U,  0U, 0U }, /* 2: ABGR8888 */
    { 4U,  8U, 16U, 24U,  0U, 0U }, /* 3: ARGB8888 */
    { 3U, 16U,  8U,  0U,  0U, 0U }, /* 4: BGR888 */
    { 3U,  0U,  8U, 16U,  0U, 0U }, /* 5: RGB888 */
    { 2U,  0U,  0U,  0U,  0U, 0U }, /* 6: RGB565 */
    { 2U,  0U,  0U,  0U,  0U, 1U }, /* 7: BGR565 */
};

static struct fb_info g_fb;
static uint64_t g_ovl_base = MT6785_DISP_OVL0_BASE;
static uint32_t g_ovl_layer;
static uint32_t g_fmt_idx;
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

static uint32_t fb_pack_px(uint32_t argb)
{
    const uint8_t *f = k_fmts[g_fmt_idx];
    uint32_t a = (argb >> 24) & 0xffU;
    uint32_t r = (argb >> 16) & 0xffU;
    uint32_t g = (argb >> 8) & 0xffU;
    uint32_t b = argb & 0xffU;
    if (f[0] == 2U) {
        return f[5]
            ? (((b >> 3) << 11) | ((g >> 2) << 5) | (r >> 3))
            : (((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }
    uint32_t packed = (r << f[1]) | (g << f[2]) | (b << f[3]);
    return (f[0] == 4U) ? (packed | (a << f[4])) : packed;
}

static void fb_store_span(uintptr_t addr, uint32_t count, uint32_t packed)
{
    uint32_t bpp = k_fmts[g_fmt_idx][0];
    if (bpp == 4U && (addr & 3U) == 0U) {
        volatile uint32_t *row = (volatile uint32_t *)addr;
        for (uint32_t i = 0; i < count; i++) {
            row[i] = packed;
        }
        return;
    }
    if (bpp == 2U && (addr & 1U) == 0U) {
        volatile uint16_t *row = (volatile uint16_t *)addr;
        uint16_t c16 = (uint16_t)packed;
        for (uint32_t i = 0; i < count; i++) {
            row[i] = c16;
        }
        return;
    }
    volatile uint8_t *p = (volatile uint8_t *)addr;
    for (uint32_t i = 0; i < count; i++) {
        for (uint32_t b = 0; b < bpp; b++) {
            p[b] = (uint8_t)(packed >> (b * 8U));
        }
        p += bpp;
    }
}

static void fb_px(uint32_t x, uint32_t y, uint32_t argb)
{
    uintptr_t addr = g_fb.base + (uint64_t)y * g_fb.pitch + (uint64_t)x * k_fmts[g_fmt_idx][0];
    fb_store_span(addr, 1U, fb_pack_px(argb));
}

static void fb_hline(int32_t x0, int32_t x1, int32_t y, uint32_t argb)
{
    if (!g_fb.base || y < 0 || (uint32_t)y >= g_fb.height || x1 <= x0) {
        return;
    }
    if (x0 < 0) {
        x0 = 0;
    }
    if ((uint32_t)x1 > g_fb.width) {
        x1 = (int32_t)g_fb.width;
    }
    uintptr_t addr = g_fb.base + (uint64_t)(uint32_t)y * g_fb.pitch + (uint64_t)(uint32_t)x0 * k_fmts[g_fmt_idx][0];
    fb_store_span(addr, (uint32_t)(x1 - x0), fb_pack_px(argb));
}

void fb_fill_rect(uint32_t x0, uint32_t y0, uint32_t w, uint32_t h, uint32_t c)
{
    if (!g_fb.base || w == 0 || h == 0) {
        return;
    }
    int32_t x1 = (int32_t)(x0 + w);
    uint32_t y_end = y0 + h;
    for (uint32_t y = y0; y < y_end && y < g_fb.height; y++) {
        fb_hline((int32_t)x0, x1, (int32_t)y, c);
    }
}

static uint32_t isqrt_u32(uint32_t v)
{
    uint32_t r = 0;
    uint32_t bit = 1U << 30;
    while (bit > v) {
        bit >>= 2;
    }
    while (bit != 0U) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

static uint32_t clamp_radius(uint32_t w, uint32_t h, uint32_t r)
{
    if (r * 2U > w) {
        r = w / 2U;
    }
    if (r * 2U > h) {
        r = h / 2U;
    }
    return r;
}

static uint32_t round_inset(uint32_t row, uint32_t h, uint32_t r)
{
    if (r == 0U) {
        return 0U;
    }
    uint32_t dy;
    if (row < r) {
        dy = r - 1U - row;
    } else if (row >= h - r) {
        dy = row - (h - r);
    } else {
        return 0U;
    }
    uint32_t rr = r * r;
    uint32_t dx = isqrt_u32(rr > dy * dy ? rr - dy * dy : 0U);
    return (r > dx) ? (r - dx) : 0U;
}

void fb_fill_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t argb)
{
    if (!g_fb.base || w == 0 || h == 0) {
        return;
    }
    r = clamp_radius(w, h, r);
    for (uint32_t row = 0; row < h; row++) {
        uint32_t inset = round_inset(row, h, r);
        fb_hline((int32_t)(x + inset), (int32_t)(x + w - inset), (int32_t)(y + row), argb);
    }
}

void fb_stroke_round_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t r, uint32_t thick, uint32_t argb)
{
    if (!g_fb.base || w == 0 || h == 0 || thick == 0) {
        return;
    }
    r = clamp_radius(w, h, r);
    uint32_t r_in = (r > thick) ? (r - thick) : 0U;
    for (uint32_t row = 0; row < h; row++) {
        uint32_t out_ins = round_inset(row, h, r);
        if (row < thick || row >= h - thick) {
            fb_hline((int32_t)(x + out_ins), (int32_t)(x + w - out_ins), (int32_t)(y + row), argb);
        } else {
            uint32_t in_ins = thick + round_inset(row - thick, h - 2U * thick, r_in);
            fb_hline((int32_t)(x + out_ins), (int32_t)(x + in_ins), (int32_t)(y + row), argb);
            fb_hline((int32_t)(x + w - in_ins), (int32_t)(x + w - out_ins), (int32_t)(y + row), argb);
        }
    }
}

void fb_fill_circle(int32_t cx, int32_t cy, int32_t r, uint32_t argb)
{
    if (!g_fb.base || r <= 0) {
        return;
    }
    int32_t rr = r * r;
    for (int32_t dy = -r; dy <= r; dy++) {
        int32_t dx = (int32_t)isqrt_u32((uint32_t)(rr - dy * dy));
        fb_hline(cx - dx, cx + dx + 1, cy + dy, argb);
    }
}

uint32_t fb_text_width(const char *s, uint32_t scale)
{
    uint32_t n = 0;
    while (*s++) {
        n++;
    }
    return n * 8U * (scale ? scale : 1U);
}

void fb_draw_text(uint32_t x, uint32_t y, const char *s, uint32_t scale, uint32_t fg, uint32_t bg, int transparent_bg)
{
    if (!g_fb.base) {
        return;
    }
    uint32_t sc = scale ? scale : 1U;
    uint32_t cw = 8U * sc;
    uint32_t ch = 16U * sc;
    uint32_t cur_x = x;
    while (*s) {
        unsigned char uc = (unsigned char)*s++;
        if (uc < 0x20U || uc > 0x7eU) {
            uc = '?';
        }
        const unsigned char *glyph = font8x16[uc - 0x20U];
        for (uint32_t r = 0; r < ch && (y + r) < g_fb.height; r++) {
            unsigned char bits = glyph[r / sc];
            for (uint32_t col = 0; col < cw && (cur_x + col) < g_fb.width; col++) {
                if (bits & (0x80U >> (col / sc))) {
                    fb_px(cur_x + col, y + r, fg);
                } else if (!transparent_bg) {
                    fb_px(cur_x + col, y + r, bg);
                }
            }
        }
        cur_x += cw;
    }
}

static uint64_t le_read(uintptr_t addr, uint32_t nbytes)
{
    const volatile uint8_t *p = (const volatile uint8_t *)addr;
    uint64_t v = 0;
    for (uint32_t i = 0; i < nbytes; i++) {
        v |= (uint64_t)p[i] << (i * 8U);
    }
    return v;
}

static void ovl_decode_fmt(uint32_t con, uint32_t pitch)
{
    uint32_t cfmt = (con >> 12) & 0xfU;
    uint32_t btsw = (con >> 24) & 1U;
    uint32_t rgbsw = (con >> 25) & 1U;
    uint32_t man = (con >> 23) & 1U;

    if (cfmt == 0U && pitch >= g_fb.width * 2U) {
        g_fmt_idx = 6U + btsw;
    } else if (cfmt == 1U && pitch >= g_fb.width * 3U) {
        g_fmt_idx = 4U + btsw;
    } else if (cfmt == 2U && pitch >= g_fb.width * 4U) {
        g_fmt_idx = btsw;
    } else if (cfmt == 3U && pitch >= g_fb.width * 4U) {
        g_fmt_idx = man ? (btsw ? (3U - rgbsw) : rgbsw) : (2U + btsw);
    } else if (pitch < g_fb.width * 3U) {
        g_fmt_idx = 6U;
    } else if (pitch < g_fb.width * 4U) {
        g_fmt_idx = 4U;
    } else {
        g_fmt_idx = 0U;
    }
}

/*
 * Probe LK framebuffer: /chosen atag,videolfb first, then atag,videolfb-fb_base_l,
 * then unconditionally adopt any active OVL0 / OVL0_2L layer base, pitch & format.
 */
void console_init(uintptr_t fdt, uint32_t scale)
{
    g_scale = scale ? scale : 1U;
    g_fb.width = BEGONIA_PANEL_W;
    g_fb.height = BEGONIA_PANEL_H;
    g_fb.pitch = BEGONIA_FB_STRIDE_DEF;
    g_fb.base = BEGONIA_FB_FALLBACK;
    g_fb.size = (uint32_t)BEGONIA_FB_FALLBACK_SZ;
    g_fb.ovl_active = 0;
    g_ovl_base = MT6785_DISP_OVL0_BASE;
    g_ovl_layer = 0U;
    g_fmt_idx = 0U;

    uint32_t vlen = 0;
    uintptr_t vprop = fdt_chosen_prop(fdt, "atag,videolfb", &vlen);
    uint64_t fb_lo = 0, fb_hi = 0, vram_sz = 0;
    if (vprop && vlen >= 20U) {
        uint64_t base = le_read(vprop, 8U);
        uint32_t vram = (uint32_t)le_read(vprop + 16U, 4U);
        if (base >= 0x40000000ULL) {
            g_fb.base = base;
            if (vram >= g_fb.width * g_fb.height * 2U) {
                g_fb.size = vram;
            }
        }
    } else if (fdt_chosen_u64(fdt, "atag,videolfb-fb_base_l", &fb_lo) && fb_lo != 0ULL) {
        (void)fdt_chosen_u64(fdt, "atag,videolfb-fb_base_h", &fb_hi);
        g_fb.base = (fb_hi << 32) | (fb_lo & 0xffffffffULL);
        if (fdt_chosen_u64(fdt, "atag,videolfb-vramSize", &vram_sz) &&
            vram_sz >= (uint64_t)g_fb.pitch * g_fb.height) {
            g_fb.size = (uint32_t)vram_sz;
        }
    }

    const uint64_t ovl_bases[2] = { MT6785_DISP_OVL0_BASE, MT6785_DISP_OVL0_2L };
    uint64_t best_area = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t ovl_src = mmio_read32(ovl_bases[i] + OVL_SRC_CON);
        for (uint32_t l = 0; l < 4U; l++) {
            if ((ovl_src & (1U << l)) == 0U) {
                continue;
            }
            uint32_t off = l * OVL_LAYER_STRIDE;
            uint32_t con = mmio_read32(ovl_bases[i] + OVL_L0_CON + off);
            uint32_t addr = mmio_read32(ovl_bases[i] + OVL_L0_ADDR + off);
            uint32_t pitch = mmio_read32(ovl_bases[i] + OVL_L0_PITCH + off) & 0xffffU;
            uint32_t sz = mmio_read32(ovl_bases[i] + OVL_L0_SRC_SIZE + off);
            if ((con & (3U << 28)) != 0U || addr < 0x40000000U ||
                pitch < g_fb.width * 2U || pitch > 8192U) {
                continue;
            }
            uint64_t area = (uint64_t)((sz & 0x1fffU) ? (sz & 0x1fffU) : g_fb.width) *
                            (((sz >> 16) & 0x1fffU) ? ((sz >> 16) & 0x1fffU) : g_fb.height);
            if (area > best_area) {
                best_area = area;
                g_ovl_base = ovl_bases[i];
                g_ovl_layer = l;
                g_fb.ovl_active = 1U;
                g_fb.base = (uint64_t)addr;
                g_fb.pitch = pitch;
                ovl_decode_fmt(con, pitch);
            }
        }
    }

    /* Clamp mapped FB size to active scanout so LK sub-buffers never cross into tee-secmem. */
    uint64_t active_sz = (uint64_t)g_fb.pitch * g_fb.height;
    if (active_sz >= (uint64_t)g_fb.width * g_fb.height * 2ULL &&
        active_sz < (uint64_t)g_fb.size) {
        g_fb.size = (uint32_t)active_sz;
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
    uint32_t off = g_ovl_layer * OVL_LAYER_STRIDE;
    uint32_t con = mmio_read32(g_ovl_base + OVL_L0_CON + off);
    uint32_t src = mmio_read32(g_ovl_base + OVL_SRC_CON);
    uint32_t keep = 1U << g_ovl_layer;
    if ((con & (1U << 8)) != 0U) {
        mmio_write32(g_ovl_base + OVL_L0_CON + off, con & ~(1U << 8));
    }
    if ((src & ~keep) != 0U) {
        mmio_write32(g_ovl_base + OVL_SRC_CON, src & keep);
    }
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
    __asm__ volatile("dsb sy" ::: "memory");
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

/* persistent_ram_buffer as Linux 4.14 ramoops reads it: sig, start, size, data[]. */
#define PLOG_SIG        0x43474244U
#define PLOG_DATA_CAP   ((uint32_t)(PLOG_ZONE_SIZE - 12U))
static int g_plog_on;
static uint32_t g_plog_pos;
static uint32_t g_plog_size;

/*
 * A valid zone is kept and appended to (19 Nov): after an iOS boot it holds the XNU console that XNU mirrors
 * there (begonia_plog.c), and the next Android boot must still find it in pstore. Android's ramoops zaps the
 * header (start = size = 0) when it boots, so the zone only grows between two Android boots.
 */
void plog_init(void)
{
    volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)PLOG_ZONE_PA;
    if (h[0] == PLOG_SIG && h[1] < PLOG_DATA_CAP && h[2] <= PLOG_DATA_CAP) {
        g_plog_pos = h[1];
        g_plog_size = h[2];
    } else {
        h[0] = PLOG_SIG;
        h[1] = 0U;
        h[2] = 0U;
        g_plog_pos = 0U;
        g_plog_size = 0U;
    }
    g_plog_on = 1;
}

/* 1 if the kept log (since the last Android boot) contains needle. */
int plog_contains(const char *needle)
{
    const volatile uint8_t *d = (const volatile uint8_t *)(uintptr_t)(PLOG_ZONE_PA + 12U);
    uint32_t n = 0U;
    while (needle[n]) {
        n++;
    }
    if (n == 0U || g_plog_size < n) {
        return 0;
    }
    uint32_t start = (g_plog_pos + PLOG_DATA_CAP - g_plog_size) % PLOG_DATA_CAP;
    for (uint32_t i = 0U; i + n <= g_plog_size; i++) {
        uint32_t k = 0U;
        while (k < n && d[(start + i + k) % PLOG_DATA_CAP] == (uint8_t)needle[k]) {
            k++;
        }
        if (k == n) {
            return 1;
        }
    }
    return 0;
}

/*
 * The loader runs with the MMU and D-cache off until it boots XNU, but LK left its cache lines
 * behind (the Linux boot protocol only asks for a clean, not an invalidate). A stale line over a
 * BSS buffer written now, with the cache off, would hide the new bytes once the MMU and the cache
 * are on (20 Nov: the request's boot-args never reached XNU). Drop those lines first.
 */
static void dcache_invalidate_range(volatile void *p, uint32_t len)
{
    uintptr_t a = (uintptr_t)p & ~(uintptr_t)63U;
    uintptr_t end = (uintptr_t)p + len;
    for (; a < end; a += 64U) {
        __asm__ volatile("dc ivac, %0" :: "r"(a) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static int boot_arg_char(uint8_t c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           c == '=' || c == '_' || c == '-' || c == '.' || c == ',' || c == ' ';
}

/*
 * One-shot boot request from Android (20 Nov 2026). Android cannot reach this loader, but its kernel
 * console lands in the ramoops console zone (cprz), which survives the watchdog reset of a reboot and
 * is not touched by preloader/LK. `echo 'LOXY_BOOT_IOS_ONCE{begoniareboot=420}' > /dev/kmsg; reboot`
 * therefore asks for one iOS boot, optionally with extra XNU boot-args (boot-arg characters only,
 * {...} optional). Only what that Android boot wrote is searched (header start/size, Linux zaps the zone
 * when it boots), and every match is consumed here (its first byte overwritten), so the boot after iOS,
 * and any Android crash/reboot after it, gets the normal menu again (Android after 10 s): a request can
 * never turn into a boot loop. Returns 1 if the zone held the request; args gets the last {...}.
 */
int pstore_request_take(const char *needle, char *args, uint32_t args_cap)
{
    volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)PSTORE_CONSOLE_PA;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)(PSTORE_CONSOLE_PA + 12U);
    const uint32_t cap = (uint32_t)(PSTORE_CONSOLE_SIZE - 12U);
    uint32_t n = 0U;
    int found = 0;

    if (args_cap) {
        dcache_invalidate_range(args, args_cap);
        args[0] = '\0';
    }
    while (needle[n]) {
        n++;
    }
    if (n == 0U || h[0] != PLOG_SIG || h[1] >= cap || h[2] > cap || h[2] < n) {
        return 0;
    }
    uint32_t size = h[2];
    uint32_t start = (h[1] + cap - size) % cap;
    for (uint32_t i = 0U; i + n <= size; i++) {
        uint32_t k = 0U;
        while (k < n && d[(start + i + k) % cap] == (uint8_t)needle[k]) {
            k++;
        }
        if (k != n) {
            continue;
        }
        found = 1;
        uint32_t j = i + n;
        if (args_cap && j < size && d[(start + j) % cap] == '{') {
            uint32_t m = 0U;
            j++;
            while (j < size && m + 1U < args_cap && boot_arg_char(d[(start + j) % cap])) {
                args[m++] = (char)d[(start + j) % cap];
                j++;
            }
            if (j >= size || d[(start + j) % cap] != '}') {
                m = 0U;     /* unterminated or a character that is not allowed: no extra args */
            }
            args[m] = '\0';
        }
        d[(start + i) % cap] = (uint8_t)'#';
    }
    return found;
}

void plog_putc(char c)
{
    if (!g_plog_on) {
        return;
    }
    volatile uint32_t *h = (volatile uint32_t *)(uintptr_t)PLOG_ZONE_PA;
    volatile uint8_t *d = (volatile uint8_t *)(uintptr_t)(PLOG_ZONE_PA + 12U);
    d[g_plog_pos] = (uint8_t)c;
    g_plog_pos = (g_plog_pos + 1U) % PLOG_DATA_CAP;
    if (g_plog_size < PLOG_DATA_CAP) {
        g_plog_size++;
    }
    h[1] = g_plog_pos;
    h[2] = g_plog_size;
}

void con_putc(char c)
{
    plog_putc(c);
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
    if (g_col_x + cw > g_fb.width) {
        fb_newline();
    }
    char str[2] = { c, '\0' };
    fb_draw_text(g_col_x, g_row_y, str, g_scale, g_fg, g_bg, 0);
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
