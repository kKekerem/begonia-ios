/*
 * AArch64 EL1/EL2 identity-mapped 4 KiB-granule MMU setup and cache maintenance.
 */
#ifndef MMU_H
#define MMU_H

#include <stddef.h>
#include <stdint.h>

#define MMU_ATTR_DEV    0U  /* Device-nGnRnE (PXN | UXN) */
#define MMU_ATTR_NC     1U  /* Normal Non-Cacheable / write-combine (PXN | UXN) */
#define MMU_ATTR_WB     2U  /* Normal Write-Back Read/Write-Allocate */

struct mmu_range {
    uint64_t base;
    uint64_t size;
    uint32_t attr;
};

/*
 * Build identity (VA == PA) stage-1 translation tables for [ranges, ranges + count)
 * and enable MMU + D-cache + I-cache at CurrentEL (EL1 or EL2).
 * Returns 0 on success, negative on error.
 */
int mmu_enable_ranges(const struct mmu_range *ranges, size_t count);

/* Clean+invalidate [base, base + size) to PoC using CTR_EL0.DminLine. */
void mmu_flush_dcache_range(uint64_t base, uint64_t size);

/*
 * Clean+invalidate [clean_base, clean_base + clean_len), tear down CurrentEL
 * MMU/D-cache/I-cache/VBAR, invalidate TLB + I-cache, and jump to entry(arg0, 0, 0, 0).
 */
void __attribute__((noreturn))
mmu_off_jump(uint64_t arg0, uint64_t entry, uint64_t clean_base, uint64_t clean_len);

typedef void __attribute__((noreturn)) (*mmu_chain_fn_t)(
    uint64_t fdt, uint64_t dst, uint64_t src, uint64_t len, uint64_t entry, uint64_t tail_zero);

extern char chain_stub[];
extern char chain_stub_end[];

#endif /* MMU_H */
