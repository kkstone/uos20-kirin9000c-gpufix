/*
 * gbm-hw-shim.so -- enable Chrome 157 hardware compositing on UOS 20 /
 * Kirin 9000c / Maleoon 910 (HUAWEI libGFX_hisi GBM/EGL).
 *
 * ROOT CAUSE (verified with real stderr):
 *   Chrome's GbmPixmapWayland::InitializeBuffer() calls
 *   gbm_bo_create(w,h,format, flags=Scanout|Rendering|Texturing = 37) for
 *   the real window pixmap. HUAWEI GBM rejects those flags for every RGB
 *   format -> gbm_pixmap_wayland.cc:104 "Cannot create bo" -> GPU process
 *   crash loop -> Chrome silently degrades to software compositing.
 *
 *   HUAWEI GBM CAN create the same buffer via
 *   gbm_bo_create_with_modifiers(..., modifier=0x0900000000000002), which
 *   is the only modifier KWin advertises for XR24/AR24 (verified with
 *   wl-dmabuf-query.c and gbm-fd-test.c).
 *
 * ABI ISSUES BRIDGED:
 *   1. Chromium resolves gbm_* via dlsym(handle,...) after dlopen, so plain
 *      LD_PRELOAD symbol interposition is bypassed. We hook dlsym().
 *   2. gbm_bo_get_fd_for_plane is a WEAK import HUAWEI doesn't export ->
 *      drmPrimeHandleToFD fallback -> EBADF flood. We supply it via
 *      HUAWEI's gbm_bo_get_fd().
 *   3. gbm_bo_get_modifier on HUAWEI bo returns 0; we report KWin's modifier
 *      so Chrome's gbm_bo_import carries a modifier KWin accepts.
 *   4. gbm_import_fd_modifier_data: Chrome writes modifier at byte 56,
 *      HUAWEI reads at byte 64. We write to BOTH in a local copy.
 *   5. JITTER ROOT CAUSE: HUAWEI reports stride=0 / stride_for_plane=32 for
 *      HISI-modifier bos (correct is width*4). Wrong stride -> every row
 *      sampled at the wrong offset -> moving video/WebGL jitters. We repair
 *      implausible strides to width*4. See repair_stride().
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

typedef struct gbm_bo gbm_bo;
typedef struct gbm_device gbm_device;
union gbm_bo_handle { void *ptr; uint32_t u32; };

typedef int (*fn_plane_fd)(gbm_bo *, int);
typedef int (*fn_gbm_bo_get_fd)(gbm_bo *);
typedef uint64_t (*fn_gbm_bo_get_modifier)(gbm_bo *);
typedef gbm_device *(*fn_gbm_bo_get_device)(gbm_bo *);
typedef int (*fn_gbm_device_get_fd)(gbm_device *);
typedef union gbm_bo_handle (*fn_gbm_bo_get_handle_for_plane)(gbm_bo *, int);
typedef int (*fn_drm_prime)(int, uint32_t, uint32_t, int *);
typedef gbm_bo *(*fn_gbm_bo_create)(gbm_device *, uint32_t, uint32_t,
                                    uint32_t, uint32_t);
typedef gbm_bo *(*fn_gbm_bo_create_with_modifiers)(gbm_device *, uint32_t,
        uint32_t, uint32_t, const uint64_t *, uint32_t);
typedef gbm_bo *(*fn_gbm_bo_import)(gbm_device *, uint32_t, void *, uint32_t);
typedef uint32_t (*fn_gbm_bo_get_stride)(gbm_bo *);
typedef uint32_t (*fn_gbm_bo_get_stride_for_plane)(gbm_bo *, int);
typedef uint32_t (*fn_gbm_bo_get_width)(gbm_bo *);

/* ---- VAAPI version spoof (merged: only ONE dlsym hook allowed) ------- */
typedef int VAStatus;
typedef void *VADisplay;
#define VA_STATUS_SUCCESS 0
static VAStatus (*captured_real_vaInitialize)(VADisplay, int *, int *) = NULL;
VAStatus vaInitialize(VADisplay dpy, int *major, int *minor);

#define KWIN_MOD_DEFAULT 0x0900000000000002ULL
#define GBM_BO_IMPORT_FD_MODIFIER 0x5504

/* Resolve the REAL dlsym via a versioned symbol (proven in vaapi spoof).
 * __libc_dlsym(RTLD_NEXT, ...) segfaults on this glibc. */
static void *(*real_dlsym_fn(void))(void *, const char *) {
    static void *(*r)(void *, const char *) = NULL;
    if (!r) r = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    return r;
}
static void *real_sym(const char *name) {
    void *(*rd)(void *, const char *) = real_dlsym_fn();
    return rd ? rd(RTLD_NEXT, name) : NULL;
}

__attribute__((constructor)) static void gbm_hw_loaded(void) {
    /* Chrome's zygote scrubs these; restore VAAPI env in every child. */
    if (!getenv("LIBVA_DRIVER_NAME")) setenv("LIBVA_DRIVER_NAME", "vacore", 1);
    if (!getenv("LIBVA_DRIVERS_PATH")) setenv("LIBVA_DRIVERS_PATH", "/usr/lib", 1);
    setenv("LIBVA_DRI3_DISABLE", "1", 1);
}

static int env_flag(const char *name, int def_on) {
    const char *v = getenv(name);
    if (!v) return def_on;
    if (strcmp(v, "0") == 0) return 0;
    if (strcmp(v, "1") == 0) return 1;
    return def_on;
}

static uint64_t target_modifier(void) {
    static uint64_t m = 0;
    if (!m) {
        const char *v = getenv("HISI_GBM_MODIFIER");
        m = v ? strtoull(v, NULL, 0) : KWIN_MOD_DEFAULT;
        if (!m) m = KWIN_MOD_DEFAULT;
    }
    return m;
}

/* ---------- fd -> bo cache (HUAWEI gbm_bo_import is a no-op) ------------
 * HUAWEI libGFX's gbm_bo_import() returns NULL for EVERY type (verified with
 * gbmimport.c on this machine). Chrome creates a bo via gbm_bo_create*, gets
 * its dma-buf fd, then calls gbm_bo_import to turn that fd back into a bo for
 * Wayland. We remember every bo we create together with its dma-buf identity,
 * and on import return the ORIGINAL bo instead of calling the broken HUAWEI
 * function.
 *
 * IMPORTANT: HUAWEI gbm_bo_get_fd() returns a NEW fd on EVERY call (verified
 * with diagnostics/gbm-fd-inode.c: 4 calls -> fd 5,6,7,8 but the SAME st_ino).
 * Keying the cache by fd number therefore NEVER matches the fd Chrome passes
 * to gbm_bo_import -> every import fell back to a freshly-created empty bo,
 * which shows up as frame jitter/tearing. Key by (st_dev, st_ino) instead.
 */
#define CACHE_MAX 256
static struct { dev_t dev; ino_t ino; gbm_bo *bo; uint32_t w, h, fmt; } bo_cache[CACHE_MAX];
static int bo_cache_n;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void cache_bo(gbm_bo *bo, uint32_t w, uint32_t h, uint32_t fmt) {
    if (!bo) return;
    static fn_gbm_bo_get_fd gfd;
    if (!gfd) gfd = (fn_gbm_bo_get_fd)real_sym("gbm_bo_get_fd");
    int fd = gfd ? gfd(bo) : -1;
    if (fd < 0) return;
    struct stat st; memset(&st, 0, sizeof(st));
    int fr = fstat(fd, &st);
    close(fd);                 /* it's a dup; don't leak one per create */
    if (fr != 0) return;
    pthread_mutex_lock(&cache_lock);
    /* refresh an existing inode entry (same bo re-created) else append */
    int slot = -1;
    for (int i = 0; i < bo_cache_n; i++)
        if (bo_cache[i].dev == st.st_dev && bo_cache[i].ino == st.st_ino) { slot = i; break; }
    if (slot < 0 && bo_cache_n < CACHE_MAX) { slot = bo_cache_n++; }
    if (slot >= 0) {
        bo_cache[slot].dev = st.st_dev;
        bo_cache[slot].ino = st.st_ino;
        bo_cache[slot].bo  = bo;
        bo_cache[slot].w = w; bo_cache[slot].h = h; bo_cache[slot].fmt = fmt;
    }
    pthread_mutex_unlock(&cache_lock);
}
/* Look up the original bo for an imported dma-buf fd.
 *
 * Keying by (st_dev, st_ino) alone is NOT sufficient on HUAWEI libGFX:
 * gbm_bo_get_fd() hands out a NEW dup every call, and the kernel recycles
 * dma-buf inodes aggressively when Chrome streams many scroll tiles. If the
 * inode is reused, an import of a fresh bo can match a STALE cache entry and
 * get back an old bo -> a tile keeps showing scrolled-away content until a
 * mouse hover forces Chrome to repaint it.
 *
 * FIX: additionally require the imported geometry (w/h/fmt) to match the
 * cached entry, and prefer the newest matching slot (highest index). Callers
 * that don't know the geometry pass w=h=0, which keeps the old behaviour.
 */
static gbm_bo *lookup_bo(int fd, uint32_t w, uint32_t h, uint32_t fmt) {
    if (fd < 0) return NULL;
    struct stat st; memset(&st, 0, sizeof(st));
    if (fstat(fd, &st) != 0) return NULL;
    gbm_bo *found = NULL;
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < bo_cache_n; i++) {
        if (bo_cache[i].dev != st.st_dev || bo_cache[i].ino != st.st_ino)
            continue;
        /* geometry guard: reject stale inode reuse with a different buffer */
        if (w && h && (bo_cache[i].w != w || bo_cache[i].h != h ||
                       bo_cache[i].fmt != fmt))
            continue;
        found = bo_cache[i].bo;  /* keep scanning: newest match wins */
    }
    pthread_mutex_unlock(&cache_lock);
    return found;
}

/* ---------- wrapper: gbm_bo_get_fd_for_plane --------------------------- */
int gbm_bo_get_fd_for_plane(gbm_bo *bo, int plane) {
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_PLANE_FD", 1)) {
        fn_plane_fd legacy = (fn_plane_fd)real_sym("gbm_bo_get_fd_for_plane");
        return legacy ? legacy(bo, plane) : -1;
    }
    static fn_gbm_bo_get_fd gfd;
    static fn_gbm_bo_get_device gd;
    static fn_gbm_device_get_fd gdfd;
    static fn_gbm_bo_get_handle_for_plane gh;
    static fn_drm_prime gp;
    if (!gfd) {
        gfd  = (fn_gbm_bo_get_fd)real_sym("gbm_bo_get_fd");
        gd   = (fn_gbm_bo_get_device)real_sym("gbm_bo_get_device");
        gdfd = (fn_gbm_device_get_fd)real_sym("gbm_device_get_fd");
        gh   = (fn_gbm_bo_get_handle_for_plane)real_sym("gbm_bo_get_handle_for_plane");
        gp   = (fn_drm_prime)real_sym("drmPrimeHandleToFD");
    }
    if (plane == 0 && gfd) {
        int fd = gfd(bo);
        if (fd >= 0) return fd;
    }
    if (gd && gdfd && gh && gp) {
        gbm_device *dev = gd(bo);
        int devfd = dev ? gdfd(dev) : -1;
        uint32_t h = gh(bo, plane).u32;
        int fd = -1;
        int r = gp(devfd, h, O_CLOEXEC | O_RDWR, &fd);
        if (r == 0 && fd >= 0) return fd;
    }
    return -1;
}

/* ---------- wrapper: gbm_bo_create_with_modifiers --------------------- */
gbm_bo *gbm_bo_create_with_modifiers(gbm_device *dev, uint32_t w, uint32_t h,
        uint32_t format, const uint64_t *modifiers, uint32_t count) {
    static fn_gbm_bo_create_with_modifiers real;
    if (!real) real = (fn_gbm_bo_create_with_modifiers)real_sym("gbm_bo_create_with_modifiers");
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_CREATE_FIX", 1))
        return real ? real(dev, w, h, format, modifiers, count) : NULL;

    uint64_t fixed[8];
    uint32_t n = count > 8 ? 8 : count;
    int changed = 0;
    for (uint32_t i = 0; i < n; i++) {
        fixed[i] = modifiers[i];
        if (fixed[i] == 0 || fixed[i] == 0x00FFFFFFFFFFFFFFULL) {
            fixed[i] = target_modifier();
            changed = 1;
        }
    }
    if (n == 0) { fixed[0] = target_modifier(); n = 1; changed = 1; }
    gbm_bo *bo = real ? real(dev, w, h, format, changed ? fixed : modifiers, n) : NULL;
    cache_bo(bo, w, h, format);
    return bo;
}

/* ---------- wrapper: gbm_bo_create (THE real-window fix) -------------- */
gbm_bo *gbm_bo_create(gbm_device *dev, uint32_t width, uint32_t height,
                      uint32_t format, uint32_t flags) {
    static fn_gbm_bo_create real;
    static fn_gbm_bo_create_with_modifiers real_mod;
    if (!real) real = (fn_gbm_bo_create)real_sym("gbm_bo_create");
    if (!real_mod) real_mod = (fn_gbm_bo_create_with_modifiers)real_sym("gbm_bo_create_with_modifiers");
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_CREATE_FIX", 1))
        return real ? real(dev, width, height, format, flags) : NULL;

    /* Real window pixmaps are >=64x64 and RGB formats. HUAWEI rejects
     * Scanout|Rendering|Texturing (flags=37) outright; route through the
     * modifiers entry point with KWin's only accepted modifier. */
    uint64_t m = target_modifier();
    if (real_mod && width >= 64 && height >= 64) {
        gbm_bo *bo = real_mod(dev, width, height, format, &m, 1);
        if (bo) { cache_bo(bo, width, height, format); return bo; }
    }
    gbm_bo *bo = real ? real(dev, width, height, format, flags) : NULL;
    cache_bo(bo, width, height, format);
    return bo;
}

/* ---------- wrapper: gbm_bo_get_modifier ------------------------------ */
uint64_t gbm_bo_get_modifier(gbm_bo *bo) {
    static fn_gbm_bo_get_modifier real;
    if (!real) real = (fn_gbm_bo_get_modifier)real_sym("gbm_bo_get_modifier");
    uint64_t m = real ? real(bo) : 0;
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_REPORT_MODIFIER", 1))
        return m;
    if (m == 0 || m == 0x00FFFFFFFFFFFFFFULL) {
        uint64_t want = target_modifier();
        return want;
    }
    return m;
}

/* ---------- wrapper: stride repair (THE jitter root cause) ------------
 * HUAWEI libGFX returns a BROKEN stride for bos created with the HISI
 * modifier (the only modifier KWin accepts):
 *   LINEAR bo: gbm_bo_get_stride()=1024, stride_for_plane(0)=1024
 *   HISI   bo: gbm_bo_get_stride()=0,    stride_for_plane(0)=32
 * for a 256x144 AR24 buffer, where 1024 (=256*4) is correct.
 * Chrome uses these values to lay out pixels and to build the dmabuf params
 * it hands to KWin. A wrong stride means every row is sampled at the wrong
 * offset -> static frames look fine but moving video/WebGL visibly jitters.
 * Repair any implausible stride (< width) to width*4 (Chrome window buffers
 * are 32bpp: AR24/XR24/AB24). HISI_GBM_STRIDE_FIX=0 disables for A/B.
 */
static uint32_t repair_stride(gbm_bo *bo, uint32_t got, const char *which) {
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_STRIDE_FIX", 1))
        return got;
    if (!bo || got >= 4) return got;           /* plausible already */
    static fn_gbm_bo_get_width gw;
    if (!gw) gw = (fn_gbm_bo_get_width)real_sym("gbm_bo_get_width");
    uint32_t w = gw ? gw(bo) : 0;
    if (!w) return got;
    uint32_t fixed = w * 4;
    return fixed;
}

uint32_t gbm_bo_get_stride(gbm_bo *bo) {
    static fn_gbm_bo_get_stride real;
    if (!real) real = (fn_gbm_bo_get_stride)real_sym("gbm_bo_get_stride");
    uint32_t s = real ? real(bo) : 0;
    return repair_stride(bo, s, "stride");
}

uint32_t gbm_bo_get_stride_for_plane(gbm_bo *bo, int plane) {
    static fn_gbm_bo_get_stride_for_plane real;
    if (!real) real = (fn_gbm_bo_get_stride_for_plane)real_sym("gbm_bo_get_stride_for_plane");
    uint32_t s = real ? real(bo, plane) : 0;
    return repair_stride(bo, s, "stride_plane");
}

/* ---------- wrapper: gbm_bo_import ------------------------------------ */
gbm_bo *gbm_bo_import(gbm_device *dev, uint32_t type, void *buffer,
                      uint32_t flags) {
    static fn_gbm_bo_import real;
    if (!real) real = (fn_gbm_bo_import)real_sym("gbm_bo_import");
    if (type != GBM_BO_IMPORT_FD_MODIFIER || !buffer ||
        !env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_IMPORT_FIX", 1))
        return real ? real(dev, type, buffer, flags) : NULL;

    /* The incoming fd is at byte offset 16 of gbm_import_fd_modifier_data;
     * width/height/format at 0/4/8. Pass geometry so lookup_bo can reject a
     * stale inode that now belongs to a differently-sized scroll tile. */
    int in_fd = *(int *)((unsigned char *)buffer + 16);
    uint32_t in_w = *(uint32_t *)((unsigned char *)buffer + 0);
    uint32_t in_h = *(uint32_t *)((unsigned char *)buffer + 4);
    uint32_t in_fmt = *(uint32_t *)((unsigned char *)buffer + 8);
    gbm_bo *cached = lookup_bo(in_fd, in_w, in_h, in_fmt);
    if (cached) {
        return cached;
    }

    unsigned char local[80];
    memset(local, 0, sizeof(local));
    memcpy(local, buffer, 64);
    uint64_t chrome_mod = *(uint64_t *)((unsigned char *)buffer + 56);
    if (chrome_mod == 0 || chrome_mod == 0x00FFFFFFFFFFFFFFULL)
        chrome_mod = target_modifier();
    *(uint64_t *)(local + 56) = chrome_mod;
    *(uint64_t *)(local + 64) = chrome_mod;
    gbm_bo *bo = real ? real(dev, type, local, flags) : NULL;
    /* Last resort: a fresh bo of the same geometry (import is broken here). */
    if (!bo) {
        uint32_t w = *(uint32_t *)((unsigned char *)buffer + 0);
        uint32_t h = *(uint32_t *)((unsigned char *)buffer + 4);
        uint32_t fmt = *(uint32_t *)((unsigned char *)buffer + 8);
        static fn_gbm_bo_create_with_modifiers cwm;
        if (!cwm) cwm = (fn_gbm_bo_create_with_modifiers)real_sym("gbm_bo_create_with_modifiers");
        if (cwm && w && h) {
            uint64_t m = chrome_mod;
            bo = cwm(dev, w, h, fmt, &m, 1);
        }
    }
    return bo;
}

/* ---------- dlsym hook: hand Chrome our GBM wrappers ------------------ */
void *dlsym(void *handle, const char *symbol) {
    void *(*rd)(void *, const char *) = real_dlsym_fn();
    if (!rd) return NULL;
    if (!env_flag("HISI_GBM_SHIM", 1)) return rd(handle, symbol);
    if (handle && symbol) {
        if (strcmp(symbol, "vaInitialize") == 0 && handle != RTLD_NEXT &&
            env_flag("HISI_VAAPI_SPOOF", 1)) {
            void *real = rd(handle, symbol);
            if (real)
                captured_real_vaInitialize =
                    (VAStatus (*)(VADisplay, int *, int *))real;
            return (void *)&vaInitialize;
        }
        if (strcmp(symbol, "gbm_bo_get_fd_for_plane") == 0)
            return (void *)&gbm_bo_get_fd_for_plane;
        if (strcmp(symbol, "gbm_bo_create_with_modifiers") == 0)
            return (void *)&gbm_bo_create_with_modifiers;
        if (strcmp(symbol, "gbm_bo_create") == 0)
            return (void *)&gbm_bo_create;
        if (strcmp(symbol, "gbm_bo_get_modifier") == 0)
            return (void *)&gbm_bo_get_modifier;
        if (strcmp(symbol, "gbm_bo_get_stride") == 0)
            return (void *)&gbm_bo_get_stride;
        if (strcmp(symbol, "gbm_bo_get_stride_for_plane") == 0)
            return (void *)&gbm_bo_get_stride_for_plane;
        if (strcmp(symbol, "gbm_bo_import") == 0)
            return (void *)&gbm_bo_import;
    }
    return rd(handle, symbol);
}

/* ---- VAAPI vaInitialize wrapper (report 1.17 to pass Chrome's gate) ---- */
VAStatus vaInitialize(VADisplay dpy, int *major, int *minor)
{
    static VAStatus (*real)(VADisplay, int *, int *);
    if (!real) real = captured_real_vaInitialize;
    if (!real) real = (VAStatus (*)(VADisplay, int *, int *))
            real_dlsym_fn()(RTLD_NEXT, "vaInitialize");
    VAStatus st;
    if (real) st = real(dpy, major, minor);
    else { if (major) *major = 1; if (minor) *minor = 17; return VA_STATUS_SUCCESS; }
    if (st == VA_STATUS_SUCCESS && major && minor && *major == 1 && *minor < 17) {
        *minor = 17;
    }
    return st;
}
