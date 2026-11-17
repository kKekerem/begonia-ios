/*
 * Hardware definitions for Redmi Note 8 Pro (begonia / MediaTek MT6785).
 * Base addresses are taken from mt6785.dts, xiaomi-mt6785-common.dtsi, and BoardConfig.mk.
 */
#ifndef BEGONIA_HW_H
#define BEGONIA_HW_H

#include <stdint.h>

#define BEGONIA_KERNEL_PA       0x40080000ULL
#define BEGONIA_CHAIN_OFFSET    0x00040000ULL
#define BEGONIA_CHAIN_STUB_PA   0x46000000ULL

/* GICv3 & MediaTek interrupt polarity inverter */
#define MT6785_GICD_BASE        0x0c000000ULL
#define MT6785_GICR_BASE        0x0c040000ULL
#define MT6785_INT_POL_BASE     0x0c53a650ULL

/* System timer frequency (13 MHz on MT6785) */
#define MT6785_CNTFRQ_FALLBACK  13000000ULL

/* Watchdog (TOPRGU) */
#define MT6785_TOPRGU_BASE      0x10007000ULL
#define TOPRGU_MODE             (MT6785_TOPRGU_BASE + 0x00u)
#define TOPRGU_LENGTH           (MT6785_TOPRGU_BASE + 0x04u)
#define TOPRGU_RESTART          (MT6785_TOPRGU_BASE + 0x08u)
#define TOPRGU_SWRST            (MT6785_TOPRGU_BASE + 0x14u)
#define TOPRGU_MODE_KEY         0x22000000u
#define TOPRGU_RESTART_KEY      0x1971u
#define TOPRGU_SWRST_KEY        0x1209u

/* PMIC wrapper & MT6359 PMIC registers (pwrap_hal.h / mt6785 upmu_hw.h) */
#define MT6785_PWRAP_BASE       0x1000d000ULL
#define PWRAP_WACS2_EN          (MT6785_PWRAP_BASE + 0x09cu)
#define PWRAP_WACS2_CMD         (MT6785_PWRAP_BASE + 0xc20u)
#define PWRAP_WACS2_RDATA       (MT6785_PWRAP_BASE + 0xc24u)
#define PWRAP_WACS2_VLDCLR      (MT6785_PWRAP_BASE + 0xc28u)
#define PWRAP_FSM(rdata)        (((rdata) >> 16) & 7u)
#define PWRAP_FSM_IDLE          0u
#define PWRAP_FSM_WFVLDCLR      6u

#define MT6359_HWCID            0x0008u
#define MT6359_TOPSTATUS        0x002au
#define MT6359_PWRKEY_DEB_BIT   (1u << 1)
#define MT6359_HOMEKEY_DEB_BIT  (1u << 3)

/* Keypad controller (hal_kpd.h) */
#define MT6785_KEYPAD_BASE      0x10010000ULL
#define KP_MEM1                 (MT6785_KEYPAD_BASE + 0x0004u)

/* Early UART0 (8250-compatible MTK APUART0) */
#define MT6785_UART0_BASE       0x11002000ULL
#define UART_THR                0x00u
#define UART_LSR                0x14u
#define UART_LSR_THRE           0x20u

/* High-speed peripherals (for bring-up probing only) */
#define MT6785_MTU3_MAC_BASE    0x11201000ULL
#define MT6785_MTU3_IPPC_BASE   0x11203e00ULL
#define MT6785_UFSHCI_BASE      0x11270000ULL
#define UFSHCI_VER              0x08u
#define MT6785_MALI_BASE        0x13040000ULL

/* Display pipeline (mt6785.dts: disp_ovl0@14008000, disp_mutex at 0x14016000) */
#define MT6785_DISP_OVL0_BASE   0x14008000ULL
#define MT6785_DISP_OVL0_2L     0x14009000ULL
#define MT6785_DISP_RDMA0_BASE  0x1400b000ULL
#define MT6785_DISP_MUTEX_BASE  0x14016000ULL

#define OVL_SRC_CON             0x02cu
#define OVL_L0_CON              0x030u
#define OVL_L0_SRC_SIZE         0x038u
#define OVL_L0_PITCH            0x044u
#define OVL_L0_ADDR             0xf40u
#define OVL_LAYER_STRIDE        0x020u
#define DISP_MUTEX0_EN          (MT6785_DISP_MUTEX_BASE + 0x020u)
#define DISP_MUTEX0_RST         (MT6785_DISP_MUTEX_BASE + 0x028u)

/* Panel & fallback LK framebuffer (begonia live DTB videolfb: 0x7d9b0000, vram 0x02250000) */
#define BEGONIA_PANEL_W         1080u
#define BEGONIA_PANEL_H         2340u
#define BEGONIA_FB_STRIDE_DEF   4352u
#define BEGONIA_FB_FALLBACK     0x7d9b0000ULL
#define BEGONIA_FB_FALLBACK_SZ  0x02250000ULL

/* Clocks, GPIO & SPI5 (Novatek NT36672A touchscreen controller on begonia) */
#define MT6785_TOPCKGEN_BASE    0x10000000ULL
#define TOPCK_CLK_CFG_UPDATE    (MT6785_TOPCKGEN_BASE + 0x004u)
#define TOPCK_CLK_CFG_5_CLR     (MT6785_TOPCKGEN_BASE + 0x078u)
#define MT6785_INFRACFG_AO_BASE 0x10001000ULL
#define INFRA_SW_CG2_CLR        (MT6785_INFRACFG_AO_BASE + 0x0a8u)
#define INFRA_CG2_SPI5_BIT      (1u << 26)

#define MT6785_GPIO_BASE        0x10005000ULL
#define MT6785_SPI5_BASE        0x11019000ULL
#define SPI_CFG0_REG            0x0000u
#define SPI_CFG1_REG            0x0004u
#define SPI_TX_DATA_REG         0x0010u
#define SPI_RX_DATA_REG         0x0014u
#define SPI_CMD_REG             0x0018u
#define SPI_STATUS0_REG         0x001cu
#define SPI_PAD_SEL_REG         0x0024u
#define SPI_CFG2_REG            0x0028u

/* MMIO helpers (MMU is off, so all memory is Device-nGnRnE) */
static inline uint32_t mmio_read32(uint64_t addr)
{
    return *(volatile uint32_t *)addr;
}

static inline void mmio_write32(uint64_t addr, uint32_t val)
{
    *(volatile uint32_t *)addr = val;
}

#endif /* BEGONIA_HW_H */
