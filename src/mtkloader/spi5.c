/*
 * Polled MediaTek MT6785 SPI5 FIFO master and GPIO helpers.
 */
#include "spi5.h"
#include "begonia_hw.h"
#include "console.h"

#define SPI_CMD_ACT                 (1u << 0)
#define SPI_CMD_RESUME              (1u << 1)
#define SPI_CMD_RST                 (1u << 2)
#define SPI_CMD_PAUSE_EN            (1u << 4)
#define SPI_CMD_TXMSBF              (1u << 12)
#define SPI_CMD_RXMSBF              (1u << 13)
#define SPI_CMD_FINISH_IE           (1u << 16)
#define SPI_CMD_PAUSE_IE            (1u << 17)
#define SPI_STATUS1_REG             0x0020u
#define SPI_FIFO_BYTES              32u

uint32_t spi5_fail_count;
uint32_t spi5_last_fail;

void spi5_gpio_set_mode(uint32_t pin, uint32_t mode)
{
    uint64_t reg = MT6785_GPIO_BASE + 0x0300ULL + (uint64_t)(pin / 8u) * 0x10ULL;
    uint32_t shift = (pin % 8u) * 4u;
    uint32_t v = mmio_read32(reg);
    v = (v & ~(0xfu << shift)) | ((mode & 0xfu) << shift);
    mmio_write32(reg, v);
}

void spi5_gpio_set_dir(uint32_t pin, int out)
{
    uint64_t reg = MT6785_GPIO_BASE + 0x0000ULL + (uint64_t)(pin / 32u) * 0x10ULL;
    uint32_t bit = 1u << (pin % 32u);
    uint32_t v = mmio_read32(reg);
    mmio_write32(reg, out ? (v | bit) : (v & ~bit));
}

void spi5_gpio_set_dout(uint32_t pin, int val)
{
    uint64_t reg = MT6785_GPIO_BASE + 0x0100ULL + (uint64_t)(pin / 32u) * 0x10ULL;
    uint32_t bit = 1u << (pin % 32u);
    uint32_t v = mmio_read32(reg);
    mmio_write32(reg, val ? (v | bit) : (v & ~bit));
}

static void spi5_delay_us(uint32_t us)
{
    uint64_t end = timer_ticks() + (timer_freq() * us + 999999ULL) / 1000000ULL;
    while (timer_ticks() < end) {
    }
}

/*
 * 25 Nov 2026: SPI5 transfers stalled now and then (about a third of the iOS touch polls; every firmware
 * download in here died on one of its ~7000 writes even with 6 retries each). SPI_CMD_RST was set and cleared
 * back to back and the TX FIFO filled right after it, but the reset runs in the 26 MHz SPI clock domain: a pulse
 * a few bus cycles long is caught late or not at all, TX words written while it is still active are dropped and
 * the controller (must_tx) then waits for data that never comes, so FINISH is never set. Linux resets only when
 * CS is deasserted at the end of a message and fills the FIFO much later. The reset is now held and released for
 * 2 us each (52 SPI clocks), with read-backs so each write has reached SPI5 before the wait starts.
 */
static void spi5_reset(void)
{
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, SPI_CMD_RST);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_CMD_REG);
    spi5_delay_us(2u);
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, 0u);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_CMD_REG);
    spi5_delay_us(2u);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_STATUS0_REG); /* read-clear a stale FINISH/PAUSE */
}

void spi5_hw_init(void)
{
    /* Select clk26m (0) and clear PDN_SPI (bit 15) on TOPCKGEN CLK_CFG_5 */
    mmio_write32(TOPCK_CLK_CFG_5_CLR, (0x3u << 8) | (1u << 15));
    mmio_write32(TOPCK_CLK_CFG_UPDATE, (1u << 21));

    /* Ungate INFRACFG_AO_SPI5_CG (infra2 bit 26) */
    mmio_write32(INFRA_SW_CG2_CLR, INFRA_CG2_SPI5_BIT);

    /* begonia live pinmux: GPIO1=INT, GPIO2=RST (keep high), GPIO35..38=SPI5_A (func 5) */
    spi5_gpio_set_mode(1u, 0u);
    spi5_gpio_set_dir(1u, 0);
    spi5_gpio_set_dout(2u, 1);
    spi5_gpio_set_mode(2u, 0u);
    spi5_gpio_set_dir(2u, 1);

    spi5_gpio_set_mode(35u, 5u);
    spi5_gpio_set_dir(35u, 1);
    spi5_gpio_set_mode(36u, 5u);
    spi5_gpio_set_dir(36u, 1);
    spi5_gpio_set_mode(37u, 5u);
    spi5_gpio_set_dir(37u, 0);
    spi5_gpio_set_mode(38u, 5u);
    spi5_gpio_set_dir(38u, 1);

    /* mt6765_compat enhance_timing: 26 MHz / 6 = ~4.33 MHz, pad-select = 0 */
    spi5_delay_us(10u);   /* the clock mux and gate writes above settle */
    spi5_reset();
    mmio_write32(MT6785_SPI5_BASE + SPI_PAD_SEL_REG, 0u);
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG2_REG, ((3u - 1u) << 0) | ((3u - 1u) << 16));
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG0_REG, ((6u - 1u) << 0) | ((6u - 1u) << 16));
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG1_REG, 5u);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_CFG1_REG);
}

/* FINISH (bit 0) or PAUSE (bit 1); 2 ms is ~30x the longest 32-byte transfer at 4.33 MHz */
static int spi5_wait_status(void)
{
    uint64_t end = timer_ticks() + timer_freq() / 500ULL;
    for (;;) {
        uint32_t st = mmio_read32(MT6785_SPI5_BASE + SPI_STATUS0_REG);
        if (st & 3u) {
            return 0;
        }
        if (timer_ticks() > end) {
            return -1;
        }
    }
}

/*
 * 25 Nov 2026 (v22 log: the loader's touch FW start failed 3/3 with 1408 SPI timeouts, dbg 0x13): begonia's own
 * Linux driver (drivers/spi/spi-mt65xx.c, mt6765_compat) never lets the controller FINISH a transfer. set_cs()
 * sets PAUSE_EN before the first transfer of a message, every FIFO load ends in PAUSE (CS stays asserted), the
 * next one starts with RESUME, and the message ends with PAUSE_EN cleared and a reset (CS deasserted) while the
 * controller is idle. Here a reset ran before every transfer and single-load transfers ran to FINISH; the stalls
 * (busy, no FINISH) came in runs that the reset did not end. Now the same sequence as Linux; after a stall the
 * timing registers are written again too.
 */
static void spi5_setup_regs(void)
{
    mmio_write32(MT6785_SPI5_BASE + SPI_PAD_SEL_REG, 0u);
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG2_REG, ((3u - 1u) << 0) | ((3u - 1u) << 16));
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG0_REG, ((6u - 1u) << 0) | ((6u - 1u) << 16));
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG1_REG, 5u);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_CFG1_REG);
}

int spi5_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if (len == 0u) {
        return 0;
    }

    uint32_t base_cmd = SPI_CMD_TXMSBF | SPI_CMD_RXMSBF | SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE |
                        SPI_CMD_PAUSE_EN;
    uint32_t off = 0;
    int rc = 0;

    (void)mmio_read32(MT6785_SPI5_BASE + SPI_STATUS0_REG);     /* read-clear a stale FINISH/PAUSE */
    /* mode bits and PAUSE_EN first (Linux prepare_message + set_cs), the start bit later in its own write */
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, base_cmd);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_CMD_REG);

    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > SPI_FIFO_BYTES) {
            chunk = SPI_FIFO_BYTES;
        }
        int is_first = (off == 0u);

        uint32_t cfg1 = mmio_read32(MT6785_SPI5_BASE + SPI_CFG1_REG);
        cfg1 &= ~0x03ffff00u;
        cfg1 |= ((chunk - 1u) << 16);
        mmio_write32(MT6785_SPI5_BASE + SPI_CFG1_REG, cfg1);

        for (uint32_t i = 0; i < chunk; i += 4u) {
            uint32_t w = 0u;
            uint32_t rem = chunk - i;
            if (rem > 4u) {
                rem = 4u;
            }
            if (tx) {
                for (uint32_t b = 0; b < rem; b++) {
                    w |= ((uint32_t)tx[off + i + b]) << (b * 8u);
                }
            }
            mmio_write32(MT6785_SPI5_BASE + SPI_TX_DATA_REG, w);
        }

        uint32_t cmd = base_cmd | (is_first ? SPI_CMD_ACT : SPI_CMD_RESUME);
        mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, cmd);

        if (spi5_wait_status() != 0) {
            spi5_fail_count++;
            /* [31:24] STATUS1 (bit 0 busy), [23:16] CMD low byte, [15:0] transfer length */
            spi5_last_fail = ((mmio_read32(MT6785_SPI5_BASE + SPI_STATUS1_REG) & 0xffu) << 24) |
                             ((cmd & 0xffu) << 16) | (len & 0xffffu);
            rc = -1;
            break;
        }

        for (uint32_t i = 0; i < chunk; i += 4u) {
            uint32_t w = mmio_read32(MT6785_SPI5_BASE + SPI_RX_DATA_REG);
            if (rx) {
                uint32_t rem = chunk - i;
                if (rem > 4u) {
                    rem = 4u;
                }
                for (uint32_t b = 0; b < rem; b++) {
                    rx[off + i + b] = (uint8_t)(w >> (b * 8u));
                }
            }
        }
        off += chunk;
    }
    /* end of the message (Linux set_cs(deassert)): PAUSE_EN off, then the reset deasserts CS */
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, base_cmd & ~SPI_CMD_PAUSE_EN);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_CMD_REG);
    spi5_reset();
    if (rc != 0) {
        spi5_setup_regs();
    }
    return rc;
}
