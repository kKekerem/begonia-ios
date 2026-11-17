/*
 * SPDX-License-Identifier: MIT
 *
 * libdrm.2.dylib stand-in for Mesa Panfrost + Chlorophyll on begonia (Mali-G76 MC4 / SweetMali).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <IOKit/IOKitLib.h>
#include <errno.h>
#include <fcntl.h>
#include <mach/mach.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "sweet_mali_uapi.h"

#define EXPORT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))
#define DRM_COMMAND_BASE       0x40u
#define DRM_IOCTL_GEM_CLOSE_NR 0x09u
#define DRM_CAP_SYNCOBJ        0x13ull

#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1u << 0)
#define MAP_MAX 4096u

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static io_connect_t g_conn = IO_OBJECT_NULL;
static dev_t g_null_rdev;
static int g_trace = -1;

static struct { void *addr; uint32_t handle; } g_maps[MAP_MAX];
static unsigned g_nmaps;

static char g_wait_err[192];
static unsigned g_wait_err_n;

EXPORT int drmSyncobjWait(int fd, uint32_t *h, unsigned n, int64_t t, unsigned f, uint32_t *first);
HIDDEN void sweet_sobj_mark_pending(uint32_t h, uint64_t seq);

HIDDEN void sweet_drm_record_wait_err(const char *msg)
{
	snprintf(g_wait_err, sizeof(g_wait_err), "#%u %s", ++g_wait_err_n, msg);
}

static int io_errno(kern_return_t kr)
{
	switch (kr) {
	case KERN_SUCCESS: return 0;
	case kIOReturnNoMemory: return ENOMEM;
	case kIOReturnBadArgument: return EINVAL;
	case kIOReturnNoDevice:
	case kIOReturnNotReady: return ENODEV;
	case kIOReturnNotFound: return ENOENT;
	case kIOReturnTimeout:
	case kIOReturnBusy: return EBUSY;
	case kIOReturnUnsupported: return ENOSYS;
	default: return EIO;
	}
}

static int connect_once(void)
{
	pthread_mutex_lock(&g_lock);
	if (g_trace < 0)
		g_trace = getenv("SWEET_DRM_TRACE") != NULL;
	if (g_conn == IO_OBJECT_NULL) {
		struct stat st;
		if (stat("/dev/null", &st) == 0)
			g_null_rdev = st.st_rdev;
		io_service_t svc = IOServiceGetMatchingService(kIOMainPortDefault,
		    IOServiceMatching(SWEET_MALI_SERVICE_NAME));
		if (svc != IO_OBJECT_NULL) {
			if (IOServiceOpen(svc, mach_task_self(), 0, &g_conn) != KERN_SUCCESS)
				g_conn = IO_OBJECT_NULL;
			IOObjectRelease(svc);
		}
	}
	int ok = (g_conn != IO_OBJECT_NULL);
	pthread_mutex_unlock(&g_lock);
	return ok;
}

static size_t sel_size(uint32_t sel)
{
	switch (sel) {
	case SWEET_MALI_SEL_GEM_CLOSE:  return sizeof(struct sweet_mali_gem_close);
	case SWEET_MALI_SEL_SUBMIT:     return sizeof(struct sweet_mali_submit);
	case SWEET_MALI_SEL_WAIT_BO:    return sizeof(struct sweet_mali_wait_bo);
	case SWEET_MALI_SEL_CREATE_BO:  return sizeof(struct sweet_mali_create_bo);
	case SWEET_MALI_SEL_MMAP_BO:    return sizeof(struct sweet_mali_mmap_bo);
	case SWEET_MALI_SEL_GET_PARAM:  return sizeof(struct sweet_mali_get_param);
	case SWEET_MALI_SEL_GET_BO_OFF: return sizeof(struct sweet_mali_get_bo_offset);
	case SWEET_MALI_SEL_MADVISE:    return sizeof(struct sweet_mali_madvise);
	case SWEET_MALI_SEL_INFO:       return sizeof(struct sweet_mali_info);
	case SWEET_MALI_SEL_SURFACE_BO: return sizeof(struct sweet_mali_surface_bo);
	case SWEET_MALI_SEL_BO_SYNC:    return sizeof(struct sweet_mali_bo_sync);
	case SWEET_MALI_SEL_WAIT_SEQ:   return sizeof(struct sweet_mali_wait_seq);
	default: return 0;
	}
}

static uint64_t now_mono_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}

static int64_t abs_to_rel_ns(int64_t abs_ns)
{
	if (abs_ns <= 0) return 0;
	if (abs_ns == INT64_MAX) return 5000000000ll;
	uint64_t now = now_mono_ns();
	return (uint64_t)abs_ns > now ? (int64_t)((uint64_t)abs_ns - now) : 0;
}

static int sweet_call(uint32_t sel, void *data, size_t size)
{
	uint8_t in[256], out[256];
	size_t want = sel_size(sel), outsz = sizeof(out);
	if (!connect_once()) return -ENODEV;
	if (want == 0 || want > sizeof(in)) return -ENOSYS;
	if (sel == SWEET_MALI_SEL_MADVISE) {
		((struct sweet_mali_madvise *)data)->retained = 1u;
		return 0;
	}
	memset(in, 0, sizeof(in));
	if (data != NULL && size > 0)
		memcpy(in, data, size < want ? size : want);
	kern_return_t kr = IOConnectCallStructMethod(g_conn, sel,
	    size ? in : NULL, size ? want : 0, out, &outsz);
	if (g_trace && kr != KERN_SUCCESS && sel != SWEET_MALI_SEL_WAIT_BO && sel != SWEET_MALI_SEL_WAIT_SEQ)
		fprintf(stderr, "sweet_drm_mali: sel %#x -> kr %#x\n", sel, kr);
	if (kr == KERN_SUCCESS && data != NULL && outsz > 0)
		memcpy(data, out, outsz < size ? outsz : size);
	return -io_errno(kr);
}

HIDDEN int sweet_drm_connect(void) { return connect_once(); }
HIDDEN int64_t sweet_drm_abs_to_rel_ns(int64_t abs_ns) { return abs_to_rel_ns(abs_ns); }
HIDDEN int sweet_drm_call(uint32_t sel, void *data, size_t size) { return sweet_call(sel, data, size); }

EXPORT void sweet_drm_take_wait_error(char *buf, size_t size)
{
	if (size == 0) return;
	snprintf(buf, size, "%s", g_wait_err);
	g_wait_err[0] = 0;
}

EXPORT drmVersionPtr drmGetVersion(int fd)
{
	(void)fd;
	drmVersionPtr v = calloc(1, sizeof(*v));
	if (!v) return NULL;
	v->version_major = 1;
	v->version_minor = 3;
	v->version_patchlevel = 0;
	v->name = strdup("panfrost");
	v->name_len = 8;
	v->date = strdup("20261117");
	v->date_len = 8;
	v->desc = strdup("Begonia SweetMali Bifrost");
	v->desc_len = (int)strlen(v->desc);
	return v;
}

EXPORT void drmFreeVersion(drmVersionPtr v)
{
	if (!v) return;
	free(v->name); free(v->date); free(v->desc); free(v);
}

EXPORT int drmOpenWithType(const char *name, const char *busid, int type)
{
	(void)busid; (void)type;
	if (name && strcmp(name, "panfrost") != 0) {
		errno = ENODEV;
		return -1;
	}
	if (!connect_once()) {
		errno = ENODEV;
		return -1;
	}
	return open("/dev/null", O_RDWR | O_CLOEXEC);
}

EXPORT int drmOpen(const char *name, const char *busid) { return drmOpenWithType(name, busid, 0); }
EXPORT int drmClose(int fd) { return close(fd); }

EXPORT int drmGetCap(int fd, uint64_t cap, uint64_t *value)
{
	(void)fd;
	*value = (cap == DRM_CAP_SYNCOBJ) ? 1ull : 0ull;
	return 0;
}

EXPORT int drmGetMagic(int fd, uint32_t *magic) { (void)fd; *magic = 0x534d414cu; return 0; }
EXPORT int drmAuthMagic(int fd, uint32_t magic) { (void)fd; (void)magic; return 0; }

EXPORT int drmPrimeHandleToFD(int fd, uint32_t h, uint32_t f, int *pfd)
{ (void)fd; (void)h; (void)f; (void)pfd; errno = ENOSYS; return -1; }
EXPORT int drmPrimeFDToHandle(int fd, int pfd, uint32_t *h)
{ (void)fd; (void)pfd; (void)h; errno = ENOSYS; return -1; }

static int submit_panfrost(struct sweet_mali_submit *req)
{
	if (req->in_sync_count && req->in_syncs) {
		const uint32_t *ins = (const uint32_t *)(uintptr_t)req->in_syncs;
		int r = drmSyncobjWait(-1, (uint32_t *)ins, req->in_sync_count, INT64_MAX, DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL, NULL);
		if (r < 0) return r;
	}
	struct sweet_mali_submit k = *req;
	int r = sweet_call(SWEET_MALI_SEL_SUBMIT, &k, sizeof(k));
	if (r != 0) return r;
	sweet_sobj_mark_pending(req->out_sync, k.jc);
	return 0;
}

EXPORT int drmIoctl(int fd, unsigned long request, void *arg)
{
	(void)fd;
	uint32_t nr = (uint32_t)(request & 0xffu);
	int r = -ENOSYS;
	if (nr == DRM_IOCTL_GEM_CLOSE_NR) {
		r = sweet_call(SWEET_MALI_SEL_GEM_CLOSE, arg, sizeof(struct sweet_mali_gem_close));
	} else if (nr == SWEET_MALI_SEL_SUBMIT) {
		r = arg ? submit_panfrost((struct sweet_mali_submit *)arg) : -EINVAL;
	} else if (nr == SWEET_MALI_SEL_WAIT_BO) {
		struct sweet_mali_wait_bo w;
		if (!arg) { errno = EINVAL; return -1; }
		w = *(struct sweet_mali_wait_bo *)arg;
		w.timeout_ns = abs_to_rel_ns(w.timeout_ns);
		r = sweet_call(SWEET_MALI_SEL_WAIT_BO, &w, sizeof(w));
		if (r == -EBUSY) r = -ETIMEDOUT;
	} else if (nr >= 0x42u && nr <= 0x48u) {
		r = sweet_call(nr, arg, sel_size(nr));
	}
	if (r < 0) { errno = -r; return -1; }
	return 0;
}

EXPORT int drmCommandWriteRead(int fd, unsigned long idx, void *data, unsigned long size)
{ (void)size; return drmIoctl(fd, DRM_COMMAND_BASE + idx, data); }
EXPORT int drmCommandWrite(int fd, unsigned long idx, void *data, unsigned long size)
{ return drmCommandWriteRead(fd, idx, data, size); }
EXPORT int drmCommandRead(int fd, unsigned long idx, void *data, unsigned long size)
{ return drmCommandWriteRead(fd, idx, data, size); }
EXPORT int drmCommandNone(int fd, unsigned long idx)
{ (void)fd; (void)idx; return 0; }

EXPORT int drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct sweet_mali_gem_close c = { .handle = handle };
	(void)fd;
	int r = sweet_call(SWEET_MALI_SEL_GEM_CLOSE, &c, sizeof(c));
	if (r < 0) { errno = -r; return -1; }
	return 0;
}

static int is_ours(int fd)
{
	struct stat st;
	if (g_conn == IO_OBJECT_NULL || fd < 0 || fstat(fd, &st) != 0) return 0;
	return S_ISCHR(st.st_mode) && st.st_rdev == g_null_rdev;
}

EXPORT void *sweet_drm_mmap(int fd, size_t len, uint64_t offset)
{
	(void)fd;
	if (!connect_once() || len == 0) { errno = EINVAL; return MAP_FAILED; }
	uint32_t handle = (uint32_t)(offset >> 12);
	if (handle == 0) handle = (uint32_t)offset;
	mach_vm_address_t addr = 0;
	mach_vm_size_t sz = (mach_vm_size_t)len;
	if (IOConnectMapMemory64(g_conn, handle, mach_task_self(), &addr, &sz,
	    kIOMapAnywhere | kIOMapWriteCombineCache) != KERN_SUCCESS) {
		errno = EINVAL; return MAP_FAILED;
	}
	pthread_mutex_lock(&g_lock);
	if (g_nmaps < MAP_MAX) { g_maps[g_nmaps].addr = (void *)(uintptr_t)addr; g_maps[g_nmaps].handle = handle; g_nmaps++; }
	pthread_mutex_unlock(&g_lock);
	return (void *)(uintptr_t)addr;
}

EXPORT int sweet_drm_munmap(void *addr, size_t len)
{
	int found = 0; uint32_t handle = 0;
	pthread_mutex_lock(&g_lock);
	for (unsigned i = 0; i < g_nmaps; i++) {
		if (g_maps[i].addr == addr) { handle = g_maps[i].handle; g_maps[i] = g_maps[--g_nmaps]; found = 1; break; }
	}
	pthread_mutex_unlock(&g_lock);
	if (found)
		return IOConnectUnmapMemory64(g_conn, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)addr) == KERN_SUCCESS ? 0 : -1;
	return munmap(addr, len);
}

static void *sweet_mmap(void *addr, size_t len, int prot, int flags, int fd, off_t off)
{
	if (!addr && is_ours(fd))
		return sweet_drm_mmap(fd, len, (uint64_t)off);
	return mmap(addr, len, prot, flags, fd, off);
}

struct interpose_tuple { const void *replacement, *replacee; };
__attribute__((used)) static const struct interpose_tuple interpose_mmap
    __attribute__((section("__DATA,__interpose"))) = { (const void *)sweet_mmap, (const void *)mmap };
__attribute__((used)) static const struct interpose_tuple interpose_munmap
    __attribute__((section("__DATA,__interpose"))) = { (const void *)sweet_drm_munmap, (const void *)munmap };

EXPORT int sweet_drm_surface_bo(int fd, struct rwa_sbo_compat *io)
{
	(void)fd;
	if (!io) return -EINVAL;
	struct sweet_mali_surface_bo k;
	memset(&k, 0, sizeof(k));
	k.surface_id = io->surface_id;
	k.handle = io->handle;
	k.flags = SWEET_MALI_SBO_IN_SYNC;
	int r = sweet_call(SWEET_MALI_SEL_SURFACE_BO, &k, sizeof(k));
	if (r != 0) return r;
	io->handle = k.handle;
	io->size = k.size;
	io->iova = k.gpu_va;
	io->offset = k.offset;
	io->bytes_per_row = k.bytes_per_row;
	io->width = k.width;
	io->height = k.height;
	io->pixel_format = k.pixel_format;
	io->planes = k.planes;
	return 0;
}

EXPORT int sweet_drm_surface_bo_ext(int fd, struct sweet_gpu_surface_bo_compat *io)
{
	int r = sweet_drm_surface_bo(fd, (struct rwa_sbo_compat *)io);
	if (r == 0) {
		io->flags = 0; io->tiling = 0; io->ubwc = 0; io->reserved = 0;
	}
	return r;
}

EXPORT int sweet_drm_gem_close(int fd, uint32_t handle) { return drmCloseBufferHandle(fd, handle); }

EXPORT int sweet_drm_bo_sync(int fd, uint32_t handle, uint64_t offset, uint64_t size)
{
	(void)fd; (void)offset; (void)size;
	struct sweet_mali_bo_sync a = { .handle = handle, .flags = 0 };
	return sweet_call(SWEET_MALI_SEL_BO_SYNC, &a, sizeof(a));
}

EXPORT int sweet_drm_info(int fd, struct bg_gpu_info_compat *out)
{
	(void)fd;
	if (!out) return -EINVAL;
	struct sweet_mali_info k;
	memset(&k, 0, sizeof(k));
	int r = sweet_call(SWEET_MALI_SEL_INFO, &k, sizeof(k));
	if (r != 0) return r;
	memset(out, 0, sizeof(*out));
	out->abi_version = k.version;
	out->flags = k.flags;
	out->gpu_id = k.gpu_id;
	out->va_start = 0x10000000ull;
	out->va_size = 0xe0000000ull;
	out->last_fence = (uint32_t)k.last_seq;
	out->abi_minor = k.abi_minor;
	return 0;
}

#define DRM_NODE_PRIMARY 0
#define DRM_NODE_RENDER  2
#define DRM_NODE_MAX     3
#define DRM_BUS_PLATFORM 2
typedef struct { char fullname[512]; } drmPlatformBusInfo;
typedef struct { char **compatible; } drmPlatformDeviceInfo;
typedef struct _drmDevice {
	char **nodes; int available_nodes; int bustype;
	union { drmPlatformBusInfo *platform; void *any; } businfo;
	union { drmPlatformDeviceInfo *platform; void *any; } deviceinfo;
} drmDevice, *drmDevicePtr;

EXPORT int drmGetDevices2(uint32_t flags, drmDevicePtr *devices, int max_devices)
{
	(void)flags;
	if (!connect_once()) return 0;
	if (!devices) return 1;
	if (max_devices < 1) return 0;
	struct blob { drmDevice dev; char *nodes[DRM_NODE_MAX]; drmPlatformBusInfo bus; drmPlatformDeviceInfo info; char *compat[2]; } *b = calloc(1, sizeof(*b));
	if (!b) return -ENOMEM;
	b->nodes[0] = ""; b->nodes[1] = ""; b->nodes[DRM_NODE_RENDER] = "/dev/null";
	b->dev.nodes = b->nodes; b->dev.available_nodes = 1 << DRM_NODE_RENDER; b->dev.bustype = DRM_BUS_PLATFORM;
	snprintf(b->bus.fullname, sizeof(b->bus.fullname), "13040000.mali");
	b->dev.businfo.platform = &b->bus;
	b->compat[0] = "arm,mali-bifrost";
	b->info.compatible = b->compat;
	b->dev.deviceinfo.platform = &b->info;
	devices[0] = &b->dev;
	return 1;
}
EXPORT int drmGetDevice2(int fd, uint32_t f, drmDevicePtr *d) { (void)fd; return drmGetDevices2(f, d, 1) > 0 ? 0 : -ENODEV; }
EXPORT void drmFreeDevice(drmDevicePtr *d) { if (d && *d) { free(*d); *d = NULL; } }
EXPORT void drmFreeDevices(drmDevicePtr *d, int n) { for (int i = 0; i < n; i++) free(d[i]); }
EXPORT int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b) { return a == b || (a && b && a->bustype == b->bustype); }
