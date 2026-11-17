/*
 * begonia (Redmi Note 8 Pro / MT6785) bare-metal loader main entry.
 * Supports PC-style dual-boot menu (Android / iOS 18 XNU / Diagnostics),
 * live LK DTB & SoC hardware probing, SWEETXNU payload loading, and Android chainbooting.
 */
#include "begonia_hw.h"
#include "boot_menu.h"
#include "console.h"
#include "fdt.h"
#include "touch.h"
#include "xnu_boot.h"

extern char _start[];
extern char chain_stub[];
extern char chain_stub_end[];

typedef void (*chain_fn_t)(uint64_t dtb, uint64_t dst, uint64_t src, uint64_t len, uint64_t entry);

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
        rd_end > rd_start &&
        xnu_payload_present((uintptr_t)rd_start, (uintptr_t)rd_end)) {
        *out_start = (uintptr_t)rd_start;
        *out_end = (uintptr_t)rd_end;
        return 1;
    }

    return 0;
}

static void chain_android(uintptr_t fdt, uint64_t android_len)
{
    con_puts("Chainbooting Android Image (len=");
    con_hex(android_len);
    con_puts(")...\n");

    uintptr_t stub_len = (uintptr_t)(chain_stub_end - chain_stub);
    volatile uint8_t *dst = (volatile uint8_t *)BEGONIA_CHAIN_STUB_PA;
    for (uintptr_t i = 0; i < stub_len; i++) {
        dst[i] = (uint8_t)chain_stub[i];
    }
    __asm__ volatile("dsb sy; isb");

    chain_fn_t fn = (chain_fn_t)BEGONIA_CHAIN_STUB_PA;
    fn((uint64_t)fdt, BEGONIA_KERNEL_PA, (uint64_t)((uintptr_t)_start + BEGONIA_CHAIN_OFFSET),
       android_len, BEGONIA_KERNEL_PA);
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

    wdt_disable();
    console_init(fdt, 2U);
    console_strip_ovl_alpha();

    uintptr_t android_img_pa = android_len ? (uintptr_t)_start + BEGONIA_CHAIN_OFFSET : 0U;
    touch_init(android_img_pa, android_len, &touch);

    int has_xnu = find_payload_window(fdt, &pl_start, &pl_end);
    int has_android = (android_len != 0ULL);

    for (;;) {
        enum boot_target target = boot_menu_select(has_android, has_xnu, &touch);

        if (target == BOOT_TARGET_ANDROID && has_android) {
            console_clear(0xff000000U);
            chain_android(fdt, android_len);
        }

        if (target == BOOT_TARGET_XNU && has_xnu) {
            show_diagnostics(fdt, &touch);
            console_set_fg(0xff3fb950U);
            con_puts("Booting SWEETXNU payload at ");
            con_hex(pl_start);
            con_putc('\n');
            console_set_fg(0xffffffffU);

            const char *err = xnu_payload_boot(pl_start, pl_end);
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
