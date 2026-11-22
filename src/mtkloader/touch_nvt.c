/*
 * Novatek NT36672A touchscreen protocol & HW-CRC host firmware download for begonia.
 */
#include "touch.h"
#include "begonia_hw.h"
#include "console.h"
#include "spi5.h"

#define NVT_EVENT_BUF_ADDR          0x21C00u
#define NVT_SWRST_N8_ADDR           0x3F0FEu
#define NVT_SPI_RD_FAST_ADDR        0x3F310u
#define NVT_BOOT_RDY_ADDR           0x3F10Du
#define NVT_ILM_LENGTH_ADDR         0x3F118u
#define NVT_ILM_DES_ADDR            0x3F128u
#define NVT_DLM_DES_ADDR            0x3F12Cu
#define NVT_DLM_LENGTH_ADDR         0x3F130u
#define NVT_G_ILM_CHECKSUM_ADDR     0x3F100u
#define NVT_G_DLM_CHECKSUM_ADDR     0x3F104u
#define NVT_BLD_CRC_EN_ADDR         0x3F30Eu
#define NVT_CHIP_ID_ADDR            0x1F600u

#define EVENT_MAP_HOST_CMD          0x50u
#define EVENT_MAP_RESET_COMPLETE    0x60u
#define EVENT_MAP_FWINFO            0x78u
#define EVENT_MAP_PROJECTID         0x9Au

#define NVT_FW_BLOCK_SIZE           0x1B000u
#define NVT_MAX_PARTITIONS          24u

struct nvt_part {
    uint32_t sram_addr;
    uint32_t size;
    uint32_t bin_addr;
    uint32_t crc;
};

static uint32_t read_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* 22 Nov: SPI5 transfers to the NT36672A fail now and then (plog dbg 0xef0013: a 16-byte FW-info read and an SRAM
 * write); retry each one instead of giving up, which used to end in a bootloader reset that wiped the firmware
 * Android had loaded. */
static int nvt_xfer(const uint8_t *tx, uint8_t *rx, uint32_t len)
{
    for (uint32_t tries = 0; tries < 6u; tries++) {
        if (spi5_xfer(tx, rx, len) == 0) {
            return 0;
        }
        delay_ms(1u);
    }
    return -1;
}

static int nvt_spi_write(const uint8_t *buf, uint32_t len)
{
    uint8_t tx[140];
    if (len == 0u || len > sizeof(tx)) {
        return -1;
    }
    for (uint32_t i = 0; i < len; i++) {
        tx[i] = buf[i];
    }
    tx[0] |= 0x80u;
    return nvt_xfer(tx, 0, len);
}

static int nvt_spi_read(uint8_t reg_addr, uint8_t *out, uint32_t data_len)
{
    uint8_t tx[72];
    uint8_t rx[72];
    uint32_t total = data_len + 2u; /* 1 cmd + 1 DUMMY_BYTE + data_len */
    if (total > sizeof(tx)) {
        return -1;
    }
    for (uint32_t i = 0; i < total; i++) {
        tx[i] = 0u;
    }
    tx[0] = reg_addr & 0x7fu;
    if (nvt_xfer(tx, rx, total) != 0) {
        return -1;
    }
    for (uint32_t i = 0; i < data_len; i++) {
        out[i] = rx[2u + i];
    }
    return 0;
}

static int nvt_set_page(uint32_t addr)
{
    uint8_t cmd[3] = {
        0xFFu,
        (uint8_t)((addr >> 15) & 0xFFu),
        (uint8_t)((addr >> 7) & 0xFFu),
    };
    return nvt_spi_write(cmd, 3u);
}

static int nvt_write_addr(uint32_t addr, uint8_t data)
{
    if (nvt_set_page(addr) != 0) {
        return -1;
    }
    uint8_t cmd[2] = { (uint8_t)(addr & 0x7fu), data };
    return nvt_spi_write(cmd, 2u);
}

static void nvt_bootloader_reset(void)
{
    (void)nvt_write_addr(NVT_SWRST_N8_ADDR, 0x69u);
    delay_ms(5u);
    (void)nvt_write_addr(NVT_SPI_RD_FAST_ADDR, 0x00u);
}

static int nvt_probe_chip_id(uint8_t out_id[6])
{
    if (nvt_set_page(NVT_CHIP_ID_ADDR) != 0) {
        return 0;
    }
    if (nvt_spi_read(0x4Eu, out_id, 6u) != 0) {
        return 0;
    }
    if (out_id[5] != 0x03u) {
        return 0;
    }
    if ((out_id[0] == 0x0Au || out_id[0] == 0x0Bu) &&
        (out_id[4] == 0x66u || out_id[4] == 0x65u || out_id[4] == 0x67u)) {
        return 1;
    }
    return 0;
}

static int nvt_read_fw_info(struct touch_state *st)
{
    uint8_t info[16];
    if (nvt_set_page(NVT_EVENT_BUF_ADDR | EVENT_MAP_FWINFO) != 0) {
        st->dbg = (st->dbg & ~0xff0000U) | 0xee0000U;
        return 0;
    }
    if (nvt_spi_read(EVENT_MAP_FWINFO, info, sizeof(info)) != 0) {
        st->dbg = (st->dbg & ~0xff0000U) | 0xef0000U;
        return 0;
    }
    st->dbg = (st->dbg & 0xffffU) | ((uint32_t)info[0] << 16) | ((uint32_t)info[1] << 24);
    if ((uint8_t)(info[0] + info[1]) != 0xFFu || info[0] == 0u) {
        return 0;
    }
    st->fw_ver = info[0];

    uint8_t pid_buf[2];
    if (nvt_set_page(NVT_EVENT_BUF_ADDR | EVENT_MAP_PROJECTID) == 0 &&
        nvt_spi_read(EVENT_MAP_PROJECTID, pid_buf, 2u) == 0) {
        st->pid = (uint16_t)(((uint16_t)pid_buf[1] << 8) | pid_buf[0]);
    }
    (void)nvt_set_page(NVT_EVENT_BUF_ADDR);
    return 1;
}

static const uint8_t *find_builtin_nvt_fw(uintptr_t img_pa, uint64_t img_len)
{
    if (!img_pa || img_len < NVT_FW_BLOCK_SIZE + 4096ULL) {
        return 0;
    }
    const uint8_t *base = (const uint8_t *)img_pa;
    const uint8_t *best = 0;
    uint64_t limit = img_len - NVT_FW_BLOCK_SIZE;

    for (uint64_t off = 0; off <= limit; off += 16ULL) {
        const uint8_t *p = base + off;
        uint32_t ilm_bin = read_le32(p);
        if (ilm_bin < 0x30u || ilm_bin > 0x200u || (ilm_bin & 0xfu) != 0u) {
            continue;
        }
        if (read_le32(p + 4u) != 0u) {
            continue;
        }
        if (p[NVT_FW_BLOCK_SIZE - 3u] != 'N' || p[NVT_FW_BLOCK_SIZE - 2u] != 'V' ||
            p[NVT_FW_BLOCK_SIZE - 1u] != 'T') {
            continue;
        }
        const uint8_t *tail = p + (NVT_FW_BLOCK_SIZE - 4096u);
        uint8_t ver = tail[0];
        uint8_t vbar = tail[1];
        if ((uint8_t)(ver + vbar) != 0xFFu || ver == 0u || tail[13] != 0x01u) {
            continue;
        }
        /* Reject debug/test >= 0x80 versions; prefer CSOT PID 0x5921 */
        if (ver < 0x80u && tail[0x22] == 0x21u && tail[0x23] == 0x59u) {
            return p;
        }
        if (!best && ver < 0x80u) {
            best = p;
        }
    }
    return best;
}

static int nvt_write_sram(const uint8_t *fw, uint32_t sram_addr, uint32_t size, uint32_t bin_addr)
{
    uint8_t pkt[129];
    uint32_t pos = 0;
    while (pos < size) {
        uint32_t page_rem = 128u - ((sram_addr + pos) & 0x7fu);
        uint32_t n = size - pos;
        if (n > page_rem) {
            n = page_rem;
        }
        if (n > 28u) {
            n = 28u; /* fit 1 addr byte + data inside a single 32-byte SPI5 FIFO transfer */
        }
        if (nvt_set_page(sram_addr + pos) != 0) {
            return -1;
        }
        pkt[0] = (uint8_t)((sram_addr + pos) & 0x7fu);
        for (uint32_t i = 0; i < n; i++) {
            pkt[1u + i] = fw[bin_addr + pos + i];
        }
        if (nvt_spi_write(pkt, n + 1u) != 0) {
            return -1;
        }
        pos += n;
    }
    return 0;
}

static void nvt_set_crc_bank(uint32_t des_addr, uint32_t sram_addr,
                             uint32_t len_addr, uint32_t sz,
                             uint32_t chk_addr, uint32_t crc)
{
    (void)nvt_set_page(des_addr);
    uint8_t b0[4] = {
        (uint8_t)(des_addr & 0x7fu),
        (uint8_t)(sram_addr & 0xffu),
        (uint8_t)((sram_addr >> 8) & 0xffu),
        (uint8_t)((sram_addr >> 16) & 0xffu),
    };
    (void)nvt_spi_write(b0, 4u);

    uint8_t b1[3] = {
        (uint8_t)(len_addr & 0x7fu),
        (uint8_t)(sz & 0xffu),
        (uint8_t)((sz >> 8) & 0xffu),
    };
    (void)nvt_spi_write(b1, 3u);

    uint8_t b2[5] = {
        (uint8_t)(chk_addr & 0x7fu),
        (uint8_t)(crc & 0xffu),
        (uint8_t)((crc >> 8) & 0xffu),
        (uint8_t)((crc >> 16) & 0xffu),
        (uint8_t)((crc >> 24) & 0xffu),
    };
    (void)nvt_spi_write(b2, 5u);
}

static int nvt_download_fw(const uint8_t *fw, struct touch_state *st)
{
    struct nvt_part parts[NVT_MAX_PARTITIONS];
    uint32_t hdr_end = read_le32(fw);
    uint32_t info_num = (hdr_end > 0x30u) ? ((hdr_end - 0x30u) / 16u) : 0u;
    uint8_t ovly = fw[0x28];
    uint32_t ovly_num = (ovly & 0x10u) ? (ovly & 0x0fu) : 0u;
    uint32_t total = 2u + info_num + ovly_num;
    if (total > NVT_MAX_PARTITIONS) {
        st->dbg = (st->dbg & ~0xffU) | 0x11U;
        return -1;
    }

    for (uint32_t i = 0; i < 2u; i++) {
        parts[i].bin_addr = read_le32(fw + i * 12u);
        parts[i].sram_addr = read_le32(fw + i * 12u + 4u);
        parts[i].size = read_le32(fw + i * 12u + 8u);
        parts[i].crc = read_le32(fw + 0x18u + i * 4u);
    }
    for (uint32_t i = 0; i < info_num; i++) {
        uint32_t p = 0x30u + i * 16u;
        parts[2u + i].sram_addr = read_le32(fw + p);
        parts[2u + i].size = read_le32(fw + p + 4u);
        parts[2u + i].bin_addr = read_le32(fw + p + 8u);
        parts[2u + i].crc = read_le32(fw + p + 12u);
    }
    uint32_t dlm_bin = parts[1].bin_addr;
    for (uint32_t i = 0; i < ovly_num; i++) {
        uint32_t p = dlm_bin + i * 16u;
        parts[2u + info_num + i].sram_addr = read_le32(fw + p);
        parts[2u + info_num + i].size = read_le32(fw + p + 4u);
        parts[2u + info_num + i].bin_addr = read_le32(fw + p + 8u);
        parts[2u + info_num + i].crc = read_le32(fw + p + 12u);
    }

    nvt_bootloader_reset();

    for (uint32_t i = 0; i < total; i++) {
        if (parts[i].size == 0u) {
            continue;
        }
        uint32_t sz = parts[i].size + 1u;
        if (parts[i].bin_addr + sz > NVT_FW_BLOCK_SIZE) {
            st->dbg = (st->dbg & ~0xffU) | 0x12U;
            return -1;
        }
        if (nvt_write_sram(fw, parts[i].sram_addr, sz, parts[i].bin_addr) != 0) {
            st->dbg = (st->dbg & ~0xffU) | 0x13U;
            return -1;
        }
    }

    nvt_set_crc_bank(NVT_ILM_DES_ADDR, parts[0].sram_addr, NVT_ILM_LENGTH_ADDR,
                     parts[0].size, NVT_G_ILM_CHECKSUM_ADDR, parts[0].crc);
    nvt_set_crc_bank(NVT_DLM_DES_ADDR, parts[1].sram_addr, NVT_DLM_LENGTH_ADDR,
                     parts[1].size, NVT_G_DLM_CHECKSUM_ADDR, parts[1].crc);

    uint8_t en = 0;
    if (nvt_set_page(NVT_BLD_CRC_EN_ADDR) == 0 &&
        nvt_spi_read((uint8_t)(NVT_BLD_CRC_EN_ADDR & 0x7fu), &en, 1u) == 0) {
        (void)nvt_write_addr(NVT_BLD_CRC_EN_ADDR, (uint8_t)(en | 0x80u));
    }

    (void)nvt_set_page(NVT_EVENT_BUF_ADDR);
    uint8_t clr[2] = { EVENT_MAP_RESET_COMPLETE, 0x00u };
    uint8_t ae[2] = { EVENT_MAP_HOST_CMD, 0xAEu };
    (void)nvt_spi_write(clr, 2u);
    (void)nvt_spi_write(ae, 2u);

    (void)nvt_write_addr(NVT_BOOT_RDY_ADDR, 0x01u);
    delay_ms(10u);

    for (uint32_t retry = 0; retry < 40u; retry++) {
        uint8_t rst_state = 0;
        if (nvt_set_page(NVT_EVENT_BUF_ADDR) == 0 &&
            nvt_spi_read(EVENT_MAP_RESET_COMPLETE, &rst_state, 1u) == 0) {
            st->dbg = (st->dbg & ~0xff00U) | ((uint32_t)rst_state << 8);
            if (rst_state >= 0xA0u && rst_state <= 0xAFu) {
                return 0;
            }
        }
        delay_ms(10u);
    }
    st->dbg = (st->dbg & ~0xffU) | 0x14U;
    return -1;
}

void touch_init(uintptr_t android_img_pa, uint64_t android_img_len, struct touch_state *out)
{
    for (uint32_t i = 0; i < (uint32_t)sizeof(*out); i++) {
        ((uint8_t *)out)[i] = 0u;
    }
    spi5_hw_init();

    /* firmware already running (Android loaded it before a warm reboot): never reset it on one failed read */
    for (uint32_t tries = 0; tries < 5u; tries++) {
        if (nvt_read_fw_info(out)) {
            out->ready = 1;
            return;
        }
        delay_ms(20u);
    }

    int id_ok = nvt_probe_chip_id(out->chip_id);
    if (!id_ok) {
        nvt_bootloader_reset();
        id_ok = nvt_probe_chip_id(out->chip_id);
    }
    if (!id_ok) {
        spi5_gpio_set_dout(2u, 0);
        delay_ms(2u);
        spi5_gpio_set_dout(2u, 1);
        delay_ms(10u);
        nvt_bootloader_reset();
        id_ok = nvt_probe_chip_id(out->chip_id);
    }
    if (!id_ok) {
        out->dbg = 0x02U;
        return;
    }

    if (nvt_read_fw_info(out)) {
        out->ready = 1;
        out->dbg |= 0x01U;
        return;
    }

    const uint8_t *fw = find_builtin_nvt_fw(android_img_pa, android_img_len);
    if (!fw) {
        out->dbg = (out->dbg & ~0xffU) | 0x03U;
        return;
    }
    if (nvt_download_fw(fw, out) == 0 && nvt_read_fw_info(out)) {
        out->fw_uploaded = 1;
        out->ready = 1;
        out->dbg = (out->dbg & ~0xffU) | 0x07U;
    } else if ((out->dbg & 0xffU) == 0U) {
        out->dbg |= 0x06U;
    }
}

void touch_poll(struct touch_state *out)
{
    uint8_t pts[65];
    out->down = 0;
    if (!out->ready) {
        return;
    }
    if (nvt_set_page(NVT_EVENT_BUF_ADDR) != 0 || nvt_spi_read(0x00u, pts, sizeof(pts)) != 0) {
        return;
    }
    for (uint32_t i = 0; i < 10u; i++) {
        uint32_t pos = i * 6u;
        uint8_t id = pts[pos] >> 3;
        uint8_t state = pts[pos] & 0x07u;
        if (id >= 1u && id <= 10u && (state == 1u || state == 2u)) {
            uint32_t x = ((uint32_t)pts[pos + 1u] << 4) | ((uint32_t)pts[pos + 3u] >> 4);
            uint32_t y = ((uint32_t)pts[pos + 2u] << 4) | ((uint32_t)pts[pos + 3u] & 0x0fu);
            if (x < BEGONIA_PANEL_W && y < BEGONIA_PANEL_H) {
                out->down = 1;
                out->x = x;
                out->y = y;
                return;
            }
        }
    }
}
