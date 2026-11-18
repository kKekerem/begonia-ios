/*
 * Minimal identity-mapped (VA == PA) AArch64 EL1/EL2 MMU driver (4 KiB granule).
 *
 * Page tables live in a dedicated scratch window above BEGONIA_CHAIN_STUB_PA
 * so mtkloader's .bss stays small well under BEGONIA_CHAIN_OFFSET (256 KiB).
 * Only explicitly requested ranges are mapped; unmentioned DRAM (including
 * ATF/TEE/SCP TZASC carve-outs) stays unmapped to prevent speculative aborts.
 */
#include "mmu.h"

#define PAGE_SHIFT      12U
#define PAGE_SIZE_4K    (1ULL << PAGE_SHIFT)
#define BLOCK_SHIFT     21U
#define BLOCK_SIZE_2M   (1ULL << BLOCK_SHIFT)
#define L1_SHIFT        30U
#define ENTRIES_PER_PT  512U
#define MAX_VA_BYTES    (1ULL << 39) /* T0SZ = 25 -> 512 GiB VA space */

#define MMU_PT_POOL_PA  0x46100000ULL
#define MMU_PT_MAX_PGS  64U /* 256 KiB total page table scratch */

#define DESC_VALID      (1ULL << 0)
#define DESC_BLOCK      (1ULL << 0)
#define DESC_TABLE      (3ULL << 0)
#define DESC_PAGE       (3ULL << 0)
#define DESC_AF         (1ULL << 10)
#define DESC_SH_OSH     (2ULL << 8)
#define DESC_SH_ISH     (3ULL << 8)
#define DESC_PXN        (1ULL << 53)
#define DESC_UXN        (1ULL << 54)
#define DESC_ADDR_MASK  0x0000FFFFFFFFF000ULL

#define SCTLR_M         (1ULL << 0)
#define SCTLR_C         (1ULL << 2)
#define SCTLR_I         (1ULL << 12)
#define SCTLR_WXN       (1ULL << 19)

#define MAIR_CONFIG     0x0000000000FF4400ULL /* [0]=DEV, [1]=NC, [2]=WB-WA */

static uint32_t g_pt_used;

static uint64_t *pt_alloc_page(void)
{
    if (g_pt_used >= MMU_PT_MAX_PGS) {
        return NULL;
    }
    uint64_t pa = MMU_PT_POOL_PA + ((uint64_t)g_pt_used << PAGE_SHIFT);
    g_pt_used++;
    volatile uint64_t *p = (volatile uint64_t *)(uintptr_t)pa;
    for (uint32_t i = 0; i < ENTRIES_PER_PT; i++) {
        p[i] = 0;
    }
    return (uint64_t *)(uintptr_t)pa;
}

extern void dcache_clean_all(void);

static uint64_t leaf_attrs(uint32_t attr, uint64_t el)
{
    /* Non-VHE EL2 uses bit 54 as XN (bit 53 RES0); EL1 uses bit 53=PXN, bit 54=UXN. */
    uint64_t xn_all = (el == 2ULL) ? DESC_UXN : (DESC_PXN | DESC_UXN);
    uint64_t wb_xn  = (el == 2ULL) ? 0ULL : DESC_UXN;
    if (attr == MMU_ATTR_WB) {
        return DESC_AF | DESC_SH_ISH | (2ULL << 2) | wb_xn;
    }
    if (attr == MMU_ATTR_NC) {
        return DESC_AF | DESC_SH_OSH | (1ULL << 2) | xn_all;
    }
    return DESC_AF | DESC_SH_OSH | (0ULL << 2) | xn_all;
}

static uint64_t *ensure_l2(uint64_t *l1, uint64_t va)
{
    uint32_t idx = (uint32_t)((va >> L1_SHIFT) & (ENTRIES_PER_PT - 1U));
    if (l1[idx] & DESC_VALID) {
        return (uint64_t *)(uintptr_t)(l1[idx] & DESC_ADDR_MASK);
    }
    uint64_t *l2 = pt_alloc_page();
    if (!l2) {
        return NULL;
    }
    l1[idx] = ((uint64_t)(uintptr_t)l2 & DESC_ADDR_MASK) | DESC_TABLE;
    return l2;
}

static uint64_t *ensure_l3(uint64_t *l2, uint32_t l2_idx)
{
    uint64_t entry = l2[l2_idx];
    if ((entry & 3ULL) == DESC_TABLE) {
        return (uint64_t *)(uintptr_t)(entry & DESC_ADDR_MASK);
    }
    uint64_t *l3 = pt_alloc_page();
    if (!l3) {
        return NULL;
    }
    if ((entry & 3ULL) == DESC_BLOCK) {
        /* Split an existing 2 MiB block into 512 x 4 KiB pages preserving attrs. */
        uint64_t base = entry & ~((BLOCK_SIZE_2M - 1ULL)) & DESC_ADDR_MASK;
        uint64_t attrs = (entry & ~(DESC_ADDR_MASK | 3ULL)) | DESC_PAGE;
        for (uint32_t i = 0; i < ENTRIES_PER_PT; i++) {
            l3[i] = (base + ((uint64_t)i << PAGE_SHIFT)) | attrs;
        }
    }
    l2[l2_idx] = ((uint64_t)(uintptr_t)l3 & DESC_ADDR_MASK) | DESC_TABLE;
    return l3;
}

static int map_range(uint64_t *l1, const struct mmu_range *r, uint64_t el)
{
    if (r->size == 0) {
        return 0;
    }
    uint64_t base = r->base & ~(PAGE_SIZE_4K - 1ULL);
    uint64_t raw_end = r->base + r->size;
    if (raw_end < r->base) {
        return -1;
    }
    uint64_t end = (raw_end + PAGE_SIZE_4K - 1ULL) & ~(PAGE_SIZE_4K - 1ULL);
    if (end > MAX_VA_BYTES) {
        return -1;
    }
    uint64_t attrs = leaf_attrs(r->attr, el);

    for (uint64_t cur = base; cur < end;) {
        uint64_t *l2 = ensure_l2(l1, cur);
        if (!l2) {
            return -2;
        }
        uint32_t l2_idx = (uint32_t)((cur >> BLOCK_SHIFT) & (ENTRIES_PER_PT - 1U));
        if ((cur & (BLOCK_SIZE_2M - 1ULL)) == 0 && cur + BLOCK_SIZE_2M <= end &&
            (l2[l2_idx] & 3ULL) != DESC_TABLE) {
            l2[l2_idx] = cur | DESC_BLOCK | attrs;
            cur += BLOCK_SIZE_2M;
            continue;
        }
        uint64_t *l3 = ensure_l3(l2, l2_idx);
        if (!l3) {
            return -2;
        }
        uint32_t l3_idx = (uint32_t)((cur >> PAGE_SHIFT) & (ENTRIES_PER_PT - 1U));
        l3[l3_idx] = cur | DESC_PAGE | attrs;
        cur += PAGE_SIZE_4K;
    }
    return 0;
}

void mmu_flush_dcache_range(uint64_t base, uint64_t size)
{
    if (size == 0) {
        return;
    }
    uint64_t ctr;
    __asm__ volatile("mrs %0, ctr_el0" : "=r"(ctr));
    uint64_t line = 4ULL << ((ctr >> 16) & 0xFULL);
    uint64_t p = base & ~(line - 1ULL);
    uint64_t end = base + size;

    __asm__ volatile("dsb sy" ::: "memory");
    for (; p < end; p += line) {
        __asm__ volatile("dc civac, %0" :: "r"(p) : "memory");
    }
    __asm__ volatile("dsb sy" ::: "memory");
}

static uint64_t read_current_el(void)
{
    uint64_t el;
    __asm__ volatile("mrs %0, CurrentEL" : "=r"(el));
    return (el >> 2) & 3ULL;
}

static uint64_t read_pa_range(void)
{
    uint64_t mmfr0;
    __asm__ volatile("mrs %0, id_aa64mmfr0_el1" : "=r"(mmfr0));
    uint64_t pa = mmfr0 & 0xFULL;
    return (pa > 5ULL) ? 5ULL : pa;
}

static void disable_active_mmu(uint64_t el)
{
    uint64_t sctlr;
    if (el == 2ULL) {
        __asm__ volatile("mrs %0, sctlr_el2" : "=r"(sctlr));
        if (!(sctlr & (SCTLR_M | SCTLR_C | SCTLR_I))) {
            return;
        }
        dcache_clean_all();
        sctlr &= ~(SCTLR_M | SCTLR_C | SCTLR_I);
        __asm__ volatile("msr sctlr_el2, %0; isb; tlbi alle2; ic iallu; dsb sy; isb"
                         :: "r"(sctlr) : "memory");
        return;
    }
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (!(sctlr & (SCTLR_M | SCTLR_C | SCTLR_I))) {
        return;
    }
    dcache_clean_all();
    sctlr &= ~(SCTLR_M | SCTLR_C | SCTLR_I);
    __asm__ volatile("msr sctlr_el1, %0; isb; tlbi vmalle1; ic iallu; dsb sy; isb"
                     :: "r"(sctlr) : "memory");
}

int mmu_enable_ranges(const struct mmu_range *ranges, size_t count)
{
    uint64_t el = read_current_el();
    if (el != 1ULL && el != 2ULL) {
        return -3;
    }
    disable_active_mmu(el);

    g_pt_used = 0;
    uint64_t *l1 = pt_alloc_page();
    if (!l1) {
        return -2;
    }

    /* Keep hardware page-table pool WB-mapped before enabling TTBR0 walks. */
    struct mmu_range pt_range = {
        MMU_PT_POOL_PA,
        (uint64_t)MMU_PT_MAX_PGS << PAGE_SHIFT,
        MMU_ATTR_WB
    };
    if (map_range(l1, &pt_range, el) != 0) {
        return -1;
    }
    for (size_t i = 0; i < count; i++) {
        int rc = map_range(l1, &ranges[i], el);
        if (rc != 0) {
            return rc;
        }
    }

    uint64_t pa_range = read_pa_range();
    uint64_t tcr_common = 25ULL | (1ULL << 8) | (1ULL << 10) | (3ULL << 12);
    uint64_t ttbr = (uint64_t)(uintptr_t)l1;
    uint64_t sctlr_on = SCTLR_M | SCTLR_C | SCTLR_I;

    __asm__ volatile("dsb sy" ::: "memory");
    if (el == 2ULL) {
        uint64_t tcr = tcr_common | (1ULL << 23) | (1ULL << 31) | (pa_range << 16);
        uint64_t sctlr;
        __asm__ volatile(
            "msr mair_el2, %1\n"
            "msr tcr_el2, %2\n"
            "msr ttbr0_el2, %3\n"
            "dsb sy\n"
            "tlbi alle2\n"
            "ic iallu\n"
            "dsb sy\n"
            "isb\n"
            "mrs %0, sctlr_el2\n"
            "bic %0, %0, %4\n"
            "orr %0, %0, %5\n"
            "msr sctlr_el2, %0\n"
            "isb\n"
            : "=&r"(sctlr)
            : "r"(MAIR_CONFIG), "r"(tcr), "r"(ttbr), "r"(SCTLR_WXN), "r"(sctlr_on)
            : "memory");
        return 0;
    }

    uint64_t tcr = tcr_common | (1ULL << 23) | (pa_range << 32);
    uint64_t sctlr;
    __asm__ volatile(
        "msr mair_el1, %1\n"
        "msr tcr_el1, %2\n"
        "msr ttbr0_el1, %3\n"
        "msr ttbr1_el1, xzr\n"
        "dsb sy\n"
        "tlbi vmalle1\n"
        "ic iallu\n"
        "dsb sy\n"
        "isb\n"
        "mrs %0, sctlr_el1\n"
        "bic %0, %0, %4\n"
        "orr %0, %0, %5\n"
        "msr sctlr_el1, %0\n"
        "isb\n"
        : "=&r"(sctlr)
        : "r"(MAIR_CONFIG), "r"(tcr), "r"(ttbr), "r"(SCTLR_WXN), "r"(sctlr_on)
        : "memory");
    return 0;
}
