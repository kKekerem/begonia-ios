/*
 * SPDX-License-Identifier: MIT
 * Userspace definitions matching XNU SweetMaliABI.h + Linux Panfrost DRM uapi.
 */
#ifndef _SWEET_MALI_UAPI_H_
#define _SWEET_MALI_UAPI_H_

#include <stdint.h>

#define SWEET_MALI_SERVICE_NAME     "SweetMali"
#define SWEET_MALI_ABI_VERSION      1

enum sweet_mali_selector {
	SWEET_MALI_SEL_GEM_CLOSE    = 0x09,
	SWEET_MALI_SEL_SUBMIT       = 0x40,
	SWEET_MALI_SEL_WAIT_BO      = 0x41,
	SWEET_MALI_SEL_CREATE_BO    = 0x42,
	SWEET_MALI_SEL_MMAP_BO      = 0x43,
	SWEET_MALI_SEL_GET_PARAM    = 0x44,
	SWEET_MALI_SEL_GET_BO_OFF   = 0x45,
	SWEET_MALI_SEL_MADVISE      = 0x48,
	SWEET_MALI_SEL_INFO         = 0x70,
	SWEET_MALI_SEL_SURFACE_BO   = 0x71,
	SWEET_MALI_SEL_BO_SYNC      = 0x72,
	SWEET_MALI_SEL_WAIT_SEQ     = 0x7d,
};

#define SWEET_MALI_INFO_READY       0x1u
#define SWEET_MALI_INFO_LOST        0x2u

#define SWEET_MALI_SBO_IN_SYNC      0x1u

struct sweet_mali_gem_close {
	uint32_t handle, pad;
};

struct sweet_mali_submit {
	uint64_t jc;
	uint64_t in_syncs;
	uint32_t in_sync_count;
	uint32_t out_sync;
	uint64_t bo_handles;
	uint32_t bo_handle_count;
	uint32_t requirements;
};

struct sweet_mali_wait_bo {
	uint32_t handle, pad;
	int64_t timeout_ns;
};

struct sweet_mali_create_bo {
	uint32_t size, flags, handle, pad;
	uint64_t offset;
};

struct sweet_mali_mmap_bo {
	uint32_t handle, flags;
	uint64_t offset;
};

struct sweet_mali_get_param {
	uint32_t param, pad;
	uint64_t value;
};

struct sweet_mali_get_bo_offset {
	uint32_t handle, pad;
	uint64_t offset;
};

struct sweet_mali_madvise {
	uint32_t handle, madv, retained;
};

struct sweet_mali_info {
	uint32_t version, abi_minor, flags, gpu_id;
	uint32_t va_bits, faults;
	uint64_t last_seq;
};

struct sweet_mali_surface_bo {
	uint32_t surface_id, handle;
	uint64_t size, gpu_va;
	uint32_t offset, bytes_per_row;
	uint32_t width, height, pixel_format, planes;
	uint32_t flags, pad;
};

struct sweet_mali_bo_sync {
	uint32_t handle, flags;
};

struct sweet_mali_wait_seq {
	uint64_t seq;
	int64_t timeout_ns;
};

/* Binary compatibility struct expected by Chlorophyll's bgmtl_gpu_state() */
struct bg_gpu_info_compat {
	uint32_t abi_version, flags, gpu_id, chip_id;
	uint64_t va_start, va_size;
	uint32_t last_fence, abi_minor;
};

/* Exact 48-byte prefix matching Chlorophyll air_metal.c struct rwa_sbo */
struct rwa_sbo_compat {
	uint32_t surface_id, handle;
	uint64_t size, iova;
	uint32_t offset, bytes_per_row, width, height, pixel_format, planes;
};

struct sweet_gpu_surface_bo_compat {
	uint32_t surface_id, handle;
	uint64_t size, iova;
	uint32_t offset, bytes_per_row, width, height, pixel_format, planes;
	uint32_t flags, tiling, ubwc, reserved;
};

#endif /* _SWEET_MALI_UAPI_H_ */
