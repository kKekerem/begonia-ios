/*
 * macOS Startup Manager-style Dual-Boot Screen for Redmi Note 8 Pro (begonia / MT6785).
 * Supports capacitive touch (Novatek NT36672A via SPI5) and physical Vol+/Vol-/Power buttons.
 */
#include "boot_menu.h"
#include "begonia_hw.h"
#include "console.h"

#define COL_BG          0xff111318U
#define COL_CARD        0xff1b2029U
#define COL_CARD_SEL    0xff232b3aU
#define COL_BORDER      0xff2f3644U
#define COL_SEL_RING    0xff3b82f6U
#define COL_WHITE       0xffffffffU
#define COL_MUTED       0xff9ca3afU
#define COL_ANDROID     0xff34d399U
#define COL_ANDROID_DK  0xff064e3bU
#define COL_IOS_SILVER  0xffd1d5dbU
#define COL_IOS_DK      0xff1e293bU
#define COL_WARN        0xfffbbf24U
#define COL_ERR         0xffef4444U
/* Auto-boot: with no button press or tap, Android starts after this many seconds, so an
 * unattended reboot (OTA, crash, adb reboot) never stops at the picker. Any input cancels it. */
#define AUTO_BOOT_SECONDS 10U
#define COUNTDOWN_Y     1520U

#define BTN_UP          (1U << 0)
#define BTN_DOWN        (1U << 1)
#define BTN_POWER       (1U << 2)

/* UI hitboxes in 1080x2340 panel coordinates */
#define CARD_W          450U
#define CARD_H          620U
#define CARD_Y          400U
#define CARD_AND_X      60U
#define CARD_IOS_X      570U
#define PATH_BOX_Y      1090U
#define PATH_BOX_H      160U
#define CONT_X          290U
#define CONT_Y          1330U
#define CONT_W          500U
#define CONT_H          116U
#define DIAG_X          310U
#define DIAG_Y          1920U
#define DIAG_W          460U
#define DIAG_H          92U
#define MODAL_X         80U
#define MODAL_Y         720U
#define MODAL_W         920U
#define MODAL_H         650U
#define MODAL_OK_X      340U
#define MODAL_OK_Y      1210U
#define MODAL_OK_W      400U
#define MODAL_OK_H      96U

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

static void draw_centered_text(uint32_t cx, uint32_t y, const char *s, uint32_t scale, uint32_t fg)
{
    uint32_t w = fb_text_width(s, scale);
    uint32_t x = (cx > w / 2U) ? (cx - w / 2U) : 0U;
    fb_draw_text(x, y, s, scale, fg, COL_BG, 1);
}

static void fmt_u32_append(char *dst, uint32_t *pos, uint32_t val, int hex)
{
    static const char hd[] = "0123456789abcdef";
    char tmp[12];
    int n = 0;
    if (hex) {
        do {
            tmp[n++] = hd[val & 0xfU];
            val >>= 4;
        } while (val);
    } else {
        do {
            tmp[n++] = (char)('0' + (val % 10U));
            val /= 10U;
        } while (val);
    }
    while (n > 0) {
        dst[(*pos)++] = tmp[--n];
    }
}

static void str_append(char *dst, uint32_t *pos, const char *src)
{
    while (*src) {
        dst[(*pos)++] = *src++;
    }
}

static void draw_android_emblem(int32_t cx, int32_t cy)
{
    /* Rounded drive disk tile + clean procedural droid emblem */
    fb_fill_round_rect((uint32_t)(cx - 95), (uint32_t)(cy - 95), 190U, 190U, 42U, COL_ANDROID_DK);
    fb_stroke_round_rect((uint32_t)(cx - 95), (uint32_t)(cy - 95), 190U, 190U, 42U, 4U, COL_ANDROID);
    /* Head dome (top half circle) + flat lower rim + eyes + antennas */
    fb_fill_circle(cx, cy + 12, 48, COL_ANDROID);
    fb_fill_rect((uint32_t)(cx - 52), (uint32_t)(cy + 16), 104U, 48U, COL_ANDROID_DK);
    fb_fill_rect((uint32_t)(cx - 48), (uint32_t)(cy + 20), 96U, 30U, COL_ANDROID);
    /* Left/right antennas (small angled rects) */
    fb_fill_rect((uint32_t)(cx - 34), (uint32_t)(cy - 52), 8U, 26U, COL_ANDROID);
    fb_fill_rect((uint32_t)(cx + 26), (uint32_t)(cy - 52), 8U, 26U, COL_ANDROID);
    /* Eyes */
    fb_fill_circle(cx - 18, cy - 6, 6, COL_ANDROID_DK);
    fb_fill_circle(cx + 18, cy - 6, 6, COL_ANDROID_DK);
}

static void draw_ios_emblem(int32_t cx, int32_t cy)
{
    /* Sleek macOS-style silver/titanium volume badge for iOS 18 (XNU) */
    fb_fill_round_rect((uint32_t)(cx - 95), (uint32_t)(cy - 95), 190U, 190U, 42U, COL_IOS_DK);
    fb_stroke_round_rect((uint32_t)(cx - 95), (uint32_t)(cy - 95), 190U, 190U, 42U, 4U, 0xff64748bU);
    /* Minimalist Apple/XNU orb + top leaf accent + crisp iOS 18 glyph */
    fb_fill_circle(cx, cy + 12, 44, COL_IOS_SILVER);
    fb_fill_circle(cx + 42, cy + 10, 18, COL_IOS_DK); /* classic right bite */
    fb_fill_round_rect((uint32_t)(cx - 4), (uint32_t)(cy - 54), 22U, 16U, 8U, 0xff60a5faU);
    draw_centered_text((uint32_t)cx, (uint32_t)(cy - 4), "18", 2U, COL_IOS_DK);
}

static void draw_card(uint32_t x, uint32_t y, int is_ios, int selected, int available)
{
    uint32_t bg = selected ? COL_CARD_SEL : COL_CARD;
    fb_fill_round_rect(x, y, CARD_W, CARD_H, 40U, bg);
    fb_stroke_round_rect(x, y, CARD_W, CARD_H, 40U, selected ? 5U : 2U,
                         selected ? COL_SEL_RING : COL_BORDER);

    uint32_t cx = x + CARD_W / 2U;
    if (!is_ios) {
        draw_android_emblem((int32_t)cx, (int32_t)(y + 185U));
        draw_centered_text(cx, y + 345U, "Android 17", 3U, COL_WHITE);
        draw_centered_text(cx, y + 410U, "AOSP RAM Chain", 2U, COL_MUTED);
        draw_centered_text(cx, y + 475U, available ? "boot -> 0x40080000" : "Not Attached",
                           2U, available ? COL_ANDROID : COL_ERR);
    } else {
        draw_ios_emblem((int32_t)cx, (int32_t)(y + 185U));
        draw_centered_text(cx, y + 345U, "iOS 18", 3U, COL_WHITE);
        draw_centered_text(cx, y + 410U, "XNU-11215", 2U, 0xff93c5fdU);
        draw_centered_text(cx, y + 475U, available ? "Payload Ready" : "Not Bootable",
                           2U, available ? COL_ANDROID : COL_WARN);
    }

    if (selected) {
        fb_fill_round_rect(cx - 80U, y + CARD_H - 54U, 160U, 34U, 17U, COL_SEL_RING);
        draw_centered_text(cx, y + CARD_H - 52U, "Selected", 2U, COL_WHITE);
    }
}

static void draw_target_path_box(int selected, int has_xnu)
{
    fb_fill_round_rect(60U, PATH_BOX_Y, 960U, PATH_BOX_H, 28U, COL_CARD);
    fb_stroke_round_rect(60U, PATH_BOX_Y, 960U, PATH_BOX_H, 28U, 2U, COL_BORDER);

    if (selected == BOOT_TARGET_ANDROID) {
        draw_centered_text(540U, PATH_BOX_Y + 30U, "Configured Boot Source: Android 17 (RAM)", 2U, COL_ANDROID);
        draw_centered_text(540U, PATH_BOX_Y + 84U, "Chain Image @ 0x400c0000 -> 0x40080000 (no disk write)", 2U, COL_MUTED);
    } else {
        draw_centered_text(540U, PATH_BOX_Y + 26U, "Target: " IOS_EXPECTED_VOLUME, 2U, 0xff93c5fdU);
        draw_centered_text(540U, PATH_BOX_Y + 72U, IOS_EXPECTED_KERNEL, 2U, COL_WHITE);
        draw_centered_text(540U, PATH_BOX_Y + 116U,
                           has_xnu ? "Volume attached in RAM payload" : "Status: Volume not present on UFS",
                           2U, has_xnu ? COL_ANDROID : COL_WARN);
    }
}

static void draw_continue_button(int selected)
{
    fb_fill_round_rect(CONT_X, CONT_Y, CONT_W, CONT_H, 58U, COL_SEL_RING);
    const char *label = (selected == BOOT_TARGET_ANDROID) ? "Start Android 17 ->" : "Continue (iOS 18) ->";
    draw_centered_text(CONT_X + CONT_W / 2U, CONT_Y + 34U, label, 3U, COL_WHITE);
}

static void draw_touch_footer(const struct touch_state *t)
{
    fb_fill_rect(40U, 2060U, 1000U, 190U, COL_BG);
    char line[80];
    uint32_t pos = 0;

    if (t && t->ready) {
        str_append(line, &pos, "Touch: NT36672A Ready (PID 0x");
        fmt_u32_append(line, &pos, t->pid, 1);
        str_append(line, &pos, " FW v");
        fmt_u32_append(line, &pos, t->fw_ver, 0);
        if (t->fw_uploaded) {
            str_append(line, &pos, " RAM-DL");
        }
        str_append(line, &pos, ")");
        line[pos] = '\0';
        draw_centered_text(540U, 2070U, line, 2U, COL_ANDROID);
    } else {
        draw_centered_text(540U, 2070U, "Touch: Offline on SPI5 (Use Vol+/Vol-/Power)", 2U, COL_WARN);
    }
    draw_centered_text(540U, 2135U, "Tap card to select  *  Tap Continue or Vol+/- + Power", 2U, COL_MUTED);
}

static void draw_menu(int has_android, int has_xnu, int selected, const struct touch_state *t)
{
    console_clear(COL_BG);
    draw_centered_text(540U, 120U, "Startup Manager", 4U, COL_WHITE);
    draw_centered_text(540U, 210U, "begonia-ios  *  Redmi Note 8 Pro (MT6785)", 2U, COL_MUTED);

    draw_card(CARD_AND_X, CARD_Y, 0, selected == BOOT_TARGET_ANDROID, has_android);
    draw_card(CARD_IOS_X, CARD_Y, 1, selected == BOOT_TARGET_XNU, has_xnu);
    draw_target_path_box(selected, has_xnu);
    draw_continue_button(selected);

    /* Hardware diagnostics chip */
    fb_fill_round_rect(DIAG_X, DIAG_Y, DIAG_W, DIAG_H, 46U, COL_CARD);
    fb_stroke_round_rect(DIAG_X, DIAG_Y, DIAG_W, DIAG_H, 46U, 2U, COL_BORDER);
    draw_centered_text(DIAG_X + DIAG_W / 2U, DIAG_Y + 30U, "Hardware Diagnostics", 2U, COL_IOS_SILVER);

    draw_touch_footer(t);
}

static void draw_ios_missing_modal(void)
{
    fb_fill_round_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 36U, 0xff1e2430U);
    fb_stroke_round_rect(MODAL_X, MODAL_Y, MODAL_W, MODAL_H, 36U, 4U, COL_ERR);

    fb_fill_circle(540, (int32_t)(MODAL_Y + 95U), 46, COL_ERR);
    draw_centered_text(540U, MODAL_Y + 71U, "!", 3U, COL_WHITE);

    draw_centered_text(540U, MODAL_Y + 175U, "iOS 18 Cannot Be Started", 3U, COL_WHITE);
    draw_centered_text(540U, MODAL_Y + 250U, "Expected boot volume / kernel path not found:", 2U, COL_MUTED);
    draw_centered_text(540U, MODAL_Y + 305U, IOS_EXPECTED_VOLUME IOS_EXPECTED_KERNEL, 2U, 0xff93c5fdU);
    draw_centered_text(540U, MODAL_Y + 370U, "iOS boot skipped safely. Android data untouched.", 2U, COL_ANDROID);

    fb_fill_round_rect(MODAL_OK_X, MODAL_OK_Y, MODAL_OK_W, MODAL_OK_H, 48U, COL_SEL_RING);
    draw_centered_text(MODAL_OK_X + MODAL_OK_W / 2U, MODAL_OK_Y + 24U, "OK", 3U, COL_WHITE);
}

static int in_box(uint32_t x, uint32_t y, uint32_t bx, uint32_t by, uint32_t bw, uint32_t bh)
{
    return x >= bx && x < (bx + bw) && y >= by && y < (by + bh);
}

static void show_modal_until_ack(struct touch_state *touch)
{
    draw_ios_missing_modal();
    delay_ms(120U);

    uint32_t prev_btn = sample_buttons();
    int prev_touch = 1; /* require fresh tap/release so holding doesn't auto-dismiss */
    for (;;) {
        delay_ms(20U);
        uint32_t b = sample_buttons();
        uint32_t pressed = b & ~prev_btn;
        prev_btn = b;
        if (pressed != 0U) {
            return;
        }
        if (touch && touch->ready) {
            touch_poll(touch);
            if (!touch->down) {
                prev_touch = 0;
            } else if (!prev_touch) {
                if (in_box(touch->x, touch->y, MODAL_OK_X, MODAL_OK_Y, MODAL_OK_W, MODAL_OK_H) ||
                    in_box(touch->x, touch->y, MODAL_X, MODAL_Y, MODAL_W, MODAL_H)) {
                    /* Wait for finger release cleanly */
                    while (touch->down) {
                        delay_ms(20U);
                        touch_poll(touch);
                    }
                    return;
                }
            }
        }
    }
}

void boot_menu_wait_any_input(struct touch_state *touch)
{
    delay_ms(250U);
    uint32_t prev_btn = sample_buttons();
    int had_release = 0;
    for (;;) {
        delay_ms(25U);
        uint32_t b = sample_buttons();
        uint32_t pressed = b & ~prev_btn;
        prev_btn = b;
        if (pressed != 0U) {
            return;
        }
        if (touch && touch->ready) {
            touch_poll(touch);
            if (!touch->down) {
                had_release = 1;
            } else if (had_release) {
                while (touch->down) {
                    delay_ms(20U);
                    touch_poll(touch);
                }
                return;
            }
        }
    }
}

static void draw_countdown(uint32_t left)
{
    fb_fill_rect(40U, COUNTDOWN_Y - 10U, 1000U, 56U, COL_BG);
    if (left == 0U) {
        return;
    }
    char line[80];
    uint32_t pos = 0;
    str_append(line, &pos, "Android 17 starts in ");
    if (left >= 10U) {
        line[pos++] = (char)('0' + (left / 10U) % 10U);
    }
    line[pos++] = (char)('0' + left % 10U);
    str_append(line, &pos, " s  -  tap or press a key to stay");
    line[pos] = '\0';
    draw_centered_text(540U, COUNTDOWN_Y, line, 2U, COL_ANDROID);
}

enum boot_target boot_menu_select(int has_android, int has_xnu, struct touch_state *touch)
{
    int selected = has_android ? BOOT_TARGET_ANDROID : BOOT_TARGET_XNU;
    uint32_t prev_btn = sample_buttons();
    uint32_t stable_btn = prev_btn;
    int prev_touch_down = 0;

    draw_menu(has_android, has_xnu, selected, touch);

    uint64_t freq = timer_freq();
    uint64_t t0 = timer_ticks();
    uint32_t auto_left = has_android ? AUTO_BOOT_SECONDS : 0U;
    uint32_t loops = 0;
    if (auto_left) {
        draw_countdown(auto_left);
    }

    for (;;) {
        delay_ms(20U);
        loops++;

        /* 0. Auto-boot countdown (timer based; ~50 loops per second if CNTFRQ is not set) */
        if (auto_left) {
            uint64_t elapsed = freq ? (timer_ticks() - t0) / freq : (uint64_t)(loops / 50U);
            if (elapsed >= AUTO_BOOT_SECONDS) {
                return BOOT_TARGET_ANDROID;
            }
            uint32_t left = AUTO_BOOT_SECONDS - (uint32_t)elapsed;
            if (left != auto_left) {
                auto_left = left;
                draw_countdown(left);
            }
        }

        /* 1. Hardware buttons */
        uint32_t s1 = sample_buttons();
        if (s1 == prev_btn) {
            uint32_t pressed = s1 & ~stable_btn;
            stable_btn = s1;
            if (pressed && auto_left) {
                auto_left = 0U;
                draw_countdown(0U);
            }
            if (pressed & (BTN_UP | BTN_DOWN)) {
                selected = (selected == BOOT_TARGET_ANDROID) ? BOOT_TARGET_XNU : BOOT_TARGET_ANDROID;
                draw_menu(has_android, has_xnu, selected, touch);
            } else if (pressed & BTN_POWER) {
                if (selected == BOOT_TARGET_XNU && !has_xnu) {
                    show_modal_until_ack(touch);
                    draw_menu(has_android, has_xnu, selected, touch);
                } else if (selected != BOOT_TARGET_ANDROID || has_android) {
                    return (enum boot_target)selected;
                }
            }
        } else {
            prev_btn = s1;
        }

        /* 2. Capacitive touchscreen polling */
        if (touch && touch->ready) {
            touch_poll(touch);
            int tapped = (!prev_touch_down && touch->down);
            prev_touch_down = touch->down;
            if (!tapped) {
                continue;
            }
            if (auto_left) {
                auto_left = 0U;
                draw_countdown(0U);
            }
            uint32_t tx = touch->x;
            uint32_t ty = touch->y;

            if (in_box(tx, ty, CARD_AND_X, CARD_Y, CARD_W, CARD_H)) {
                if (selected == BOOT_TARGET_ANDROID && has_android) {
                    return BOOT_TARGET_ANDROID;
                }
                selected = BOOT_TARGET_ANDROID;
                draw_menu(has_android, has_xnu, selected, touch);
            } else if (in_box(tx, ty, CARD_IOS_X, CARD_Y, CARD_W, CARD_H)) {
                if (selected == BOOT_TARGET_XNU) {
                    if (!has_xnu) {
                        show_modal_until_ack(touch);
                        draw_menu(has_android, has_xnu, selected, touch);
                    } else {
                        return BOOT_TARGET_XNU;
                    }
                } else {
                    selected = BOOT_TARGET_XNU;
                    draw_menu(has_android, has_xnu, selected, touch);
                }
            } else if (in_box(tx, ty, CONT_X, CONT_Y, CONT_W, CONT_H)) {
                if (selected == BOOT_TARGET_XNU && !has_xnu) {
                    show_modal_until_ack(touch);
                    draw_menu(has_android, has_xnu, selected, touch);
                } else if (selected == BOOT_TARGET_ANDROID && has_android) {
                    return BOOT_TARGET_ANDROID;
                } else if (selected == BOOT_TARGET_XNU && has_xnu) {
                    return BOOT_TARGET_XNU;
                }
            } else if (in_box(tx, ty, DIAG_X, DIAG_Y, DIAG_W, DIAG_H)) {
                return BOOT_TARGET_DIAG;
            }
        }
    }
}
