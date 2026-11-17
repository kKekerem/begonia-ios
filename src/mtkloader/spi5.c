/*
 * Polled MediaTek MT6785 SPI5 FIFO master and GPIO helpers.
 */
#include "spi5.h"
#include "begonia_hw.h"

#define SPI_CMD_ACT                 (1u << 0)
#define SPI_CMD_RESUME              (1u << 1)
#define SPI_CMD_RST                 (1u << 2)
#define SPI_CMD_PAUSE_EN            (1u << 4)
#define SPI_CMD_TXMSBF              (1u << 12)
#define SPI_CMD_RXMSBF              (1u << 13)
#define SPI_CMD_FINISH_IE           (1u << 16)
#define SPI_CMD_PAUSE_IE            (1u << 17)
#define SPI_FIFO_BYTES              32u

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
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, SPI_CMD_RST);
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, 0u);
    mmio_write32(MT6785_SPI5_BASE + SPI_PAD_SEL_REG, 0u);
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG2_REG, ((3u - 1u) << 0) | ((3u - 1u) << 16));
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG0_REG, ((6u - 1u) << 0) | ((6u - 1u) << 16));
    mmio_write32(MT6785_SPI5_BASE + SPI_CFG1_REG, 5u);
}

static int spi5_wait_status(void)
{
    for (uint32_t spin = 0; spin < 250000u; spin++) {
        uint32_t st = mmio_read32(MT6785_SPI5_BASE + SPI_STATUS0_REG);
        if (st & 3u) {
            return 0;
        }
    }
    return -1;
}

int spi5_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    if (len == 0u) {
        return 0;
    }

    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, SPI_CMD_RST);
    mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, 0u);
    (void)mmio_read32(MT6785_SPI5_BASE + SPI_STATUS0_REG);

    uint32_t base_cmd = SPI_CMD_TXMSBF | SPI_CMD_RXMSBF | SPI_CMD_FINISH_IE | SPI_CMD_PAUSE_IE;
    uint32_t off = 0;

    while (off < len) {
        uint32_t chunk = len - off;
        if (chunk > SPI_FIFO_BYTES) {
            chunk = SPI_FIFO_BYTES;
        }
        int has_more = (off + chunk) < len;
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

        uint32_t cmd = base_cmd | (has_more ? SPI_CMD_PAUSE_EN : 0u) |
                       (is_first ? SPI_CMD_ACT : SPI_CMD_RESUME);
        mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, cmd);

        if (spi5_wait_status() != 0) {
            mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, SPI_CMD_RST);
            mmio_write32(MT6785_SPI5_BASE + SPI_CMD_REG, 0u);
            return -1;
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
    return 0;
}
