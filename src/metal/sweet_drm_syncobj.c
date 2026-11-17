/*
 * SPDX-License-Identifier: MIT
 * DRM binary syncobj emulation over SweetMali 64-bit fence sequences.
 */
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include "sweet_mali_uapi.h"

#define EXPORT __attribute__((visibility("default")))
#define HIDDEN __attribute__((visibility("hidden")))
#define SOBJ_MAX   8192u
#define SOBJ_FREE  0u
#define SOBJ_UNSIG 1u
#define SOBJ_SIG   2u
#define SOBJ_PEND  3u

#define DRM_SYNCOBJ_CREATE_SIGNALED     (1u << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1u << 0)

HIDDEN int sweet_drm_connect(void);
HIDDEN int sweet_drm_call(uint32_t sel, void *data, size_t size);
HIDDEN int64_t sweet_drm_abs_to_rel_ns(int64_t abs_ns);
HIDDEN void sweet_drm_record_wait_err(const char *msg);

static pthread_mutex_t g_sobj_lock = PTHREAD_MUTEX_INITIALIZER;
static struct { uint32_t state; uint64_t seq; } g_sobj[SOBJ_MAX];
static uint32_t g_sobj_next = 1u;

static int sobj_valid(uint32_t h) { return h > 0u && h < SOBJ_MAX && g_sobj[h].state != SOBJ_FREE; }
static int sobj_err(int e) { errno = e; return -e; }

HIDDEN void sweet_sobj_mark_pending(uint32_t h, uint64_t seq)
{
	if (!h) return;
	pthread_mutex_lock(&g_sobj_lock);
	if (sobj_valid(h)) {
		g_sobj[h].state = seq ? SOBJ_PEND : SOBJ_SIG;
		g_sobj[h].seq = seq;
	}
	pthread_mutex_unlock(&g_sobj_lock);
}

EXPORT int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	(void)fd;
	if (!sweet_drm_connect()) return sobj_err(ENODEV);
	pthread_mutex_lock(&g_sobj_lock);
	uint32_t h = 0u;
	for (uint32_t n = 0; n < SOBJ_MAX - 1u; n++) {
		uint32_t cand = ((g_sobj_next - 1u + n) % (SOBJ_MAX - 1u)) + 1u;
		if (g_sobj[cand].state == SOBJ_FREE) {
			h = cand; g_sobj_next = (cand % (SOBJ_MAX - 1u)) + 1u; break;
		}
	}
	if (!h) { pthread_mutex_unlock(&g_sobj_lock); return sobj_err(ENOMEM); }
	g_sobj[h].state = (flags & DRM_SYNCOBJ_CREATE_SIGNALED) ? SOBJ_SIG : SOBJ_UNSIG;
	g_sobj[h].seq = 0;
	pthread_mutex_unlock(&g_sobj_lock);
	*handle = h;
	return 0;
}

EXPORT int drmSyncobjDestroy(int fd, uint32_t h)
{
	(void)fd;
	pthread_mutex_lock(&g_sobj_lock);
	if (!sobj_valid(h)) { pthread_mutex_unlock(&g_sobj_lock); return sobj_err(ENOENT); }
	g_sobj[h].state = SOBJ_FREE; g_sobj[h].seq = 0;
	pthread_mutex_unlock(&g_sobj_lock);
	return 0;
}

EXPORT int drmSyncobjReset(int fd, const uint32_t *h, uint32_t n)
{
	(void)fd;
	pthread_mutex_lock(&g_sobj_lock);
	for (uint32_t i = 0; i < n; i++)
		if (sobj_valid(h[i])) { g_sobj[h[i]].state = SOBJ_UNSIG; g_sobj[h[i]].seq = 0; }
	pthread_mutex_unlock(&g_sobj_lock);
	return 0;
}

EXPORT int drmSyncobjSignal(int fd, const uint32_t *h, uint32_t n)
{
	(void)fd;
	pthread_mutex_lock(&g_sobj_lock);
	for (uint32_t i = 0; i < n; i++)
		if (sobj_valid(h[i])) { g_sobj[h[i]].state = SOBJ_SIG; g_sobj[h[i]].seq = 0; }
	pthread_mutex_unlock(&g_sobj_lock);
	return 0;
}

EXPORT int drmSyncobjWait(int fd, uint32_t *h, unsigned n, int64_t timeout_ns, unsigned flags, uint32_t *first)
{
	(void)fd;
	if (!n || !h) return sobj_err(EINVAL);
	int wait_all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	for (unsigned i = 0; i < n; i++) {
		uint32_t st; uint64_t seq;
		pthread_mutex_lock(&g_sobj_lock);
		if (!sobj_valid(h[i])) { pthread_mutex_unlock(&g_sobj_lock); return sobj_err(ENOENT); }
		st = g_sobj[h[i]].state; seq = g_sobj[h[i]].seq;
		pthread_mutex_unlock(&g_sobj_lock);
		if (st == SOBJ_UNSIG) {
			if (!wait_all && i + 1u < n) continue;
			return sobj_err(ETIME);
		}
		if (st == SOBJ_PEND) {
			struct sweet_mali_wait_seq w = { .seq = seq, .timeout_ns = sweet_drm_abs_to_rel_ns(timeout_ns) };
			int r = sweet_drm_call(SWEET_MALI_SEL_WAIT_SEQ, &w, sizeof(w));
			if (r != 0) {
				char msg[128];
				snprintf(msg, sizeof(msg), "WAIT_SEQ seq=%llu err=%d", (unsigned long long)seq, r);
				sweet_drm_record_wait_err(msg);
				return sobj_err(ETIME);
			}
		}
		if (first) *first = i;
		if (!wait_all) return 0;
	}
	return 0;
}

EXPORT int drmSyncobjTimelineSignal(int fd, const uint32_t *h, uint64_t *p, uint32_t n) { (void)fd;(void)h;(void)p;(void)n; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjTimelineWait(int fd, uint32_t *h, uint64_t *p, unsigned n, int64_t t, unsigned f, uint32_t *first) { (void)fd;(void)h;(void)p;(void)n;(void)t;(void)f;(void)first; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjQuery(int fd, uint32_t *h, uint64_t *p, uint32_t n) { (void)fd;(void)h;(void)p;(void)n; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjQuery2(int fd, uint32_t *h, uint64_t *p, uint32_t n, uint32_t f) { (void)fd;(void)h;(void)p;(void)n;(void)f; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjTransfer(int fd, uint32_t d, uint64_t dp, uint32_t s, uint64_t sp, uint32_t f) { (void)fd;(void)d;(void)dp;(void)s;(void)sp;(void)f; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjHandleToFD(int fd, uint32_t h, int *ofd) { (void)fd;(void)h;(void)ofd; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjFDToHandle(int fd, int ofd, uint32_t *h) { (void)fd;(void)ofd;(void)h; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjExportSyncFile(int fd, uint32_t h, int *sfd) { (void)fd;(void)h;(void)sfd; return sobj_err(ENOSYS); }
EXPORT int drmSyncobjImportSyncFile(int fd, uint32_t h, int sfd) { (void)fd;(void)h;(void)sfd; return sobj_err(ENOSYS); }
