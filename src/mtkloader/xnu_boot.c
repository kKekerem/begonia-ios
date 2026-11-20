/*
 * SWEETXNU payload validator, loader, and XNU-11215 boot_args handoff.
 */
#include "xnu_boot.h"
#include "console.h"
#include "inflate.h"
#include "mmu.h"

extern char _start[];
extern char __loader_end[];
extern void __attribute__((noreturn)) jump_to_kernel(uint64_t entry_pa, uint64_t boot_args_pa);

char g_xnu_extra_args[BOOT_REQUEST_ARGS_MAX + 1U];

static void copy64(uintptr_t dst, uintptr_t src, uint64_t n)
{
    volatile uint64_t *d = (volatile uint64_t *)dst;
    const volatile uint64_t *s = (const volatile uint64_t *)src;
    uint64_t words = n / 8ULL;
    for (uint64_t i = 0; i < words; i++) {
        d[i] = s[i];
    }
    for (uint64_t i = words * 8ULL; i < n; i++) {
        ((volatile uint8_t *)dst)[i] = ((const volatile uint8_t *)src)[i];
    }
}

static void zero64(uintptr_t dst, uint64_t n)
{
    volatile uint64_t *d = (volatile uint64_t *)dst;
    uint64_t words = n / 8ULL;
    for (uint64_t i = 0; i < words; i++) {
        d[i] = 0;
    }
    for (uint64_t i = words * 8ULL; i < n; i++) {
        ((volatile uint8_t *)dst)[i] = 0;
    }
}

int xnu_payload_present(uintptr_t start, uintptr_t end)
{
    if (end <= start || (end - start) < sizeof(struct sweet_payload)) {
        return 0;
    }
    const char *m = (const char *)start;
    const char want[] = PAYLOAD_MAGIC_STR;
    for (int i = 0; i < 8; i++) {
        if (m[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

static void read_seg(const struct sweet_payload *p, uint32_t idx, struct sweet_payload_seg_v2 *out)
{
    if (p->version == 2U) {
        const struct sweet_payload_seg_v2 *s2 =
            (const struct sweet_payload_seg_v2 *)(p->segs + (uint64_t)idx * sizeof(*s2));
        *out = *s2;
    } else {
        const struct sweet_payload_seg_v1 *s1 =
            (const struct sweet_payload_seg_v1 *)(p->segs + (uint64_t)idx * sizeof(*s1));
        out->vmaddr = s1->vmaddr;
        out->vmsize = s1->vmsize;
        out->file_off = s1->file_off;
        out->file_size = s1->file_size;
        out->raw_size = 0;
    }
}

const char *xnu_payload_check(uintptr_t start, uintptr_t end)
{
    const struct sweet_payload *p = (const struct sweet_payload *)start;
    uint64_t size = end - start;

    if (!xnu_payload_present(start, end)) {
        return "missing SWEETXNU magic";
    }
    if ((p->version != 1U && p->version != 2U) || (p->adt_off & 7ULL) || (start & 7ULL)) {
        return "invalid payload header or alignment";
    }
    uint64_t stride = (p->version == 2U) ? sizeof(struct sweet_payload_seg_v2) : sizeof(struct sweet_payload_seg_v1);
    if (p->nsegs == 0U || p->nsegs > 64U || sizeof(struct sweet_payload) + (uint64_t)p->nsegs * stride > size) {
        return "invalid segment table";
    }
    uint64_t ram_lo = p->phys_base;
    uint64_t ram_hi = p->phys_base + p->mem_size;
    if (ram_hi <= ram_lo) {
        return "invalid RAM window";
    }
    if (end > ram_lo && start < ram_hi) {
        return "payload overlaps XNU physical RAM";
    }
    if ((uintptr_t)__loader_end > ram_lo && (uintptr_t)_start < ram_hi) {
        return "loader overlaps XNU physical RAM";
    }
    if (p->adt_off > size || p->adt_size > size - p->adt_off || p->adt_size == 0U) {
        return "ADT outside payload bounds";
    }
    uint64_t pa_of_va = p->phys_base - p->virt_base;
    uint64_t kend_pa = p->kernel_end + pa_of_va;
    if (kend_pa < ram_lo || kend_pa >= ram_hi || (kend_pa & (XNU_PAGE_16K - 1ULL))) {
        return "invalid kernel_end alignment or range";
    }
    uint64_t top_pa = ((kend_pa + p->adt_size + XNU_PAGE_16K - 1ULL) & ~(XNU_PAGE_16K - 1ULL)) + XNU_PAGE_16K;
    if (top_pa > ram_hi) {
        return "kernel + ADT + boot_args exceed RAM window";
    }
    for (uint32_t i = 0; i < p->nsegs; i++) {
        struct sweet_payload_seg_v2 s;
        read_seg(p, i, &s);
        if (s.file_off > size || s.file_size > size - s.file_off) {
            return "segment file range out of bounds";
        }
        uint64_t raw = (p->version == 2U && s.raw_size) ? s.raw_size : s.file_size;
        if (raw > s.vmsize) {
            return "segment raw size exceeds vmsize";
        }
        uint64_t dst = s.vmaddr + pa_of_va;
        if (dst < ram_lo || dst + s.vmsize > kend_pa || dst + s.vmsize < dst) {
            return "segment destination outside kernel window";
        }
    }
    if (p->entry_va + pa_of_va < ram_lo || p->entry_va + pa_of_va >= kend_pa) {
        return "entry point outside kernel window";
    }
    return 0;
}

static int str_eq_n(const char *a, const char *b, uint32_t max_a)
{
    for (uint32_t i = 0; i < max_a; i++) {
        if (a[i] != b[i]) {
            return 0;
        }
        if (a[i] == '\0') {
            return 1;
        }
    }
    return b[max_a] == '\0';
}

/*
 * Walk the serialized ADT in-place to patch /sweet-fbcon with the live LK framebuffer
 * address/size/stride so XNU's early framebuffer console works even if LK relocates splash RAM.
 */
static const uint8_t *adt_patch_node(uint8_t *cur, const uint8_t *end, int depth,
                                     const struct fb_info *fb)
{
    if (depth > 8 || cur + 8 > end) {
        return 0;
    }
    uint32_t nprops = *(const uint32_t *)cur;
    uint32_t nchildren = *(const uint32_t *)(cur + 4);
    cur += 8;

    int is_fbcon = 0;
    uint8_t *reg_ptr = 0;
    uint32_t reg_len = 0;
    uint8_t *stride_ptr = 0;
    uint32_t stride_len = 0;

    for (uint32_t i = 0; i < nprops; i++) {
        if (cur + 36 > end) {
            return 0;
        }
        const char *pname = (const char *)cur;
        uint32_t raw_sz = *(const uint32_t *)(cur + 32);
        uint32_t plen = raw_sz & 0x7fffffffU;
        uint32_t padded = (plen + 3U) & ~3U;
        uint8_t *pval = cur + 36;
        if (pval + padded > end) {
            return 0;
        }
        if (str_eq_n(pname, "name", 32U) && str_eq_n((const char *)pval, "sweet-fbcon", plen)) {
            is_fbcon = 1;
        } else if (str_eq_n(pname, "reg", 32U)) {
            reg_ptr = pval;
            reg_len = plen;
        } else if (str_eq_n(pname, "stride", 32U)) {
            stride_ptr = pval;
            stride_len = plen;
        }
        cur = pval + padded;
    }

    if (is_fbcon) {
        if (reg_ptr && reg_len == 16U) {
            *(volatile uint64_t *)reg_ptr = fb->base;
            *(volatile uint64_t *)(reg_ptr + 8) = (uint64_t)fb->size;
        }
        if (stride_ptr && stride_len == 4U) {
            *(volatile uint32_t *)stride_ptr = fb->pitch;
        }
    }

    for (uint32_t i = 0; i < nchildren; i++) {
        cur = (uint8_t *)adt_patch_node(cur, end, depth + 1, fb);
        if (!cur) {
            return 0;
        }
    }
    return cur;
}

const char *xnu_payload_boot(uintptr_t start, uintptr_t end)
{
    const char *err = xnu_payload_check(start, end);
    if (err) {
        return err;
    }

    const struct sweet_payload *p = (const struct sweet_payload *)start;
    const struct fb_info *fb = console_fb();
    uint64_t pa_of_va = p->phys_base - p->virt_base;
    uintptr_t adt_pa = (uintptr_t)(p->kernel_end + pa_of_va);
    uintptr_t ba_pa = (adt_pa + p->adt_size + XNU_PAGE_16K - 1ULL) & ~(XNU_PAGE_16K - 1ULL);
    uintptr_t top_pa = ba_pa + XNU_PAGE_16K;

    con_puts("XNU payload v");
    con_dec(p->version);
    con_puts(" segs=");
    con_dec(p->nsegs);
    con_puts(" phys=");
    con_hex(p->phys_base);
    con_puts(" size=");
    con_hex(p->mem_size);
    con_putc('\n');
    if (((p->phys_base ^ p->virt_base) & 0x1ffffffULL) != 0ULL) {
        con_puts("WARN: phys/virt 32M offset mismatch!\n");
    }

    struct mmu_range mmu_ranges[6] = {
        { 0x0C000000ULL, 0x0A000000ULL, MMU_ATTR_DEV },
        { (uint64_t)(uintptr_t)_start, (uint64_t)(__loader_end - _start), MMU_ATTR_WB },
        { (uint64_t)start, (uint64_t)(end - start), MMU_ATTR_WB },
        { p->phys_base, (uint64_t)(top_pa - p->phys_base), MMU_ATTR_WB },
        { PLOG_ZONE_PA, PLOG_ZONE_SIZE, MMU_ATTR_NC },
        { fb ? fb->base : 0ULL, fb ? (uint64_t)fb->size : 0ULL, MMU_ATTR_NC }
    };
    size_t nranges = (fb && fb->base && fb->size) ? 6U : 5U;
    int mmu_rc = mmu_enable_ranges(mmu_ranges, nranges);
    if (mmu_rc != 0) {
        con_puts("WARN: XNU MMU setup failed rc=");
        con_hex((uint64_t)(int64_t)mmu_rc);
        con_putc('\n');
    }

    for (uint32_t i = 0; i < p->nsegs; i++) {
        struct sweet_payload_seg_v2 s;
        read_seg(p, i, &s);
        uintptr_t dst = (uintptr_t)(s.vmaddr + pa_of_va);
        uint64_t raw = s.file_size;

        if (p->version == 2U && s.raw_size) {
            uint64_t cap = s.vmsize;
            int rc = inflate_raw((const uint8_t *)(start + s.file_off), s.file_size,
                                 (volatile uint8_t *)dst, &cap);
            if (rc != 0 || cap != s.raw_size) {
                con_puts("inflate failed seg ");
                con_dec(i);
                con_puts("\n");
                return "segment inflate failed";
            }
            raw = cap;
        } else {
            copy64(dst, start + s.file_off, s.file_size);
        }

        if (s.vmsize > raw) {
            zero64(dst + raw, s.vmsize - raw);
        }
    }

    copy64(adt_pa, start + p->adt_off, p->adt_size);
    (void)adt_patch_node((uint8_t *)adt_pa, (const uint8_t *)(adt_pa + p->adt_size), 0, fb);

    zero64(ba_pa, XNU_PAGE_16K);

    volatile struct xnu_boot_args *ba = (volatile struct xnu_boot_args *)ba_pa;
    ba->Revision = 2;
    ba->Version = 2;
    ba->virtBase = p->virt_base;
    ba->physBase = p->phys_base;
    ba->memSize = p->mem_size;
    ba->topOfKernelData = top_pa;
    ba->Video.v_baseAddr = fb->base;
    ba->Video.v_display = 1;
    ba->Video.v_rowBytes = fb->pitch;
    ba->Video.v_width = fb->width;
    ba->Video.v_height = fb->height;
    ba->Video.v_depth = 32;
    ba->deviceTreeP = adt_pa - pa_of_va; /* XNU maps this virtually via kva_to_pa/physBase */
    ba->deviceTreeLength = (uint32_t)p->adt_size;
    for (int i = 0; i < BOOT_LINE_LENGTH; i++) {
        ba->CommandLine[i] = p->cmdline[i];
    }
    ba->CommandLine[BOOT_LINE_LENGTH - 1] = '\0';
    if (g_xnu_extra_args[0]) {
        /* one-shot request from Android: append its boot-args (XNU takes the first of a repeated arg) */
        int len = 0;
        while (len < BOOT_LINE_LENGTH - 1 && ba->CommandLine[len]) {
            len++;
        }
        if (len > 0 && len < BOOT_LINE_LENGTH - 2) {
            ba->CommandLine[len++] = ' ';
        }
        for (int i = 0; g_xnu_extra_args[i] && len < BOOT_LINE_LENGTH - 1; i++) {
            ba->CommandLine[len++] = g_xnu_extra_args[i];
        }
        ba->CommandLine[len] = '\0';
        con_puts("boot-args + request: ");
        con_puts(g_xnu_extra_args);
        con_putc('\n');
    } else {
        con_puts("boot-args: no request args\n");
    }
    ba->memSizeActual = p->mem_size;

    uint64_t entry_pa = p->entry_va + pa_of_va;
    con_puts("Jumping to XNU entry=");
    con_hex(entry_pa);
    con_puts(" boot_args=");
    con_hex(ba_pa);
    con_putc('\n');

    console_strip_ovl_alpha();
    if (mmu_rc == 0) {
        mmu_off_jump(ba_pa, entry_pa, p->phys_base, (uint64_t)(top_pa - p->phys_base));
    }
    jump_to_kernel(entry_pa, ba_pa);
}
