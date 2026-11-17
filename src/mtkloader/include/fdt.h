/*
 * Minimal read-only Flattened Device Tree (FDT) parser for LK's live DTB.
 * Safe for MMU-off Device-nGnRnE accesses (all token/header reads are 32-bit aligned).
 */
#ifndef FDT_H
#define FDT_H

#include <stdint.h>

#define FDT_MAX_MEM_RANGES 16

struct fdt_range {
    uint64_t base;
    uint64_t size;
};

/* Check whether fdt points to a valid DTB header (magic 0xd00dfeed). */
int fdt_valid(uintptr_t fdt);

/* Read a 32-bit or 64-bit integer property from /chosen. Returns 1 on success. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* Return a pointer into the FDT for a /chosen property's value, or 0 if absent. */
uintptr_t fdt_chosen_prop(uintptr_t fdt, const char *prop, uint32_t *len);

/* Read root model string into buf (NUL-terminated). Returns 1 if found. */
int fdt_root_model(uintptr_t fdt, char *buf, uint32_t cap);

/* Collect /memory reg ranges into out[]. Returns count stored. */
uint32_t fdt_memory_ranges(uintptr_t fdt, struct fdt_range *out, uint32_t max_out);

/* Dump live DTB summary (model, /memory, /reserved-memory, videolfb, bootargs) to console. */
void fdt_dump_board_info(uintptr_t fdt);

#endif /* FDT_H */
