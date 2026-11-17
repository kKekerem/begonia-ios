/*
 * Minimal read-only FDT parser for begonia's live MediaTek LK device tree.
 */
#include "fdt.h"
#include "console.h"

#define FDT_MAGIC       0xd00dfeedU
#define FDT_BEGIN_NODE  1U
#define FDT_END_NODE    2U
#define FDT_PROP        3U
#define FDT_NOP         4U

static uint32_t be32(uintptr_t addr)
{
    return __builtin_bswap32(*(volatile uint32_t *)addr);
}

static uintptr_t align4(uintptr_t addr)
{
    return (addr + 3U) & ~(uintptr_t)3U;
}

static uintptr_t skip_cstr(uintptr_t addr)
{
    while (*(volatile uint8_t *)addr) {
        addr++;
    }
    return addr + 1;
}

static int cstr_eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int cstr_starts(const char *s, const char *prefix)
{
    while (*prefix && *s == *prefix) {
        s++;
        prefix++;
    }
    return *prefix == '\0';
}

static void copy_cstr(char *dst, const char *src, uint32_t cap)
{
    uint32_t i = 0;
    if (cap == 0) {
        return;
    }
    while (i + 1 < cap && src[i]) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

int fdt_valid(uintptr_t fdt)
{
    return fdt != 0 && (fdt & 3U) == 0 && be32(fdt) == FDT_MAGIC;
}

uintptr_t fdt_chosen_prop(uintptr_t fdt, const char *prop, uint32_t *len_out)
{
    if (!fdt_valid(fdt)) {
        return 0;
    }
    uintptr_t p = fdt + be32(fdt + 8U);
    uintptr_t strings = fdt + be32(fdt + 12U);
    int depth = 0;
    int in_chosen = 0;

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            depth++;
            in_chosen = (depth == 2 && cstr_eq(name, "chosen"));
            p = align4(skip_cstr(p));
            break;
        }
        case FDT_END_NODE:
            if (in_chosen) {
                return 0;
            }
            depth--;
            break;
        case FDT_PROP: {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4U);
            uintptr_t val = p + 8U;
            if (in_chosen && cstr_eq((const char *)(strings + nameoff), prop)) {
                if (len_out) {
                    *len_out = len;
                }
                return val;
            }
            p = align4(val + len);
            break;
        }
        case FDT_NOP:
            break;
        default:
            return 0;
        }
    }
}

int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out)
{
    uint32_t len = 0;
    uintptr_t val = fdt_chosen_prop(fdt, prop, &len);
    if (!val) {
        return 0;
    }
    if (len == 4U) {
        *out = be32(val);
        return 1;
    }
    if (len == 8U) {
        *out = ((uint64_t)be32(val) << 32) | be32(val + 4U);
        return 1;
    }
    return 0;
}

int fdt_root_model(uintptr_t fdt, char *buf, uint32_t cap)
{
    if (!fdt_valid(fdt)) {
        return 0;
    }
    uintptr_t p = fdt + be32(fdt + 8U);
    uintptr_t strings = fdt + be32(fdt + 12U);
    int depth = 0;

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE:
            depth++;
            p = align4(skip_cstr(p));
            break;
        case FDT_END_NODE:
            depth--;
            if (depth <= 0) {
                return 0;
            }
            break;
        case FDT_PROP: {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4U);
            uintptr_t val = p + 8U;
            if (depth == 1 && cstr_eq((const char *)(strings + nameoff), "model")) {
                copy_cstr(buf, (const char *)val, cap);
                return 1;
            }
            p = align4(val + len);
            break;
        }
        case FDT_NOP:
            break;
        default:
            return 0;
        }
    }
}

uint32_t fdt_memory_ranges(uintptr_t fdt, struct fdt_range *out, uint32_t max_out)
{
    if (!fdt_valid(fdt)) {
        return 0;
    }
    uintptr_t p = fdt + be32(fdt + 8U);
    uintptr_t strings = fdt + be32(fdt + 12U);
    int depth = 0;
    int in_mem = 0;
    uint32_t count = 0;

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            depth++;
            if (depth == 2) {
                in_mem = cstr_starts(name, "memory");
            }
            p = align4(skip_cstr(p));
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (depth < 2) {
                in_mem = 0;
            }
            break;
        case FDT_PROP: {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4U);
            uintptr_t val = p + 8U;
            if (depth == 2 && in_mem && cstr_eq((const char *)(strings + nameoff), "reg")) {
                for (uint32_t off = 0; off + 16U <= len && count < max_out; off += 16U) {
                    out[count].base = ((uint64_t)be32(val + off) << 32) | be32(val + off + 4U);
                    out[count].size = ((uint64_t)be32(val + off + 8U) << 32) | be32(val + off + 12U);
                    count++;
                }
            }
            p = align4(val + len);
            break;
        }
        case FDT_NOP:
            break;
        default:
            return count;
        }
    }
}

void fdt_dump_board_info(uintptr_t fdt)
{
    if (!fdt_valid(fdt)) {
        con_puts("FDT: invalid or absent\n");
        return;
    }
    uintptr_t p = fdt + be32(fdt + 8U);
    uintptr_t strings = fdt + be32(fdt + 12U);
    int depth = 0;
    int in_mem = 0;
    int in_resv = 0;
    char resv_name[28] = "";

    for (;;) {
        uint32_t tok = be32(p);
        p += 4;
        switch (tok) {
        case FDT_BEGIN_NODE: {
            const char *name = (const char *)p;
            depth++;
            if (depth == 2) {
                in_mem = cstr_starts(name, "memory");
                in_resv = cstr_eq(name, "reserved-memory");
            } else if (depth == 3 && in_resv) {
                copy_cstr(resv_name, name, sizeof(resv_name));
            }
            p = align4(skip_cstr(p));
            break;
        }
        case FDT_END_NODE:
            depth--;
            if (depth < 2) {
                in_mem = 0;
                in_resv = 0;
            }
            break;
        case FDT_PROP: {
            uint32_t len = be32(p);
            uint32_t nameoff = be32(p + 4U);
            uintptr_t val = p + 8U;
            const char *pname = (const char *)(strings + nameoff);

            if (depth == 1 && cstr_eq(pname, "model")) {
                char model[48];
                copy_cstr(model, (const char *)val, sizeof(model));
                con_puts("DTB model: ");
                con_puts(model);
                con_putc('\n');
            } else if ((depth == 2 && in_mem && cstr_eq(pname, "reg")) ||
                       (depth == 3 && in_resv && cstr_eq(pname, "reg"))) {
                const char *tag = in_mem ? "MEM  " : resv_name;
                for (uint32_t off = 0; off + 16U <= len; off += 16U) {
                    uint64_t a = ((uint64_t)be32(val + off) << 32) | be32(val + off + 4U);
                    uint64_t s = ((uint64_t)be32(val + off + 8U) << 32) | be32(val + off + 12U);
                    con_puts("  ");
                    con_puts(tag);
                    con_puts(": ");
                    con_hex(a);
                    con_puts(" + ");
                    con_hex(s);
                    con_putc('\n');
                }
            }
            p = align4(val + len);
            break;
        }
        case FDT_NOP:
            break;
        default:
            return;
        }
    }
}
