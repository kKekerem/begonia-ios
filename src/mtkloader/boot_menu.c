/*
 * Dual-boot selection menu for Redmi Note 8 Pro (begonia / MT6785).
 * Reads MT6785 KP_MEM1 (0x10010000) and MT6359 TOPSTATUS (0x2a via PWRAP WACS2).
 */
#include "boot_menu.h"
#include "begonia_hw.h"
#include "console.h"

static int pwrap_wait_fsm(uint32_t want)
{
    for (uint32_t spin = 0; spin < 50000U; spin++) {
        if (PWRAP_FSM(mmio_read32(PWRAP_WACS2_RDATA)) == want) {
            return 1;
        }
    }
    return 0;
}

int pmic_read16(uint16_t reg, uint16_t *out)
{
    if ((mmio_read32(PWRAP_WACS2_EN) & 1U) == 0U) {
        return 0;
    }
    if (!pwrap_wait_fsm(PWRAP_FSM_IDLE)) {
        return 0;
    }
    mmio_write32(PWRAP_WACS2_CMD, ((uint32_t)(reg >> 1)) << 16);
    if (!pwrap_wait_fsm(PWRAP_FSM_WFVLDCLR)) {
        return 0;
    }
    *out = (uint16_t)(mmio_read32(PWRAP_WACS2_RDATA) & 0xffffU);
    mmio_write32(PWRAP_WACS2_VLDCLR, 1U);
    return 1;
}

#define BTN_UP      (1U << 0)
#define BTN_DOWN    (1U << 1)
#define BTN_POWER   (1U << 2)

static uint32_t sample_buttons(void)
{
    uint32_t btns = 0;
    uint32_t kp1 = mmio_read32(KP_MEM1) & 0xffffU;
    if ((kp1 & (1U << 0)) == 0U) {
        btns |= BTN_UP;
    }
    if ((kp1 & (1U << 1)) == 0U) {
        btns |= BTN_DOWN;
    }
    uint16_t top = 0xffffU;
    if (pmic_read16(MT6359_TOPSTATUS, &top)) {
        if ((top & MT6359_PWRKEY_DEB_BIT) == 0U) {
            btns |= BTN_POWER;
        }
        if ((top & MT6359_HOMEKEY_DEB_BIT) == 0U) {
            btns |= BTN_DOWN;
        }
    }
    return btns;
}

static void draw_menu(int has_android, int has_xnu, int selected, int secs_left)
{
    console_clear(0xff0d1117U);
    console_set_fg(0xff58a6ffU);
    con_puts("\n  begonia-ios Dual Boot Manager (MT6785)\n");
    console_set_fg(0xff8b949eU);
    con_puts("  --------------------------------------\n\n");

    const char *labels[3] = {
        "Android (Stock / Custom ROM)",
        "iOS 18 (XNU-11215)",
        "Diagnostic / Hardware Probe",
    };
    const int available[3] = { has_android, has_xnu, 1 };

    for (int i = 0; i < 3; i++) {
        if (i == selected) {
            console_set_fg(0xff3fb950U);
            con_puts("  > ");
        } else {
            console_set_fg(available[i] ? 0xffffffffU : 0xff6e7681U);
            con_puts("    ");
        }
        con_puts("[");
        con_dec((uint64_t)(i + 1));
        con_puts("] ");
        con_puts(labels[i]);
        if (!available[i]) {
            con_puts(" (not attached)");
        }
        con_putc('\n');
    }

    console_set_fg(0xff8b949eU);
    con_puts("\n  Vol+/Vol-: Select   Power: Boot\n");
    if (secs_left >= 0) {
        console_set_fg(0xffd29922U);
        con_puts("  Auto-booting in ");
        con_dec((uint64_t)secs_left);
        con_puts("s...\n");
    }
    console_set_fg(0xffffffffU);
}

static int next_available(int cur, int dir, const int avail[3])
{
    for (int step = 0; step < 3; step++) {
        cur = (cur + dir + 3) % 3;
        if (avail[cur]) {
            return cur;
        }
    }
    return BOOT_TARGET_DIAG;
}

enum boot_target boot_menu_select(int has_android, int has_xnu)
{
    const int avail[3] = { has_android, has_xnu, 1 };
    int selected = has_android ? BOOT_TARGET_ANDROID : (has_xnu ? BOOT_TARGET_XNU : BOOT_TARGET_DIAG);

    /* Fast path: if only diagnostic mode is available, skip wait and run immediately */
    if (!has_android && !has_xnu) {
        return BOOT_TARGET_DIAG;
    }

    int countdown = 3;
    int ticks_per_sec = 50; /* 20 ms poll cadence */
    int subtick = 0;
    uint32_t prev = sample_buttons();
    uint32_t stable = prev;

    draw_menu(has_android, has_xnu, selected, countdown);

    for (;;) {
        delay_ms(20);
        uint32_t s1 = sample_buttons();
        if (s1 == prev) {
            uint32_t pressed = s1 & ~stable;
            stable = s1;
            if (pressed & BTN_POWER) {
                return (enum boot_target)selected;
            }
            if (pressed & BTN_UP) {
                countdown = -1;
                selected = next_available(selected, -1, avail);
                draw_menu(has_android, has_xnu, selected, countdown);
            } else if (pressed & BTN_DOWN) {
                countdown = -1;
                selected = next_available(selected, 1, avail);
                draw_menu(has_android, has_xnu, selected, countdown);
            }
        }
        prev = s1;

        if (countdown >= 0 && ++subtick >= ticks_per_sec) {
            subtick = 0;
            if (--countdown <= 0) {
                return (enum boot_target)selected;
            }
            draw_menu(has_android, has_xnu, selected, countdown);
        }
    }
}
