/*
 * XNU-11215 (iOS 18) SWEETXNU payload & Apple boot_args handoff definitions.
 */
#ifndef XNU_BOOT_H
#define XNU_BOOT_H

#include <stdint.h>

#define BOOT_LINE_LENGTH    608
#define XNU_PAGE_16K        0x4000ULL
#define PAYLOAD_MAGIC_STR   "SWEETXNU"

struct sweet_payload_seg_v1 {
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t file_off;
    uint64_t file_size;
};

struct sweet_payload_seg_v2 {
    uint64_t vmaddr;
    uint64_t vmsize;
    uint64_t file_off;
    uint64_t file_size;
    uint64_t raw_size; /* Non-zero when stored as raw DEFLATE */
};

struct sweet_payload {
    char magic[8];
    uint32_t version;
    uint32_t nsegs;
    uint64_t virt_base;
    uint64_t entry_va;
    uint64_t adt_off;
    uint64_t adt_size;
    uint64_t kernel_end;
    uint64_t phys_base;
    uint64_t mem_size;
    char cmdline[BOOT_LINE_LENGTH];
    uint8_t segs[];
};

struct xnu_boot_video {
    uint64_t v_baseAddr;
    uint64_t v_display;
    uint64_t v_rowBytes;
    uint64_t v_width;
    uint64_t v_height;
    uint64_t v_depth;
};

/* Matches XNU pexpert/pexpert/arm64/boot.h (Revision=2, Version=2, 736 bytes). */
struct xnu_boot_args {
    uint16_t Revision;
    uint16_t Version;
    uint64_t virtBase;
    uint64_t physBase;
    uint64_t memSize;
    uint64_t topOfKernelData;
    struct xnu_boot_video Video;
    uint32_t machineType;
    uint64_t deviceTreeP;
    uint32_t deviceTreeLength;
    char CommandLine[BOOT_LINE_LENGTH];
    uint64_t bootFlags;
    uint64_t memSizeActual;
};

_Static_assert(__builtin_offsetof(struct sweet_payload, segs) == 0x2a8, "payload header layout");
_Static_assert(sizeof(struct sweet_payload_seg_v2) == 40, "payload segment layout");
_Static_assert(__builtin_offsetof(struct xnu_boot_args, Video) == 40, "boot_args Video offset");
_Static_assert(__builtin_offsetof(struct xnu_boot_args, CommandLine) == 108, "boot_args CommandLine offset");
_Static_assert(sizeof(struct xnu_boot_args) == 736, "boot_args size");

/* Returns 1 if [start, end) begins with the SWEETXNU magic header. */
int xnu_payload_present(uintptr_t start, uintptr_t end);

/* Validate payload structure & RAM bounds. Returns NULL on success, error string otherwise. */
const char *xnu_payload_check(uintptr_t start, uintptr_t end);

/* Load segments, ADT, and boot_args, then jump to XNU (never returns on success). */
const char *xnu_payload_boot(uintptr_t start, uintptr_t end);

/* Extra boot-args of a one-shot request from Android (boot_menu.c), appended to the payload's. */
extern char g_xnu_extra_args[];

#endif /* XNU_BOOT_H */
