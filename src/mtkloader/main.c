/*
 * begonia (Redmi Note 8 Pro / MT6785) bare-metal loader main entry.
 * Supports PC-style dual-boot menu (Android / iOS 18 XNU / Diagnostics),
 * live LK DTB & SoC hardware probing, SWEETXNU payload loading, and Android chainbooting.
 */
#include "begonia_hw.h"
#include "boot_menu.h"
#include "console.h"
#include "fdt.h"
#include "inflate.h"
#include "mmu.h"
#include "touch.h"
#include "spi5.h"
#include "xnu_boot.h"

extern char _start[];
extern char __loader_end[];

static const uint8_t *g_xnu_gz_in;
static uint64_t g_xnu_gz_len;
static uint64_t g_xnu_cpio_data;
static uint32_t g_xnu_cpio_size;

void loader_exception_handler(uint64_t slot, uint64_t esr, uint64_t elr, uint64_t far)
{
    console_set_fg(0xffff5555U);
    con_puts("\n[EXCEPTION] slot=");
    con_dec(slot);
    con_puts(" ESR=");
    con_hex(esr);
    con_puts(" ELR=");
    con_hex(elr);
    con_puts(" FAR=");
    con_hex(far);
    con_putc('\n');
    for (;;) {
        __asm__ volatile("wfe");
    }
}

static void print_cpu_probe(void)
{
    uint64_t el = 0, mpidr = 0, midr = 0, isar1 = 0, freq = 0;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    __asm__ volatile("mrs %0, mpidr_el1" : "=r"(mpidr));
    __asm__ volatile("mrs %0, midr_el1" : "=r"(midr));
    __asm__ volatile("mrs %0, id_aa64isar1_el1" : "=r"(isar1));
    freq = timer_freq();

    con_puts("EL=");
    con_dec((el >> 2) & 3ULL);
    con_puts(" MPIDR=");
    con_hex(mpidr);
    con_puts(" MIDR=");
    con_hex(midr);
    con_puts("\nISAR1=");
    con_hex(isar1);
    con_puts(" CNTFRQ=");
    con_dec(freq);
    con_puts(" Hz\n");
}

static void print_soc_probe(void)
{
    const struct fb_info *fb = console_fb();
    uint32_t gicd_typer = mmio_read32(MT6785_GICD_BASE + 0x004u);
    uint32_t ufs_ver = mmio_read32(MT6785_UFSHCI_BASE + UFSHCI_VER);
    uint32_t ovl_addr = mmio_read32(MT6785_DISP_OVL0_BASE + OVL_L0_ADDR);
    uint32_t ovl_pitch = mmio_read32(MT6785_DISP_OVL0_BASE + OVL_L0_PITCH) & 0xffffU;
    uint32_t kp1 = mmio_read32(KP_MEM1) & 0xffffU;
    uint16_t pmic_id = 0, topstatus = 0;
    int pmic_ok = pmic_read16(MT6359_HWCID, &pmic_id) && pmic_read16(MT6359_TOPSTATUS, &topstatus);

    con_puts("FB=");
    con_hex(fb->base);
    con_puts(" ");
    con_dec(fb->width);
    con_puts("x");
    con_dec(fb->height);
    con_puts(" pitch=");
    con_dec(fb->pitch);
    con_puts("\nOVL0 addr=");
    con_hex(ovl_addr);
    con_puts(" pitch=");
    con_dec(ovl_pitch);
    con_puts(" GICD_TYPER=");
    con_hex(gicd_typer);
    con_puts("\nUFSHCI_VER=");
    con_hex(ufs_ver);
    con_puts(" KP_MEM1=");
    con_hex(kp1);
    if (pmic_ok) {
        con_puts(" MT6359=");
        con_hex(pmic_id);
        con_puts(" TOP=");
        con_hex(topstatus);
    }
    con_putc('\n');
}

/*
 * ARM64 Image header custom slots:
 *   hdr[4] (0x20, res2): chained Android Image byte length (0 if standalone)
 *   hdr[5] (0x28, res3): offset of appended SWEETXNU payload from _start
 *   hdr[6] (0x30, res4): byte length of appended SWEETXNU payload
 */
/*
 * begonia's LK crashes (lk_crash, cold PMIC reset) when the boot image ramdisk is not a gzip
 * Android ramdisk, and when the compressed kernel section passes ~16 MB, so a raw SWEETXNU
 * payload can travel neither as the ramdisk nor next to the chained Android Image. It travels as
 * the first file ("xnu.sweet") of the gzip'd newc cpio ramdisk instead, prepended to the Android
 * ramdisk, and is inflated here below the chain stub, outside XNU's physical RAM window.
 */
#define GZ_PAYLOAD_PA   0x42400000ULL
#define GZ_PAYLOAD_MAX  (BEGONIA_CHAIN_STUB_PA - GZ_PAYLOAD_PA)
#define GZ_PEEK_BYTES   4096ULL
#define CPIO_HDR_LEN    110U
#define XNU_CPIO_NAME   "xnu.sweet"

static uint32_t cpio_hex8(const volatile uint8_t *p, int *ok)
{
    uint32_t v = 0;
    for (int i = 0; i < 8; i++) {
        uint8_t c = p[i];
        uint32_t d;
        if (c >= '0' && c <= '9') {
            d = (uint32_t)(c - '0');
        } else if (c >= 'a' && c <= 'f') {
            d = (uint32_t)(c - 'a' + 10);
        } else if (c >= 'A' && c <= 'F') {
            d = (uint32_t)(c - 'A' + 10);
        } else {
            *ok = 0;
            return 0;
        }
        v = (v << 4) | d;
    }
    return v;
}

/* Returns the data offset of a first newc entry named XNU_CPIO_NAME, or 0; size in *filesize. */
static uint64_t cpio_first_xnu_entry(const volatile uint8_t *c, uint64_t len, uint32_t *filesize)
{
    static const char magic[6] = { '0', '7', '0', '7', '0', '1' };
    static const char name[] = XNU_CPIO_NAME;
    if (len < CPIO_HDR_LEN + sizeof(name)) {
        return 0;
    }
    for (int i = 0; i < 6; i++) {
        if (c[i] != (uint8_t)magic[i]) {
            return 0;
        }
    }
    int ok = 1;
    uint32_t fsize = cpio_hex8(c + 54, &ok);
    uint32_t namesize = cpio_hex8(c + 94, &ok);
    if (!ok || namesize != sizeof(name)) {
        return 0;
    }
    for (uint32_t i = 0; i < namesize; i++) {
        if (c[CPIO_HDR_LEN + i] != (uint8_t)name[i]) {
            return 0;
        }
    }
    *filesize = fsize;
    return ((uint64_t)CPIO_HDR_LEN + namesize + 3ULL) & ~3ULL;
}

static int find_gz_cpio_payload(uint64_t rd_start, uint64_t rd_end,
                                uintptr_t *out_start, uintptr_t *out_end)
{
    const uint8_t *g = (const uint8_t *)(uintptr_t)rd_start;
    uint64_t len = rd_end - rd_start;
    if (len < 18ULL || g[0] != 0x1fU || g[1] != 0x8bU || g[2] != 8U) {
        return 0;
    }
    uint8_t flg = g[3];
    uint64_t off = 10ULL;
    if (flg & 4U) {
        off += 2ULL + (uint64_t)(g[off] | ((uint32_t)g[off + 1ULL] << 8));
    }
    if (flg & 8U) {
        while (off < len && g[off]) {
            off++;
        }
        off++;
    }
    if (flg & 16U) {
        while (off < len && g[off]) {
            off++;
        }
        off++;
    }
    if (flg & 2U) {
        off += 2ULL;
    }
    if (off + 8ULL >= len) {
        return 0;
    }
    volatile uint8_t *out = (volatile uint8_t *)(uintptr_t)GZ_PAYLOAD_PA;

    /* Peek: only the first entry, so a plain Android ramdisk costs ~nothing at boot. */
    uint64_t cap = GZ_PEEK_BYTES;
    (void)inflate_raw(g + off, len - 8ULL - off, out, &cap);
    uint32_t fsize = 0;
    uint64_t data = cpio_first_xnu_entry(out, cap, &fsize);
    if (data == 0ULL || (data & 7ULL) || (uint64_t)fsize < sizeof(struct sweet_payload) ||
        data + (uint64_t)fsize > GZ_PAYLOAD_MAX) {
        return 0;
    }

    uintptr_t s = (uintptr_t)GZ_PAYLOAD_PA + (uintptr_t)data;
    if (!xnu_payload_present(s, s + sizeof(struct sweet_payload))) {
        return 0;
    }
    g_xnu_gz_in = g + off;
    g_xnu_gz_len = len - 8ULL - off;
    g_xnu_cpio_data = data;
    g_xnu_cpio_size = fsize;
    *out_start = s;
    *out_end = s + fsize;
    return 1;
}

static int ensure_xnu_cpio_inflated(void)
{
    if (!g_xnu_gz_in) {
        return 0;
    }
    const struct fb_info *fb = console_fb();
    uint64_t gz_base = (uint64_t)(uintptr_t)g_xnu_gz_in;
    struct mmu_range ranges[6] = {
        { 0x0C000000ULL, 0x0A000000ULL, MMU_ATTR_DEV },
        { (uint64_t)(uintptr_t)_start, (uint64_t)(__loader_end - _start), MMU_ATTR_WB },
        { gz_base, g_xnu_gz_len + 8ULL, MMU_ATTR_WB },
        { GZ_PAYLOAD_PA, GZ_PAYLOAD_MAX, MMU_ATTR_WB },
        { PLOG_ZONE_PA, PLOG_ZONE_SIZE, MMU_ATTR_NC },
        { fb ? fb->base : 0ULL, fb ? (uint64_t)fb->size : 0ULL, MMU_ATTR_NC }
    };
    size_t nranges = (fb && fb->base && fb->size) ? 6U : 5U;
    int mmu_rc = mmu_enable_ranges(ranges, nranges);
    if (mmu_rc != 0) {
        con_puts("WARN: inflate MMU setup failed rc=");
        con_hex((uint64_t)(int64_t)mmu_rc);
        con_putc('\n');
    }

    uint64_t cap = GZ_PAYLOAD_MAX;
    volatile uint8_t *out = (volatile uint8_t *)(uintptr_t)GZ_PAYLOAD_PA;
    int rc = inflate_raw(g_xnu_gz_in, g_xnu_gz_len, out, &cap);
    if (mmu_rc == 0) {
        mmu_flush_dcache_range(GZ_PAYLOAD_PA, cap);
    }
    if (rc != 0 || g_xnu_cpio_data + (uint64_t)g_xnu_cpio_size > cap) {
        return -1;
    }
    g_xnu_gz_in = 0;
    return 0;
}

static int find_payload_window(uintptr_t fdt, uintptr_t *out_start, uintptr_t *out_end)
{
    const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
    uint64_t res3 = hdr[5];
    uint64_t res4 = hdr[6];
    if (res3 != 0ULL && res4 >= sizeof(struct sweet_payload) && (res3 & 7ULL) == 0ULL) {
        uintptr_t p = (uintptr_t)_start + (uintptr_t)res3;
        uintptr_t pend = p + (uintptr_t)res4;
        if (pend > p && xnu_payload_present(p, pend)) {
            *out_start = p;
            *out_end = pend;
            return 1;
        }
    }

    uint64_t rd_start = 0, rd_end = 0;
    if (fdt_chosen_u64(fdt, "linux,initrd-start", &rd_start) &&
        fdt_chosen_u64(fdt, "linux,initrd-end", &rd_end) &&
        rd_end > rd_start) {
        if (xnu_payload_present((uintptr_t)rd_start, (uintptr_t)rd_end)) {
            *out_start = (uintptr_t)rd_start;
            *out_end = (uintptr_t)rd_end;
            return 1;
        }
        if (find_gz_cpio_payload(rd_start, rd_end, out_start, out_end)) {
            return 1;
        }
    }

    return 0;
}

static void chain_android(uintptr_t fdt, uint64_t android_len)
{
    uint64_t el = 0;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));

    uintptr_t src = (uintptr_t)_start + BEGONIA_CHAIN_OFFSET;
    uint64_t tail_zero = BEGONIA_CHAIN_OFFSET;
    if (BEGONIA_KERNEL_PA + android_len + tail_zero > BEGONIA_CHAIN_STUB_PA) {
        tail_zero = (BEGONIA_CHAIN_STUB_PA > BEGONIA_KERNEL_PA + android_len)
            ? (BEGONIA_CHAIN_STUB_PA - (BEGONIA_KERNEL_PA + android_len))
            : 0ULL;
    }

    con_puts("Booting Android 17 (EL");
    con_dec((el >> 2) & 3ULL);
    con_puts(", fdt=");
    con_hex((uint64_t)fdt);
    con_puts(", len=");
    con_hex(android_len);
    con_puts(")...\n");

    const struct fb_info *fb = console_fb();
    struct mmu_range ranges[5] = {
        { 0x0C000000ULL, 0x0A000000ULL, MMU_ATTR_DEV },
        { BEGONIA_KERNEL_PA, (src + android_len + tail_zero) - BEGONIA_KERNEL_PA, MMU_ATTR_WB },
        { BEGONIA_CHAIN_STUB_PA, 0x4000ULL, MMU_ATTR_WB },
        { PLOG_ZONE_PA, PLOG_ZONE_SIZE, MMU_ATTR_NC },
        { fb ? fb->base : 0ULL, fb ? (uint64_t)fb->size : 0ULL, MMU_ATTR_NC }
    };
    size_t nranges = (fb && fb->base && fb->size) ? 5U : 4U;
    int mmu_rc = mmu_enable_ranges(ranges, nranges);
    if (mmu_rc != 0) {
        con_puts("WARN: MMU setup rc=");
        con_hex((uint64_t)(int64_t)mmu_rc);
        con_puts(", using uncached stub\n");
    }

    uintptr_t stub_len = (uintptr_t)(chain_stub_end - chain_stub);
    volatile uint8_t *dst = (volatile uint8_t *)(uintptr_t)BEGONIA_CHAIN_STUB_PA;
    for (uintptr_t i = 0; i < stub_len; i++) {
        dst[i] = chain_stub[i];
    }
    if (mmu_rc == 0) {
        mmu_flush_dcache_range(BEGONIA_CHAIN_STUB_PA, (uint64_t)stub_len);
    }
    __asm__ volatile("dsb sy; ic iallu; dsb sy; isb" ::: "memory");

    mmu_chain_fn_t fn = (mmu_chain_fn_t)BEGONIA_CHAIN_STUB_PA;
    fn((uint64_t)fdt, BEGONIA_KERNEL_PA, (uint64_t)src,
       android_len, BEGONIA_KERNEL_PA, tail_zero);
}

static void show_diagnostics(uintptr_t fdt, const struct touch_state *touch)
{
    console_clear(0xff0d1117U);
    console_set_fg(0xff58a6ffU);
    con_puts("== begonia-ios mtkloader (MT6785 / Redmi Note 8 Pro) ==\n");
    console_set_fg(0xffffffffU);
    con_puts("LK FDT=");
    con_hex((uint64_t)fdt);
    con_puts("\nTouch ready=");
    con_dec((uint64_t)touch->ready);
    con_puts(" PID=");
    con_hex((uint64_t)touch->pid);
    con_puts(" FW=v");
    con_dec((uint64_t)touch->fw_ver);
    con_puts(" DBG=");
    con_hex((uint64_t)touch->dbg);
    con_puts(" ID=");
    for (int i = 0; i < 6; i++) {
        con_hex(touch->chip_id[i]);
        if (i < 5) {
            con_putc(':');
        }
    }
    con_putc('\n');
    print_cpu_probe();
    print_soc_probe();
    fdt_dump_board_info(fdt);
}

void mtkloader_main(uint64_t fdt_pa)
{
    uintptr_t fdt = (uintptr_t)fdt_pa;
    uintptr_t pl_start = 0, pl_end = 0;
    const volatile uint64_t *hdr = (const volatile uint64_t *)_start;
    uint64_t android_len = hdr[4];
    struct touch_state touch;

    plog_init();
    con_puts("== begonia-ios mtkloader plog ==\nentry fdt=");
    con_hex((uint64_t)fdt);
    con_puts(" android_len=");
    con_hex(android_len);
    con_putc('\n');
    wdt_disable();
    con_puts("wdt off\n");
    console_init(fdt, 2U);
    {
        const struct fb_info *cfb = console_fb();
        con_puts("console fb=");
        con_hex(cfb ? cfb->base : 0ULL);
        con_puts(" pitch=");
        con_dec(cfb ? cfb->pitch : 0U);
        con_puts(" ovl_active=");
        con_dec(cfb ? cfb->ovl_active : 0U);
        con_putc('\n');
    }
    console_strip_ovl_alpha();
    con_puts("ovl alpha stripped\n");

    uintptr_t android_img_pa = android_len ? (uintptr_t)_start + BEGONIA_CHAIN_OFFSET : 0U;
    touch_init(android_img_pa, android_len, &touch);
    con_puts("touch ready=");
    con_dec((uint64_t)touch.ready);
    /* 22 Nov: why not (touch_nvt.c dbg: 2 no chip id, 3 no firmware in the Android Image, 6 download failed,
     * 7 uploaded, bit 0 already running); the iOS touch driver needs the firmware this loads */
    con_puts(" dbg=");
    con_hex((uint64_t)touch.dbg);
    con_puts(" fw_uploaded=");
    con_dec((uint64_t)touch.fw_uploaded);
    con_puts(" pid=");
    con_hex((uint64_t)touch.pid);
    con_puts(" fw_ver=");
    con_hex((uint64_t)touch.fw_ver);
    con_puts(" spi_fail=");
    con_dec((uint64_t)spi5_fail_count);
    con_puts(" last=");
    con_hex((uint64_t)spi5_last_fail);
    con_putc('\n');

    int has_xnu = find_payload_window(fdt, &pl_start, &pl_end);
    int has_android = (android_len != 0ULL);
    con_puts("payload has_xnu=");
    con_dec((uint64_t)has_xnu);
    con_puts(" start=");
    con_hex((uint64_t)pl_start);
    con_puts(" end=");
    con_hex((uint64_t)pl_end);
    con_puts("\nmenu\n");

    for (;;) {
        enum boot_target target = boot_menu_select(has_android, has_xnu, &touch);

        if (target == BOOT_TARGET_ANDROID && has_android) {
            console_clear(0xff000000U);
            chain_android(fdt, android_len);
        }

        if (target == BOOT_TARGET_XNU && has_xnu) {
            /* 25 Nov 2026: LK's display reset stopped the touch MCU; start its firmware for iOS (touch_nvt.c) */
            touch_boot_fw(android_img_pa, android_len, &touch);
            con_puts("xnu touch: ok=");
            con_dec((uint64_t)touch.boot_ok);
            con_puts(" tries=");
            con_dec((uint64_t)touch.boot_tries);
            con_puts(" state=");
            con_hex((uint64_t)touch.boot_state);
            con_puts(" dbg=");
            con_hex((uint64_t)touch.dbg);
            con_puts(" fw_ver=");
            con_hex((uint64_t)touch.fw_ver);
            con_puts(" spi_fail=");
            con_dec((uint64_t)spi5_fail_count);
            con_puts(" last=");
            con_hex((uint64_t)spi5_last_fail);
            con_putc('\n');
            show_diagnostics(fdt, &touch);
            console_set_fg(0xff3fb950U);
            con_puts("Booting SWEETXNU payload at ");
            con_hex(pl_start);
            con_putc('\n');
            console_set_fg(0xffffffffU);

            const char *err = (ensure_xnu_cpio_inflated() != 0)
                ? "cpio payload inflate failed"
                : xnu_payload_boot(pl_start, pl_end);
            console_set_fg(0xffff5555U);
            con_puts("XNU boot error: ");
            con_puts(err);
            con_putc('\n');
            console_set_fg(0xffffffffU);
            con_puts("Halted cleanly (hold Power to reboot).\n");
            for (;;) {
                __asm__ volatile("wfe");
            }
        }

        show_diagnostics(fdt, &touch);
        console_set_fg(0xff3fb950U);
        con_puts("\nTap screen or press any key to return to Startup Manager...\n");
        console_set_fg(0xffffffffU);
        boot_menu_wait_any_input(&touch);
    }
}
