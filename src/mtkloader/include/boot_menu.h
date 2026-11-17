/*
 * PMIC wrapper (MT6359) + Keypad (MT6785) polling and GRUB-style Dual-Boot Menu.
 */
#ifndef BOOT_MENU_H
#define BOOT_MENU_H

#include <stdint.h>

enum boot_target {
    BOOT_TARGET_ANDROID = 0,
    BOOT_TARGET_XNU     = 1,
    BOOT_TARGET_DIAG    = 2,
};

/* Safely read a 16-bit MT6359 register over PWRAP WACS2. Returns 1 on success. */
int pmic_read16(uint16_t reg, uint16_t *out);

/* Render interactive dual-boot menu with countdown; returns chosen boot_target. */
enum boot_target boot_menu_select(int has_android, int has_xnu);

#endif /* BOOT_MENU_H */
