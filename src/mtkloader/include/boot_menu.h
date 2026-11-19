/*
 * PMIC wrapper (MT6359) + Keypad (MT6785) + NT36672A touch macOS-style Dual-Boot Picker.
 */
#ifndef BOOT_MENU_H
#define BOOT_MENU_H

#include <stdint.h>
#include "touch.h"

enum boot_target {
    BOOT_TARGET_ANDROID = 0,
    BOOT_TARGET_XNU     = 1,
    BOOT_TARGET_DIAG    = 2,
};

#define IOS_EXPECTED_VOLUME "gpt:/iosroot"
#define IOS_EXPECTED_KERNEL "/System/Library/Kernels/kernel.macho"

/* Safely read a 16-bit MT6359 register over PWRAP WACS2. Returns 1 on success. */
int pmic_read16(uint16_t reg, uint16_t *out);

/* Render interactive macOS-style dual-boot picker; returns chosen boot_target. */
enum boot_target boot_menu_select(int has_android, int has_xnu, struct touch_state *touch);

/* Block until a screen tap or physical key press occurs (for diagnostics return). */
void boot_menu_wait_any_input(struct touch_state *touch);

#endif /* BOOT_MENU_H */
