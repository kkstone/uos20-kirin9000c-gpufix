/*
 * vaapi-shim.so -- standalone VAAPI video-decode shim (eglrun 2.0.1+)
 *
 * Extracted from gbm-hw-shim.c, which used to carry this logic bundled with
 * the Wayland/GBM/glFinish shims. eglrun 2.0.0 dropped ALL LD_PRELOAD shims,
 * so the HUAWEI "vacore" VAAPI driver once again reports VA-API 1.14 and
 * Chrome 157's VAAPI gate rejects it (video decode falls back / errors out).
 *
 * This shim restores ONLY the VAAPI part, with nothing else:
 *
 *   1) vaInitialize() wrapper: the real vacore driver reports 1.14; Chrome's
 *      VAAPI gate requires >= 1.17. On success we raise the reported minor
 *      to 17. (This is a version-report fix; it does not fake decode.)
 *
 *   2) dlsym() hook: Chrome resolves vaInitialize via dlsym(handle,...) after
 *      dlopen("libva.so.2"), so plain LD_PRELOAD symbol interposition is
 *      bypassed. We hook dlsym() and hand Chrome our wrapper. Everything else
 *      is passed through untouched (incl. the Vulkan ICD's symbol lookups).
 *
 *   3) constructor: restore LIBVA_DRIVER_NAME / LIBVA_DRIVERS_PATH /
 *      LIBVA_DRI3_DISABLE in every child, since Chrome's zygote scrubs these.
 *
 * The real dlsym is resolved via the versioned symbol dlvsym(RTLD_NEXT,
 * "dlsym", "GLIBC_2.17"); __libc_dlsym / plain recursive dlsym segfault or
 * loop on this glibc (proven on the original vaapi spoof).
 *
 * Toggles (all opt-in, default safe):
 *   HISI_VAAPI_SPOOF=0    disable the 1.14->1.17 version spoof (A/B)
 *   VAAPI_SHIM_DEBUG=1    one-shot trace to stderr
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

typedef int VAStatus;
typedef void *VADisplay;
#define VA_STATUS_SUCCESS 0

typedef VAStatus (*fn_vaInitialize)(VADisplay, int *, int *);

/* Captured when Chrome does dlsym(libva_handle, "vaInitialize"). */
static fn_vaInitialize captured_real_vaInitialize = NULL;
static int in_vaInitialize = 0;   /* recursion guard */
VAStatus vaInitialize(VADisplay dpy, int *major, int *minor);

/* Resolve the REAL dlsym via a versioned symbol. */
static void *(*real_dlsym_fn(void))(void *, const char *) {
    static void *(*r)(void *, const char *) = NULL;
    if (!r) r = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    return r;
}

/* Call glibc's real dlsym directly (does NOT re-enter our hook). */
static void *real_dlsym(const char *name) {
    void *(*rd)(void *, const char *) = real_dlsym_fn();
    return rd ? rd(RTLD_NEXT, name) : NULL;
}

static int spoof_enabled(void) {
    const char *v = getenv("HISI_VAAPI_SPOOF");
    return !(v && strcmp(v, "0") == 0);          /* default ON */
}
static int debug_enabled(void) { return getenv("VAAPI_SHIM_DEBUG") != NULL; }

__attribute__((constructor)) static void vaapi_shim_loaded(void) {
    /* Chrome's zygote scrubs these; restore VAAPI env in every child. */
    if (!getenv("LIBVA_DRIVER_NAME")) setenv("LIBVA_DRIVER_NAME", "vacore", 1);
    if (!getenv("LIBVA_DRIVERS_PATH")) setenv("LIBVA_DRIVERS_PATH", "/usr/lib", 1);
    setenv("LIBVA_DRI3_DISABLE", "1", 1);
    if (debug_enabled())
        fprintf(stderr, "[vaapi-shim] loaded pid=%d spoof=%d LD_PRELOAD=%s\n",
                (int)getpid(), spoof_enabled(), getenv("LD_PRELOAD") ?: "NULL");
}

/* ---- dlsym hook: hand Chrome our vaInitialize wrapper ------------------ */
void *dlsym(void *handle, const char *symbol) {
    void *(*rd)(void *, const char *) = real_dlsym_fn();
    if (!rd) return NULL;

    if (handle && strcmp(symbol, "vaInitialize") == 0 &&
        handle != RTLD_NEXT && spoof_enabled()) {
        void *real = rd(handle, symbol);
        if (real && real != (void *)&vaInitialize)
            captured_real_vaInitialize = (fn_vaInitialize)real;
        if (debug_enabled())
            fprintf(stderr, "[vaapi-shim] dlsym(vaInitialize) wrapped -> %p\n", real);
        return (void *)&vaInitialize;
    }
    return rd(handle, symbol);           /* fully transparent otherwise */
}

/* ---- vaInitialize wrapper: report 1.17 so Chrome's gate passes -------- */
VAStatus vaInitialize(VADisplay dpy, int *major, int *minor) {
    if (in_vaInitialize) {               /* never recurse into ourselves */
        if (major) *major = 1;
        if (minor) *minor = 17;
        return VA_STATUS_SUCCESS;
    }
    in_vaInitialize = 1;

    fn_vaInitialize real = captured_real_vaInitialize;
    if (!real || real == &vaInitialize)
        real = (fn_vaInitialize)real_dlsym("vaInitialize");

    VAStatus st;
    if (real) st = real(dpy, major, minor);
    else {
        if (major) *major = 1;
        if (minor) *minor = 17;
        in_vaInitialize = 0;
        return VA_STATUS_SUCCESS;
    }

    if (st == VA_STATUS_SUCCESS && spoof_enabled() && major && minor &&
        *major == 1 && *minor < 17) {
        if (debug_enabled())
            fprintf(stderr, "[vaapi-shim] vaInitialize 1.%d -> 1.17\n", *minor);
        *minor = 17;
    }
    in_vaInitialize = 0;
    return st;
}
