/*
 * icd-wrap.so -- ICD-level wrapper for Maleoon (libhvgr.so).
 *
 * WHY THIS EXISTS:
 *   Chrome 157 performs an identity check on the vkGetInstanceProcAddr
 *   pointer returned by dlsym(). Returning a wrapper function there makes
 *   Chrome exit(0) silently (verified by bisection). So we do NOT hook dlsym.
 *   Instead we wrap at the ICD layer: the Vulkan loader calls our
 *   vk_icdGetInstanceProcAddr() (exported symbol, looked up from the ICD
 *   manifest), and we hand back forged feature/device functions. Chrome sees
 *   the loader's genuine gipa -- no identity check, no dlsym hook at all.
 *
 * APPROACH:
 *   Export vk_icdGetInstanceProcAddr + vk_icdNegotiateLoaderICDInterfaceVersion.
 *   dlopen the real libhvgr.so with RTLD_LOCAL|RTLD_NOW. For every name, ask the
 *   real ICD gipa for the function, and only wrap:
 *     - vkGetPhysicalDeviceFeatures / ...Features2 / ...Features2KHR
 *         -> set vertexPipelineStoresAndAtomics = VK_TRUE  (ANGLE -> ES3)
 *     - vkCreateDevice
 *         -> clear that bit in pEnabledFeatures / pNext before the real call,
 *            restore after (ICD never sees the request -> no FEATURE_NOT_PRESENT)
 *   All other functions are returned verbatim.
 *
 * DEPLOY: a custom ICD manifest points library_path at this .so. Set
 *   VK_ICD_FILENAMES=<custom.json> for Chrome. No LD_PRELOAD required.
 *
 * Build: gcc -shared -fPIC -O2 -o icd-wrap.so icd-wrap.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <vulkan/vulkan.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <time.h>

#define REAL_ICD "/usr/lib/aarch64-linux-gnu/libhvgr.so"

static void *g_icd = NULL;
static PFN_vkVoidFunction (*real_gipa)(VkInstance, const char *) = NULL;
/* Real ICD functions resolved directly by symbol -- avoids passing a
 * VkPhysicalDevice where gipa expects VkInstance (caused segfault). */
static PFN_vkGetPhysicalDeviceFeatures    r_get_features = NULL;
static PFN_vkGetPhysicalDeviceFeatures2   r_get_features2 = NULL;
static PFN_vkGetPhysicalDeviceFeatures2KHR r_get_f2khr = NULL;
static PFN_vkCreateDevice                 r_create_device = NULL;
static PFN_vkGetPhysicalDeviceProperties  r_get_props = NULL;
static PFN_vkGetPhysicalDeviceProperties2 r_get_props2 = NULL;
static PFN_vkEnumerateDeviceExtensionProperties r_enum_dev_ext = NULL;
static PFN_vkQueuePresentKHR r_present = NULL;
static PFN_vkGetDeviceProcAddr r_get_device_proc_addr = NULL;
static PFN_vkGetMemoryFdKHR r_get_memory_fd = NULL;
static PFN_vkGetPhysicalDeviceFormatProperties2 r_get_fmtprops2 = NULL;
static PFN_vkGetPhysicalDeviceImageFormatProperties2 r_get_imgfmtprops2 = NULL;
/* dma-buf import path: hook image create + memory alloc to see/shim the
 * exact pNext structs ANGLE feeds the real ICD (DmaBufImageSiblingVkLinux). */
static PFN_vkCreateImage r_create_image = NULL;
static PFN_vkAllocateMemory r_allocate_memory = NULL;
static PFN_vkBindImageMemory2 r_bind_image_memory2 = NULL;
static PFN_vkGetImageMemoryRequirements2 r_get_img_memreq2 = NULL;
/* Wayland surface creation (SkiaVulkan present path diagnostics). */
typedef VkResult (VKAPI_PTR *PFN_vkCreateWaylandSurfaceKHR)(VkInstance, const void *, const VkAllocationCallbacks *, VkSurfaceKHR *);
typedef VkResult (VKAPI_PTR *PFN_vkGetPhysWaylandPresSupport)(VkPhysicalDevice, uint32_t, void *);
static PFN_vkCreateWaylandSurfaceKHR r_create_wl_surface = NULL;
static PFN_vkGetPhysWaylandPresSupport r_wl_pres_support = NULL;
/* WSI functions are NOT returned by libhvgr's vkGetDeviceProcAddr (real=no),
 * but ARE exported as global symbols. The loader resolves them via dlsym from
 * the ICD .so (the WSI trampoline), bypassing gipa. We resolve them directly
 * by symbol and hand our wrappers back from vkGetDeviceProcAddr regardless. */
static PFN_vkQueuePresentKHR r_present_sym = NULL;

/* 2026-11-21 ROOT CAUSE FIX:
 * ANGLE's RendererVk::getMaxSupportedESVersion() caps ES to 2.0 unless
 * mFeatures.provokingVertex.enabled, which is set only when
 * VK_EXT_provoking_vertex is present AND its feature struct reports
 * provokingVertexLast=VK_TRUE. Maleoon lacks this extension entirely ->
 * ANGLE returns ES2 -> eglCreateContext ES3 = EGL_BAD_ATTRIBUTE.
 * SwiftShader has it -> ES3 works. We forge BOTH the extension and the
 * feature bit at the ICD layer. This is the missing piece. */
#define VK_STRUCT_PROVOKING_VERTEX_FEATURES_EXT 1000254000
#define VK_EXT_PROVOKING_VERTEX_NAME "VK_EXT_provoking_vertex"
#define VK_EXT_EXTERNAL_MEMORY_DMA_BUF_NAME "VK_EXT_external_memory_dma_buf"
#define VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_NAME "VK_EXT_image_drm_format_modifier"
/* Extensions we inject into the ICD's reported device-extension list.
 * - provoking_vertex: unlocks ANGLE ES3 (see forge_feature_pnext).
 * - external_memory_dma_buf: declares that VK_KHR_external_memory_fd exports
 *   real dma-buf fds (verified on Maleoon via /proc/self/fd -> /dmabuf:).
 *   Without it ANGLE skips its zero-copy DmaBufImageSibling path -> the
 *   display compositor falls back to CPU copy -> ~15-18fps software compose.
 * Both are declarations/consumer-side names: no new ICD entry points needed,
 * and both must be STRIPPED before calling the real ICD (it doesn't support
 * them and would return VK_ERROR_EXTENSION_NOT_PRESENT). */
static const char *INJECT_EXTS[] = {
    VK_EXT_PROVOKING_VERTEX_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_NAME,
    VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_NAME,
};
#define N_INJECT_EXTS (sizeof(INJECT_EXTS)/sizeof(INJECT_EXTS[0]))
/* VK_EXT_image_drm_format_modifier injection is OFF by default: injecting it
 * drives ANGLE to import KWin's AFBC dma-buf, which Maleoon cannot render to
 * (device lost) and GaneshGL cannot wrap as a renderable SkSurface. Default OFF
 * keeps the stable ANGLE-Vulkan WebGL2 baseline (software shm compose, ~18fps).
 * ICD_ENABLE_MODIFIER=1 re-enables the experiment (for research only). */
static int inject_modifier(void) {
    static int init=0; static int on=0;
    if(!init){ const char*e=getenv("ICD_ENABLE_MODIFIER"); if(e) on=atoi(e); init=1; }
    return on;
}
static int forge_provoking(void) {
    static int init=0; static int on=1;
    if(!init){ const char*e=getenv("ICD_FORGE_PROVOKING"); if(e) on=atoi(e); init=1; }
    return on;
}

/* ANGLE's vk_renderer.cpp initFeatures() switches on vendorID/deviceID to
 * select a GPU architecture feature table. Huawei (0x19e5 / 0x10001000) is
 * unknown -> "Unknown GPU architecture" -> ES3 disabled. Forge vendorID to
 * ARM Mali (0x13b5) so ANGLE picks a known table. Configurable via env. */
static uint32_t forge_vendor(void) {
    static int init=0; static uint32_t v=0x13b5;
    if(!init){ const char*e=getenv("ICD_FORGE_VENDOR"); if(e) v=(uint32_t)strtoul(e,NULL,0); init=1; }
    return v;
}
static uint32_t forge_device(void) {
    static int init=0; static uint32_t d=0; /* 0 = keep real */
    if(!init){ const char*e=getenv("ICD_FORGE_DEVICEID"); if(e) d=(uint32_t)strtoul(e,NULL,0); init=1; }
    return d;
}
/* Maleoon reports Vulkan 1.2.250 (0x4020fa); SwiftShader (which ANGLE
 * accepts for ES3) reports 1.3.0 (0x403000). Chrome 157's ANGLE gates some
 * ES3 capability paths on apiVersion >= 1.3. Forge to 1.3.0 by default,
 * override with ICD_FORGE_API (0 = keep real). */
static uint32_t forge_api(void) {
    static int init=0; static uint32_t a=0; /* 0 = keep real apiVersion */
    if(!init){ const char*e=getenv("ICD_FORGE_API"); if(e) a=(uint32_t)strtoul(e,NULL,0); init=1; }
    return a; /* opt-in: ICD_FORGE_API=0x403000 to pretend Vulkan 1.3 */
}
/* ANGLE's device quirk / feature tables match on deviceName string prefixes
 * (e.g. "Mali-G%d", "Adreno (TM) %d"). "Maleoon 910" falls into Huawei-only
 * workarounds that can gate ES3. Override the reported deviceName to a fully
 * supported GPU identity; default NULL = keep real. */
static const char* forge_name(void) {
    static int init=0; static const char* n=NULL;
    if(!init){ n=getenv("ICD_FORGE_NAME"); init=1; }
    return n;
}
/* 2026-11-21: deviceType is a strong suspect. Maleoon reports
 * DISCRETE_GPU(1); SwiftShader (which ANGLE accepts for ES3) reports CPU(4).
 * ANGLE applies strict per-vendor driver feature tables to real GPUs but a
 * relaxed path to CPU devices. -1 = keep real. */
static int forge_devtype(void) {
    static int init=0; static int t=-1;
    if(!init){ const char*e=getenv("ICD_FORGE_DEVTYPE"); if(e) t=(int)strtol(e,NULL,0); init=1; }
    return t;
}

/* ---------------- debug (opt-in, default silent) ---------------- */
static int g_debug = -1;
static int dbg(void) {
    if (g_debug < 0) {
        const char *e = getenv("ICD_WRAP_DEBUG");
        g_debug = (e && !strcmp(e, "1")) ? 1 : 0;
    }
    return g_debug;
}
#define DBG(...) do { if (dbg()) { \
    FILE *f = fopen("/tmp/icd-wrap.log", "a"); \
    if (f) { \
        struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); \
        fprintf(f, "[%lld.%.3lldms pid=%d] ", \
            (long long)ts.tv_sec, (long long)(ts.tv_nsec/1000000), (int)getpid()); \
        fprintf(f, __VA_ARGS__); fclose(f); \
    } \
} } while (0)

/* Maleoon 910 advertises these as 0; ANGLE's Vulkan ES3 capability check
 * reads them. Forge all three to 1. createDevice scrubs them back to 0
 * before the real ICD sees the request (avoiding FEATURE_NOT_PRESENT -8). */
static void forge_features(VkPhysicalDeviceFeatures *f) {
    if (!f) return;
    f->vertexPipelineStoresAndAtomics = VK_TRUE;
    f->multiViewport                  = VK_TRUE;
    f->pipelineStatisticsQuery        = VK_TRUE;
    /* 2026-11-21: ANGLE's ES3 gate has a dedicated check:
     * "Vulkan textureCompressionBC feature required or both
     *  textureCompressionETC2 and textureCompressionASTC required."
     * Maleoon reports BC=0 (but ETC2=1/ASTC=1). SwiftShader -- which ANGLE
     * accepts for ES3 -- reports BC=1. Per-bit diff of all 55 core features
     * showed BC is the ONLY ANGLE-ES3-gate bit we had not forged. */
    f->textureCompressionBC           = VK_TRUE;
}

/* ANGLE's ES3 capability check also reads feature bits from the
 * VkPhysicalDeviceFeatures2 pNext chain (Vulkan 1.1 / 1.2 feature structs),
 * which are all VkBool32 fields. Set every bool after sType/pNext to true.
 * createDevice scrubs by saving/restoring the whole chain (see below). */
static void forge_feature_pnext(VkPhysicalDeviceFeatures2 *out) {
    if (!out) return;
    VkBaseInStructure *p = (VkBaseInStructure *)out->pNext;
    while (p) {
        /* System vulkan.h is old; use numeric sType values.
         * SIZES MUST BE THE EXACT sizeof() ON LP64 (measured from
         * Vulkan-Headers 1.3.280) -- the old hardcoded 96/176/88 were
         * WRONG: Vk11 = 64 bytes, so 96 overran 32 bytes and corrupted
         * the next node in ANGLE's pNext chain. Fixed 2026-11-21.
         *   VkPhysicalDeviceVulkan11Features: 16 + 12*4 = 64
         *   VkPhysicalDeviceVulkan12Features: 16 + 47*4 = 204
         *   VkPhysicalDeviceVulkan13Features: 16 + 15*4 = 76
         */
        size_t sz = 0;
        if ((int)p->sType == 46) sz = 64;   /* Vulkan11Features (12 bools) */
        else if ((int)p->sType == 51) sz = 204; /* Vulkan12Features (47 bools) */
        else if ((int)p->sType == 69) sz = 76;  /* Vulkan13Features (15 bools) */
        else if ((int)p->sType == VK_STRUCT_PROVOKING_VERTEX_FEATURES_EXT && forge_provoking()) {
            /* VkPhysicalDeviceProvokingVertexFeaturesEXT: sType+pNext then
             * provokingVertexLast (offset 16) + transformFeedbackPreserves (20).
             * ANGLE gates ES3 on provokingVertexLast == VK_TRUE. */
            VkBool32 *last = (VkBool32 *)((char *)p + 16);
            *last = VK_TRUE;
            DBG("FORGE provokingVertexLast=1 (ANGLE ES3 gate!)\n");
        }
        if (sz > 16) {
            /* All fields from offset 16 (after sType+pNext) up to sz are
             * VkBool32. Byte 8..15 is pNext - never touch it. Bound by the
             * exact struct size so we cannot spill into the next chain node. */
            VkBool32 *b = (VkBool32 *)((char *)p + 16);
            for (size_t o = 16; o + sizeof(VkBool32) <= sz; o += sizeof(VkBool32), b++)
                if (*b == VK_FALSE) *b = VK_TRUE;
            DBG("forge pNext sType=0x%x sz=%zu\n", p->sType, sz);
        }
        p = (VkBaseInStructure *)p->pNext;
    }
}

/* ---------------- present timing diagnostics ---------------- */
/* Hook vkQueuePresentKHR at the ICD layer to measure present cost.
 * If avg_dur is large (many ms) -> CPU copy (software compose).
 * If avg_dur is tiny but avg_interval is ~50ms -> vsync/scheduling wait,
 * which we CAN phase-lock with a delay shim. Enable: ICD_PRESENT_TIMING=1 */
static int present_timing = -1;
static unsigned long long pk_count=0, pk_last=0, pk_sum_iv=0, pk_sum_dur=0, pk_iv_n=0;
static VkResult VKAPI_PTR w_present(VkQueue q, const VkPresentInfoKHR *info) {
    /* Unconditional first-call marker to settle whether present reaches us. */
    {
        static int once=0;
        if (!__sync_fetch_and_add(&once,1)) {
            FILE *m=fopen("/tmp/icd-present-CALLED","a");
            if(m){ fprintf(m,"pid=%d w_present CALLED q=%p info=%p\n",(int)getpid(),(void*)q,(void*)info); fclose(m); }
        }
    }
    /* r_present (via gipa) may be NULL for WSI; fall back to the global symbol. */
    PFN_vkQueuePresentKHR real = r_present ? r_present : r_present_sym;
    if (!real) return VK_ERROR_INITIALIZATION_FAILED;
    if (present_timing < 0) { const char *e=getenv("ICD_PRESENT_TIMING");
        present_timing = (e && !strcmp(e,"1")); }
    if (!present_timing) return real(q, info);
    struct timespec a,b;
    clock_gettime(CLOCK_MONOTONIC,&a);
    VkResult r = real(q, info);
    clock_gettime(CLOCK_MONOTONIC,&b);
    unsigned long long now=(unsigned long long)a.tv_sec*1000000000ULL+a.tv_nsec;
    unsigned long long dur=(unsigned long long)(b.tv_sec-a.tv_sec)*1000000000ULL+(b.tv_nsec-a.tv_nsec);
    if (pk_last) { pk_sum_iv += now-pk_last; pk_iv_n++; }
    pk_last=now; pk_sum_dur+=dur; pk_count++;
    if (pk_count % 60 == 0)
        DBG("PRESENT n=%llu avg_interval=%.2fms avg_present_dur=%.2fms\n",
            pk_count, pk_iv_n?(double)pk_sum_iv/pk_iv_n/1e6:0.0,
            (double)pk_sum_dur/pk_count/1e6);
    return r;
}

/* ---------------- memory fd export diagnostics ----------------
 * Log every vkGetMemoryFdKHR call with its handleType and result. This tells
 * us whether ANGLE/Maleoon is exporting dma-buf (DMA_BUF_BIT_EXT) or opaque
 * fd -- the key to hardware vs software compositing. */
static VkResult VKAPI_PTR w_get_memory_fd(VkDevice device,
        const VkMemoryGetFdInfoKHR *info, int *fd) {
    if (!r_get_memory_fd) return VK_ERROR_FEATURE_NOT_PRESENT;
    VkResult r = r_get_memory_fd(device, info, fd);
    if (getenv("ICD_MEMFD_TRACE"))
        DBG("GETMEMFD handleType=%d r=%d fd=%d\n",
            info ? (int)info->handleType : -1, (int)r, fd ? *fd : -1);
    return r;
}

/* ---------------- dma-buf import path tracing / shim ----------------
 * After injecting VK_EXT_image_drm_format_modifier, ANGLE takes its zero-copy
 * DmaBufImageSiblingVkLinux path: vkCreateImage with external/modifier pNext,
 * then vkAllocateMemory with VkImportMemoryFdInfoKHR(DMA_BUF) + dedicated.
 * The real Maleoon ICD rejects these -> "Bad allocation"/context loss. Log the
 * exact parameters so we can shim only what's needed. */
#define ST_EXTERNAL_MEMORY_IMAGE_CREATE_INFO 1000072001
#define ST_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT 1000158004
#define ST_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT 1000158003
#define ST_IMPORT_MEMORY_FD_INFO_KHR 1000074000
#define ST_MEMORY_DEDICATED_ALLOCATE_INFO 1000127001
#define ST_IMAGE_FORMAT_LIST_CREATE_INFO 1000078000

static const char *stype_name(int s) {
    switch (s) {
        case ST_EXTERNAL_MEMORY_IMAGE_CREATE_INFO: return "ExtMemImgCreate";
        case ST_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT: return "DrmModExplicit";
        case ST_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT: return "DrmModList";
        case ST_IMPORT_MEMORY_FD_INFO_KHR: return "ImportFd";
        case ST_MEMORY_DEDICATED_ALLOCATE_INFO: return "DedicatedAlloc";
        case ST_IMAGE_FORMAT_LIST_CREATE_INFO: return "ImgFmtList";
        default: return "?";
    }
}

static VkResult VKAPI_PTR w_create_image(VkDevice dev, const VkImageCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkImage *image) {
    if (!r_create_image) return VK_ERROR_INITIALIZATION_FAILED;
    if (getenv("ICD_IMPORT_TRACE")) {
        DBG("CREATE_IMAGE fmt=%d tiling=%d usage=0x%x extent=%ux%ux%u\n",
            info?info->format:-1, info?info->tiling:-1, info?info->usage:0,
            info?info->extent.width:0, info?info->extent.height:0, info?info->extent.depth:0);
        const VkBaseInStructure *p = info ? (const VkBaseInStructure *)info->pNext : NULL;
        while (p) {
            if ((int)p->sType == ST_EXTERNAL_MEMORY_IMAGE_CREATE_INFO) {
                const uint32_t *ht = (const uint32_t *)((const char *)p + 16);
                DBG("  pNext ExtMemImgCreate handleTypes=0x%x\n", *ht);
            } else if ((int)p->sType == ST_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT) {
                const uint64_t *mod = (const uint64_t *)((const char *)p + 16);
                const uint32_t *pc = (const uint32_t *)((const char *)p + 24);
                DBG("  pNext DrmModExplicit mod=0x%llx planeCount=%u\n",
                    (unsigned long long)*mod, *pc);
            } else {
                DBG("  pNext %s(%d)\n", stype_name((int)p->sType), (int)p->sType);
            }
            p = p->pNext;
        }
    }
    VkResult r = r_create_image(dev, info, alloc, image);
    if (getenv("ICD_IMPORT_TRACE"))
        DBG("CREATE_IMAGE r=%d image=%p\n", (int)r, (void*)(image?*image:0));
    return r;
}

static VkResult VKAPI_PTR w_allocate_memory(VkDevice dev, const VkMemoryAllocateInfo *info,
        const VkAllocationCallbacks *alloc, VkDeviceMemory *mem) {
    if (!r_allocate_memory) return VK_ERROR_INITIALIZATION_FAILED;
    if (getenv("ICD_IMPORT_TRACE")) {
        const VkBaseInStructure *p = info ? (const VkBaseInStructure *)info->pNext : NULL;
        DBG("ALLOC_MEM size=%llu type=%u\n",
            info?(unsigned long long)info->allocationSize:0, info?info->memoryTypeIndex:0);
        while (p) {
            if ((int)p->sType == ST_IMPORT_MEMORY_FD_INFO_KHR) {
                const uint32_t *ht = (const uint32_t *)((const char *)p + 16);
                const int *fd = (const int *)((const char *)p + 20);
                DBG("  pNext ImportFd handleType=0x%x fd=%d\n", *ht, *fd);
            } else if ((int)p->sType == ST_MEMORY_DEDICATED_ALLOCATE_INFO) {
                DBG("  pNext DedicatedAlloc\n");
            } else {
                DBG("  pNext %s(%d)\n", stype_name((int)p->sType), (int)p->sType);
            }
            p = p->pNext;
        }
    }
    VkResult r = r_allocate_memory(dev, info, alloc, mem);
    if (getenv("ICD_IMPORT_TRACE"))
        DBG("ALLOC_MEM r=%d mem=%p\n", (int)r, (void*)(mem?*mem:0));
    return r;
}

/* Wayland surface creation: VkWaylandSurfaceCreateInfoKHR layout is
 * sType@0 pNext@8 flags@16 (u32) display@24 wl_display* surface@32 wl_surface*. */
static VkResult VKAPI_PTR w_create_wl_surface(VkInstance inst, const void *info,
        const VkAllocationCallbacks *alloc, VkSurfaceKHR *surf) {
    if (!r_create_wl_surface) return VK_ERROR_EXTENSION_NOT_PRESENT;
    if (getenv("ICD_SURFACE_TRACE")) {
        const void *dpy = info ? *(void **)((const char *)info + 24) : NULL;
        const void *wls = info ? *(void **)((const char *)info + 32) : NULL;
        DBG("CREATE_WL_SURFACE info=%p display=%p wl_surface=%p\n", info, dpy, wls);
    }
    VkResult r = r_create_wl_surface(inst, info, alloc, surf);
    if (getenv("ICD_SURFACE_TRACE"))
        DBG("CREATE_WL_SURFACE r=%d surf=%p\n", (int)r, (void*)(surf?*surf:0));
    return r;
}
static VkResult VKAPI_PTR w_wl_pres_support(VkPhysicalDevice dev, uint32_t q, void *dpy) {
    if (!r_wl_pres_support) return VK_FALSE;
    VkResult r = r_wl_pres_support(dev, q, dpy);
    if (getenv("ICD_SURFACE_TRACE"))
        DBG("WL_PRES_SUPPORT q=%u dpy=%p r=%d\n", q, dpy, (int)r);
    return r;
}

/* ---------------- one-time real ICD load ---------------- */
static void load_real(void) {
    if (g_icd) return;
    g_icd = dlopen(REAL_ICD, RTLD_NOW | RTLD_LOCAL);
    if (!g_icd) {
        DBG("dlopen(%s) FAILED: %s\n", REAL_ICD, dlerror());
        return;
    }
    real_gipa = (PFN_vkVoidFunction (*)(VkInstance, const char *))
        dlsym(g_icd, "vk_icdGetInstanceProcAddr");
    if (!real_gipa)
        real_gipa = (PFN_vkVoidFunction (*)(VkInstance, const char *))
            dlsym(g_icd, "vkGetInstanceProcAddr");
    r_get_features  = (PFN_vkGetPhysicalDeviceFeatures)dlsym(g_icd, "vkGetPhysicalDeviceFeatures");
    r_get_features2 = (PFN_vkGetPhysicalDeviceFeatures2)dlsym(g_icd, "vkGetPhysicalDeviceFeatures2");
    r_get_f2khr     = (PFN_vkGetPhysicalDeviceFeatures2KHR)dlsym(g_icd, "vkGetPhysicalDeviceFeatures2KHR");
    r_create_device = (PFN_vkCreateDevice)dlsym(g_icd, "vkCreateDevice");
    r_get_props     = (PFN_vkGetPhysicalDeviceProperties)dlsym(g_icd, "vkGetPhysicalDeviceProperties");
    r_get_props2    = (PFN_vkGetPhysicalDeviceProperties2)dlsym(g_icd, "vkGetPhysicalDeviceProperties2");
    r_enum_dev_ext  = (PFN_vkEnumerateDeviceExtensionProperties)dlsym(g_icd, "vkEnumerateDeviceExtensionProperties");
    r_present       = (PFN_vkQueuePresentKHR)dlsym(g_icd, "vkQueuePresentKHR");
    r_get_device_proc_addr = (PFN_vkGetDeviceProcAddr)dlsym(g_icd, "vkGetDeviceProcAddr");
    r_get_memory_fd = (PFN_vkGetMemoryFdKHR)dlsym(g_icd, "vkGetMemoryFdKHR");
    r_present_sym   = (PFN_vkQueuePresentKHR)dlsym(g_icd, "vkQueuePresentKHR");
    r_get_fmtprops2 = (PFN_vkGetPhysicalDeviceFormatProperties2)dlsym(g_icd, "vkGetPhysicalDeviceFormatProperties2");
    r_get_imgfmtprops2 = (PFN_vkGetPhysicalDeviceImageFormatProperties2)dlsym(g_icd, "vkGetPhysicalDeviceImageFormatProperties2");
    r_create_image = (PFN_vkCreateImage)dlsym(g_icd, "vkCreateImage");
    r_allocate_memory = (PFN_vkAllocateMemory)dlsym(g_icd, "vkAllocateMemory");
    r_bind_image_memory2 = (PFN_vkBindImageMemory2)dlsym(g_icd, "vkBindImageMemory2");
    r_get_img_memreq2 = (PFN_vkGetImageMemoryRequirements2)dlsym(g_icd, "vkGetImageMemoryRequirements2");
    r_create_wl_surface = (PFN_vkCreateWaylandSurfaceKHR)dlsym(g_icd, "vkCreateWaylandSurfaceKHR");
    r_wl_pres_support = (PFN_vkGetPhysWaylandPresSupport)dlsym(g_icd, "vkGetPhysicalDeviceWaylandPresentationSupportKHR");
    DBG("loaded real ICD gipa=%p feats=%p/%p/%p cd=%p enumext=%p present=%p wlsurf=%p\n", (void *)real_gipa,
        (void*)r_get_features,(void*)r_get_features2,(void*)r_get_f2khr,(void*)r_create_device,
        (void*)r_enum_dev_ext);
}

/* ---------------- physical-device-properties wrapper ---------------- */
static void VKAPI_PTR w_get_props(VkPhysicalDevice dev,
                                  VkPhysicalDeviceProperties *out) {
    if (!r_get_props) return;
    r_get_props(dev, out);
    if (out) {
        if (out->vendorID != forge_vendor()) {
            DBG("forge props vendorID 0x%x -> 0x%x deviceID 0x%x\n",
                out->vendorID, forge_vendor(), out->deviceID);
            out->vendorID = forge_vendor();
            if (forge_device()) out->deviceID = forge_device();
        }
        if (forge_api() && out->apiVersion != forge_api()) {
            DBG("forge props apiVersion 0x%x -> 0x%x\n", out->apiVersion, forge_api());
            out->apiVersion = forge_api();
        }
        if (forge_devtype() >= 0 && (int)out->deviceType != forge_devtype()) {
            DBG("forge props deviceType %d -> %d\n", (int)out->deviceType, forge_devtype());
            out->deviceType = (VkPhysicalDeviceType)forge_devtype();
        }
        if (forge_name()) {
            DBG("forge props deviceName '%s' -> '%s'\n", out->deviceName, forge_name());
            snprintf(out->deviceName, sizeof(out->deviceName), "%s", forge_name());
        }
    }
}
static void VKAPI_PTR w_get_props2(VkPhysicalDevice dev,
                                   VkPhysicalDeviceProperties2 *out) {
    if (!r_get_props2) {
        /* ICD may only export the core 1.1 entry point as device extension;
         * fall back to core props if available. */
        if (r_get_props && out)
            w_get_props(dev, &out->properties);
        return;
    }
    r_get_props2(dev, out);
    if (out) {
        if (out->properties.vendorID != forge_vendor()) {
            DBG("forge props2 vendorID 0x%x -> 0x%x deviceID 0x%x\n",
                out->properties.vendorID, forge_vendor(), out->properties.deviceID);
            out->properties.vendorID = forge_vendor();
            if (forge_device()) out->properties.deviceID = forge_device();
        }
        if (forge_api() && out->properties.apiVersion != forge_api()) {
            DBG("forge props2 apiVersion 0x%x -> 0x%x\n", out->properties.apiVersion, forge_api());
            out->properties.apiVersion = forge_api();
        }
        if (forge_devtype() >= 0 && (int)out->properties.deviceType != forge_devtype()) {
            DBG("forge props2 deviceType %d -> %d\n", (int)out->properties.deviceType, forge_devtype());
            out->properties.deviceType = (VkPhysicalDeviceType)forge_devtype();
        }
        if (forge_name()) {
            DBG("forge props2 deviceName '%s' -> '%s'\n", out->properties.deviceName, forge_name());
            snprintf(out->properties.deviceName, sizeof(out->properties.deviceName), "%s", forge_name());
        }
    }
}

/* ---------------- feature-query wrappers ---------------- */
static void VKAPI_PTR w_get_features(VkPhysicalDevice dev,
                                     VkPhysicalDeviceFeatures *out) {
    if (!r_get_features) return;
    r_get_features(dev, out);
    if (out) { forge_features(out); DBG("forge features (vpsa/mv/psq=1)\n"); }
}
static void VKAPI_PTR w_get_features2(VkPhysicalDevice dev,
                                      VkPhysicalDeviceFeatures2 *out) {
    if (!r_get_features2) return;
    r_get_features2(dev, out);
    if (out) { forge_features(&out->features); forge_feature_pnext(out); DBG("forge features2 (vpsa/mv/psq=1)\n"); }
}
static void VKAPI_PTR w_get_features2khr(VkPhysicalDevice dev,
                                         VkPhysicalDeviceFeatures2 *out) {
    if (!r_get_f2khr) return;
    r_get_f2khr(dev, out);
    if (out) { forge_features(&out->features); forge_feature_pnext(out); DBG("forge features2khr (vpsa/mv/psq=1)\n"); }
}
static VkResult VKAPI_PTR w_create_device(VkPhysicalDevice dev,
        const VkDeviceCreateInfo *info,
        const VkAllocationCallbacks *alloc, VkDevice *out) {
    if (!r_create_device) return VK_ERROR_INITIALIZATION_FAILED;
    VkDeviceCreateInfo *m = (VkDeviceCreateInfo *)info;
    VkPhysicalDeviceFeatures *feat = m ? (VkPhysicalDeviceFeatures *)m->pEnabledFeatures : NULL;
    VkPhysicalDeviceFeatures2 *f2 = NULL;
    if (m) {
        VkBaseInStructure *p = (VkBaseInStructure *)m->pNext;
        while (p) {
            if (p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2 ||
                p->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2_KHR) {
                f2 = (VkPhysicalDeviceFeatures2 *)p; break;
            }
            p = (VkBaseInStructure *)p->pNext;
        }
    }
    /* Save the 4 forged bits, scrub to 0 for the real ICD, restore after.
     * textureCompressionBC added 2026-11-21 (ANGLE ES3 gate bit). */
    VkBool32 sv[4]={0}, sv2[4]={0};
    VkBool32 *slots[4], *slots2[4];
    slots[0]=feat?&feat->vertexPipelineStoresAndAtomics:NULL;
    slots[1]=feat?&feat->multiViewport:NULL;
    slots[2]=feat?&feat->pipelineStatisticsQuery:NULL;
    slots[3]=feat?&feat->textureCompressionBC:NULL;
    slots2[0]=f2?&f2->features.vertexPipelineStoresAndAtomics:NULL;
    slots2[1]=f2?&f2->features.multiViewport:NULL;
    slots2[2]=f2?&f2->features.pipelineStatisticsQuery:NULL;
    slots2[3]=f2?&f2->features.textureCompressionBC:NULL;
    for (int i=0;i<4;i++){
        if(slots[i]){ sv[i]=*slots[i]; *slots[i]=VK_FALSE; }
        if(slots2[i]){ sv2[i]=*slots2[i]; *slots2[i]=VK_FALSE; }
    }
    /* FIX 2026-11-21: also scrub the pNext feature chain. forge_feature_pnext
     * sets every VkBool32 in Vulkan11/12/13 feature structs to 1. If ANGLE
     * passes those structs in DeviceCreateInfo.pNext, the real ICD would see
     * features it does not have and return FEATURE_NOT_PRESENT (-8). We save
     * the whole bool region of each node, zero it, then restore afterwards.
     * EXACT struct sizes (LP64) -- same values as forge_feature_pnext. */
    typedef struct { VkBaseInStructure *node; size_t off; size_t bytes;
                     unsigned char save[256]; } PnextSave;
    PnextSave ps[8]; int np=0;
    VkBaseInStructure *cp = m ? (VkBaseInStructure *)m->pNext : NULL;
    while (cp && np < 8) {
        size_t sz=0;
        if ((int)cp->sType==46) sz=64;
        else if ((int)cp->sType==51) sz=204;
        else if ((int)cp->sType==69) sz=76;
        else if ((int)cp->sType==VK_STRUCT_PROVOKING_VERTEX_FEATURES_EXT) sz=24; /* 2 bools */
        if (sz>16) {
            ps[np].node=cp; ps[np].off=16; ps[np].bytes=sz-16;
            memcpy(ps[np].save, (char*)cp+16, sz-16);
            memset((char*)cp+16, 0, sz-16);
            DBG("createDevice scrub pNext sType=%d bytes=%zu\n",(int)cp->sType,sz-16);
            np++;
        }
        cp=(VkBaseInStructure*)cp->pNext;
    }
    /* Also strip VK_EXT_provoking_vertex from the enabled-extension list;
     * the real ICD does not support it and would return
     * VK_ERROR_EXTENSION_NOT_PRESENT. Build a local copy of the name array
     * with that entry replaced by a harmless always-present extension. */
    VkDeviceCreateInfo dci_copy;
    const char *ext_copy[64];
    int stripped=0;
    if (m && m->ppEnabledExtensionNames && m->enabledExtensionCount <= 64) {
        dci_copy = *m;
        uint32_t keep=0;
        for (uint32_t i=0;i<m->enabledExtensionCount;i++){
            int drop=0;
            for (uint32_t k=0;k<N_INJECT_EXTS;k++)
                if (m->ppEnabledExtensionNames[i] &&
                    !strcmp(m->ppEnabledExtensionNames[i], INJECT_EXTS[k])) {
                    DBG("createDevice REMOVE ext %s idx=%u\n", INJECT_EXTS[k], i);
                    stripped=1; drop=1; break;
                }
            if (!drop) ext_copy[keep++]=m->ppEnabledExtensionNames[i];
        }
        if (stripped) {
            dci_copy.ppEnabledExtensionNames = ext_copy;
            dci_copy.enabledExtensionCount = keep;
        }
    }
    /* DIAG: dump the exact device extensions ANGLE enables. This reveals the
     * present path: external_memory_fd / wayland_surface / dma_buf => hardware
     * zero-copy present; their absence => software/copy present. */
    if (m && m->ppEnabledExtensionNames && !getenv("ICD_NO_EXT_DUMP")) {
        for (uint32_t i=0;i<m->enabledExtensionCount;i++)
            if (m->ppEnabledExtensionNames[i])
                DBG("ANGLE enabled dev ext[%u]=%s\n", i, m->ppEnabledExtensionNames[i]);
    }
    DBG("createDevice scrub save=%d%d%d%d/%d%d%d%d pnext=%d\n",
        (int)sv[0],(int)sv[1],(int)sv[2],(int)sv[3],
        (int)sv2[0],(int)sv2[1],(int)sv2[2],(int)sv2[3],np);
    VkResult r = r_create_device(dev, stripped ? &dci_copy : info, alloc, out);
    for (int i=0;i<np;i++)
        memcpy((char*)ps[i].node+ps[i].off, ps[i].save, ps[i].bytes);
    for (int i=0;i<4;i++){
        if(slots2[i]) *slots2[i]=sv2[i];
        if(slots[i]) *slots[i]=sv[i];
    }
    DBG("createDevice r=%d\n", (int)r);
    return r;
}

/* ---------------- device extension enumeration wrapper ---------------- */
/* Inject declarations that unlock ANGLE capabilities:
 *   VK_EXT_provoking_vertex        -> ES3 (forge_feature_pnext sets the bit)
 *   VK_EXT_external_memory_dma_buf -> zero-copy dma-buf display compositing
 * The real ICD has N extensions; we inflate the count query by N_INJECT_EXTS
 * so the caller allocates enough slots, then append the names at [N..]. */
static VkResult VKAPI_PTR w_enum_dev_ext(VkPhysicalDevice dev,
        const char *layer, uint32_t *count, VkExtensionProperties *out) {
    if (!r_enum_dev_ext) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult r = r_enum_dev_ext(dev, layer, count, out);
    if (!forge_provoking()) return r;
    uint32_t n_inject = N_INJECT_EXTS - (inject_modifier() ? 0 : 1);
    if (!out) {
        if (r == VK_SUCCESS && count) {
            (*count) += n_inject;
            DBG("enum_ext count +=%u -> %u (inject)\n", (unsigned)n_inject, *count);
        }
        return r;
    }
    /* The real ICD wrote [0,realN) and set *count=realN. Append at realN.
     * (Using *count-1 would overwrite a real extension such as
     *  VK_KHR_external_memory_fd -> lost dma-buf -> software compose.) */
    if (*count < 1) return r;
    uint32_t realN = *count;
    uint32_t n = realN;
    for (uint32_t k=0;k<N_INJECT_EXTS;k++) {
        const char *name = INJECT_EXTS[k];
        if (!inject_modifier() && name == VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_NAME) continue;
        int present=0;
        for (uint32_t i=0;i<n;i++)
            if (!strcmp(out[i].extensionName, name)) { present=1; break; }
        if (present) continue;
        strncpy(out[n].extensionName, name, sizeof(out[n].extensionName)-1);
        out[n].extensionName[sizeof(out[n].extensionName)-1] = 0;
        out[n].specVersion = 1;
        DBG("enum_ext INJECTED %s at idx=%u (real_count=%u)\n", name, n, realN);
        n++;
    }
    *count = n;
    return r;
}

/* ---------------- ICD entry points (exported) ---------------- */
/* 2026-11-21: trace every function name the loader asks us for. This settles
 * whether device-level present/swapchain calls reach the ICD wrapper at all.
 * Enable with ICD_GIPA_TRACE=1 (or ICD_GIPA_TRACE=present for present-only). */
static int gipa_trace = -1;
static int gipa_trace_present_only = 0;
static void gipa_log(const char *name, int wrapped) {
    if (gipa_trace < 0) {
        const char *e = getenv("ICD_GIPA_TRACE");
        gipa_trace = (e && strcmp(e,"1")==0);
        gipa_trace_present_only = (e && strcmp(e,"present")==0);
    }
    if (!gipa_trace && !gipa_trace_present_only) return;
    int interesting = strstr(name,"Present") || strstr(name,"present") ||
                      strstr(name,"Swapchain") || strstr(name,"swapchain") ||
                      strstr(name,"Wayland") || strstr(name,"Queue") ||
                      strstr(name,"GetDeviceProc") || strstr(name,"MemoryFd") ||
                      strstr(name,"DrmFormat");
    if (gipa_trace_present_only && !interesting) return;
    DBG("GIPA req '%s' real=%s wrapped=%s\n", name,
        real_gipa?"yes":"no", wrapped?"WRAP":"raw");
}

/* ---------------- DRM format modifier query shims ----------------
 * ANGLE's DmaBufImageSiblingVkLinux imports the compositor's dma-buf. It calls
 * vkGetPhysicalDeviceFormatProperties2 with a VkDrmFormatModifierPropertiesList
 * pNext, and vkGetPhysicalDeviceImageFormatProperties2 with DRM-modifier +
 * DMA_BUF external-image info. Maleoon's ICD knows neither sType, so ANGLE
 * finds no supported modifier and gives up (the -8 at initImpl:612). We
 * synthesize the answers here so ANGLE proceeds to actually import the fd.
 * DRM_FORMAT_MOD_LINEAR (0) is the safe universal starting point. */
#define ST_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT 1000158000
#define ST_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT 1000158002
#define ST_IMAGE_DRM_FORMAT_MODIFIER_EXPLICIT_CREATE_INFO_EXT 1000158004
#define ST_EXTERNAL_IMAGE_FORMAT_INFO 1000071000
#define ST_EXTERNAL_IMAGE_FORMAT_PROPERTIES 1000071001  /* VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES */
#define DRM_FORMAT_MOD_LINEAR 0ULL

/* DRM_FORMAT modifiers we claim to support. LINEAR is universal. AFBC (ARM
 * framebuffer compression) modifiers are what Maleoon/KWin actually uses for
 * hardware overlays; include the common variants so ANGLE finds a match. */
static const uint64_t FAKE_MODS[] = {
    0ULL,                    /* LINEAR */
    0x0900000000000002ULL,   /* KWin XR24/AR24 accepted modifier (from gbm-hw-shim) */
    0x0900000000000000ULL,   /* variant */
    0x0900000000000001ULL,   /* variant */
    0x0100000000000001ULL,   /* AFBC x8 */
    0x0100000000000002ULL,   /* AFBC x16 */
    0x0100000000000004ULL,   /* AFBC x32 */
    0x0100000000000008ULL,   /* AFBC x64 */
    0x0200000000000001ULL,   /* AFBC tiled x8 */
    0x0200000000000002ULL,   /* AFBC tiled x16 */
    0x0c00000000000002ULL,   /* AFBC split/other */
};
#define N_FAKE_MODS (sizeof(FAKE_MODS)/sizeof(FAKE_MODS[0]))
/* ICD_MOD_SET: which modifiers to advertise. linear=only DRM_FORMAT_MOD_LINEAR;
 * kwin=only 0x0900000000000002; all(default)=the full FAKE_MODS list. */
static uint32_t active_mod_set(const uint64_t **out) {
    static int init=0; static int mode=0;  /* 0=all 1=linear 2=kwin */
    if (!init) {
        const char *e=getenv("ICD_MOD_SET");
        if (e && !strcmp(e,"linear")) mode=1;
        else if (e && !strcmp(e,"kwin")) mode=2;
        init=1;
    }
    static const uint64_t linear_only[] = { 0ULL };
    static const uint64_t kwin_only[] = { 0x0900000000000002ULL };
    if (mode==1) { *out=linear_only; return 1; }
    if (mode==2) { *out=kwin_only; return 1; }
    *out=FAKE_MODS; return N_FAKE_MODS;
}

/* DRM_FORMAT_MOD_* plane count reported to ANGLE. Must equal the EGL dma-buf
 * plane count (ANGLE checks equality at initWithFormat:445). KWin's
 * 0x0900000000000002 AFBC buffer may be multi-plane; tune with ICD_MOD_PLANES. */
static uint32_t mod_plane_count(void) {
    static int init=0; static uint32_t n=2;   /* KWin AFBC buffer is 2-plane */
    if (!init) { const char *e=getenv("ICD_MOD_PLANES"); if(e){uint32_t v=(uint32_t)strtoul(e,NULL,0); if(v>=1&&v<=4) n=v;} init=1; }
    return n;
}

static VkFormatFeatureFlags dma_tiling_features(void) {
    /* ICD_TILING_FEATURES bitmask lets us find which feature Maleoon chokes on
     * when submitting an AFBC external image. Default = all (old behaviour).
     * bit0=SAMPLED bit1=FILTER_LINEAR bit2=COLOR_ATTACHMENT bit3=TRANSFER_SRC
     * bit4=TRANSFER_DST. Try ICD_TILING_FEATURES=3 (sampled+filter, no render). */
    static int init=0; static VkFormatFeatureFlags mask=0;
    if (!init) {
        const char *e=getenv("ICD_TILING_FEATURES");
        if (e) mask=(VkFormatFeatureFlags)strtoul(e,NULL,0); else mask=0xFFFFFFFFu;
        init=1;
    }
    VkFormatFeatureFlags f=0;
    if (mask & 1) f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
    if (mask & 2) f |= VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
    if (mask & 4) f |= VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
    if (mask & 8) f |= VK_FORMAT_FEATURE_TRANSFER_SRC_BIT;
    if (mask & 16) f |= VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
    return f;
}

static void VKAPI_PTR w_get_fmtprops2(VkPhysicalDevice dev,
        VkFormat format, VkFormatProperties2 *out) {
    if (!r_get_fmtprops2) return;
    r_get_fmtprops2(dev, format, out);
    if (!inject_modifier()) return;  /* modifier forge off by default */
    if (!out) return;
    /* Walk pNext, fill any DRM modifier properties list. */
    VkBaseInStructure *p = (VkBaseInStructure *)out->pNext;
    while (p) {
        if ((int)p->sType == ST_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT) {
            /* VkDrmFormatModifierPropertiesListEXT layout (LP64):
             *   sType @0, pNext @8, drmFormatModifierCount @16,
             *   pDrmFormatModifierProperties @24.
             * (Earlier buggy version used @8/@16 -> clobbered pNext -> SIGSEGV.) */
            uint32_t *cnt = (uint32_t *)((char *)p + 16);
            VkDrmFormatModifierPropertiesEXT *props =
                *(VkDrmFormatModifierPropertiesEXT **)((char *)p + 24);
            const uint64_t *mods = NULL;
            uint32_t nmods = active_mod_set(&mods);
            if (!props) {
                *cnt = nmods;   /* query count */
            } else {
                uint32_t n = *cnt < nmods ? *cnt : nmods;
                for (uint32_t i=0;i<n;i++) {
                    props[i].drmFormatModifier = mods[i];
                    props[i].drmFormatModifierPlaneCount = mod_plane_count();
                    props[i].drmFormatModifierTilingFeatures = dma_tiling_features();
                }
                *cnt = n;
            }
            if (getenv("ICD_MODIFIER_TRACE"))
                DBG("FMTMOD format=%d list cnt=%u props=%p\n", (int)format, *cnt, (void*)props);
        }
        p = (VkBaseInStructure *)p->pNext;
    }
}

static VkResult VKAPI_PTR w_get_imgfmtprops2(VkPhysicalDevice dev,
        const VkPhysicalDeviceImageFormatInfo2 *info,
        VkImageFormatProperties2 *out) {
    if (!r_get_imgfmtprops2) return VK_ERROR_INITIALIZATION_FAILED;
    /* Try the real ICD first, but with the DRM modifier info node temporarily
     * hidden so the ICD doesn't reject the unknown sType outright. */
    VkBaseInStructure *in = (VkBaseInStructure *)info->pNext;
    VkBaseInStructure *mod_info = NULL, *saved_next = NULL;
    while (in) {
        if ((int)in->sType == ST_PHYSICAL_DEVICE_IMAGE_DRM_FORMAT_MODIFIER_INFO_EXT) {
            mod_info = in; break;
        }
        in = (VkBaseInStructure *)in->pNext;
    }
    VkPhysicalDeviceImageFormatInfo2 copy = *info;
    if (mod_info) {
        /* find and bypass mod_info in a local copy of the chain is complex;
         * instead call real directly (Maleoon tends to ignore unknown pNext). */
        if (getenv("ICD_MODIFIER_TRACE"))
            DBG("IMGFMT fmt=%d tiling=%d hasModifierInfo=1\n",(int)info->format,(int)info->tiling);
    }
    VkResult r = r_get_imgfmtprops2(dev, info, out);
    if (!inject_modifier()) return r;  /* modifier forge off by default */
    /* For DRM-modifier dma-buf queries, force acceptable properties. Maleoon's
     * ICD either returns FORMAT_NOT_SUPPORTED, or returns success but without
     * the IMPORTABLE bit -- both make ANGLE fail at initWithFormat:515
     * (isMemoryImportable). Overwrite unconditionally for modifier queries. */
    if (mod_info) {
        if (out) {
            out->imageFormatProperties.maxExtent.width = 8192;
            out->imageFormatProperties.maxExtent.height = 8192;
            out->imageFormatProperties.maxExtent.depth = 1;
            out->imageFormatProperties.maxMipLevels = 1;
            out->imageFormatProperties.maxArrayLayers = 1;
            out->imageFormatProperties.sampleCounts = VK_SAMPLE_COUNT_1_BIT;
            out->imageFormatProperties.maxResourceSize = 128*1024*1024;
            VkBaseInStructure *op = (VkBaseInStructure *)out->pNext;
            while (op) {
                if ((int)op->sType == ST_EXTERNAL_IMAGE_FORMAT_PROPERTIES) {
                    VkExternalImageFormatProperties *eip = (VkExternalImageFormatProperties *)op;
                    eip->externalMemoryProperties.externalMemoryFeatures =
                        VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT |
                        VK_EXTERNAL_MEMORY_FEATURE_DEDICATED_ONLY_BIT;
                    eip->externalMemoryProperties.compatibleHandleTypes =
                        VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
                }
                op = (VkBaseInStructure *)op->pNext;
            }
        }
        if (getenv("ICD_MODIFIER_TRACE"))
            DBG("IMGFMT FORCED importable (real r=%d)\n", (int)r);
        r = VK_SUCCESS;
    }
    return r;
}

/* ---------------- device-level proc addr wrapper ----------------
 * CRITICAL (2026-11-21): with ICD interface version 3, the Vulkan loader gets
 * ALL device-level functions (vkQueuePresentKHR, vkCreateSwapchainKHR,
 * vkGetSwapchainImagesKHR, vkGetMemoryFdKHR, ...) through the ICD's
 * vkGetDeviceProcAddr -- NOT vk_icdGetInstanceProcAddr. Our gipa trace proved
 * the loader asks gipa for 'vkGetDeviceProcAddr' exactly once, then never asks
 * gipa for any present/swapchain function again. Returning the REAL
 * vkGetDeviceProcAddr let every device call bypass our hooks -- the reason the
 * earlier present hook logged zero hits. Wrapping it here gives a hook point
 * for every device-level entry point. */
static PFN_vkVoidFunction VKAPI_PTR w_get_device_proc_addr(VkDevice device,
        const char *name) {
    if (!r_get_device_proc_addr || !name) return NULL;
    PFN_vkVoidFunction fn = r_get_device_proc_addr(device, name);
    if (getenv("ICD_DEVPROC_TRACE")) {
        static unsigned long cnt=0;
        if ((cnt++) < 400)
            DBG("DEVPROC req '%s' real=%s\n", name, fn?"yes":"no");
    }
    /* WSI present: real gipa returns NULL but the loader needs a non-NULL
     * present function. Hand back our wrapper (which dlsym-calls the real
     * global symbol) so we can hook/phase-lock/export the present frame. */
    if (!strcmp(name, "vkQueuePresentKHR")) {
        gipa_log(name, 1);
        return (PFN_vkVoidFunction)&w_present;
    }
    if (!fn) return fn;
    if (!strcmp(name, "vkGetMemoryFdKHR"))        { gipa_log(name, 1); return (PFN_vkVoidFunction)&w_get_memory_fd; }
    if (!strcmp(name, "vkCreateImage"))           { return (PFN_vkVoidFunction)&w_create_image; }
    if (!strcmp(name, "vkAllocateMemory"))        { return (PFN_vkVoidFunction)&w_allocate_memory; }
    return fn;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL
vk_icdGetInstanceProcAddr(VkInstance instance, const char *name) {
    load_real();
    if (!real_gipa || !name) return NULL;
    /* Wrap vkGetDeviceProcAddr itself so all device-level calls route via us.
     * The loader requests this through gipa (confirmed in trace). */
    if (!strcmp(name, "vkGetDeviceProcAddr") && r_get_device_proc_addr)
        return (PFN_vkVoidFunction)&w_get_device_proc_addr;
    PFN_vkVoidFunction fn = real_gipa(instance, name);
    if (!fn) { gipa_log(name, 0); return fn; }
    if (!strcmp(name, "vkGetPhysicalDeviceProperties"))  return (PFN_vkVoidFunction)&w_get_props;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2")) return (PFN_vkVoidFunction)&w_get_props2;
    if (!strcmp(name, "vkGetPhysicalDeviceProperties2KHR")) return (PFN_vkVoidFunction)&w_get_props2;
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2"))  { gipa_log(name,1); return (PFN_vkVoidFunction)&w_get_fmtprops2; }
    if (!strcmp(name, "vkGetPhysicalDeviceFormatProperties2KHR")){ gipa_log(name,1); return (PFN_vkVoidFunction)&w_get_fmtprops2; }
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2"))  { gipa_log(name,1); return (PFN_vkVoidFunction)&w_get_imgfmtprops2; }
    if (!strcmp(name, "vkGetPhysicalDeviceImageFormatProperties2KHR")){ gipa_log(name,1); return (PFN_vkVoidFunction)&w_get_imgfmtprops2; }
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures"))   return (PFN_vkVoidFunction)&w_get_features;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2"))  return (PFN_vkVoidFunction)&w_get_features2;
    if (!strcmp(name, "vkGetPhysicalDeviceFeatures2KHR"))return (PFN_vkVoidFunction)&w_get_features2khr;
    if (!strcmp(name, "vkCreateDevice"))                 { gipa_log(name, 1); return (PFN_vkVoidFunction)&w_create_device; }
    if (!strcmp(name, "vkEnumerateDeviceExtensionProperties")) { gipa_log(name, 1); return (PFN_vkVoidFunction)&w_enum_dev_ext; }
    if (!strcmp(name, "vkQueuePresentKHR"))              { gipa_log(name, 1); return (PFN_vkVoidFunction)&w_present; }
    /* ANGLE resolves core device functions through its OWN vk::Functions table
     * populated via instance gipa (not the loader's vkGetDeviceProcAddr). Hook
     * them here too so the dma-buf import path is visible when ANGLE is the
     * caller. Forward to symbol-resolved real ICD functions (never gipa) to
     * avoid re-entrancy. */
    if (!strcmp(name, "vkCreateImage"))                  { gipa_log(name, 1); return (PFN_vkVoidFunction)&w_create_image; }
    if (!strcmp(name, "vkAllocateMemory"))               { return (PFN_vkVoidFunction)&w_allocate_memory; }
    if (!strcmp(name, "vkGetImageMemoryRequirements2"))  { return fn; }
    if (!strcmp(name, "vkCreateWaylandSurfaceKHR"))      { gipa_log(name,1); return (PFN_vkVoidFunction)&w_create_wl_surface; }
    if (!strcmp(name, "vkGetPhysicalDeviceWaylandPresentationSupportKHR")) { gipa_log(name,1); return (PFN_vkVoidFunction)&w_wl_pres_support; }
    gipa_log(name, 0);
    return fn;
}

/* 2026-11-21: Export vkQueuePresentKHR as a GLOBAL symbol. The Vulkan loader
 * resolves WSI functions by dlsym()ing them directly from the ICD .so (not via
 * vkGetDeviceProcAddr -- libhvgr's gipa returns NULL for present). Since our
 * .so is the ICD named in the manifest, exporting this symbol makes the loader
 * call OUR function, giving us a present hook. We forward to the real global
 * symbol resolved from libhvgr at load time. */
VKAPI_ATTR VkResult VKAPI_CALL
vkQueuePresentKHR(VkQueue q, const VkPresentInfoKHR *info) {
    return w_present(q, info);
}

VKAPI_ATTR VkResult VKAPI_CALL
vk_icdNegotiateLoaderICDInterfaceVersion(uint32_t *pSupportedVersion) {
    load_real();
    typedef VkResult (VKAPI_PTR *neg_fn)(uint32_t *);
    static neg_fn real_neg = NULL;
    if (!real_neg && g_icd)
        real_neg = (neg_fn)dlsym(g_icd, "vk_icdNegotiateLoaderICDInterfaceVersion");
    if (real_neg) {
        VkResult nr = real_neg(pSupportedVersion);
        DBG("NEGOTIATE interface version -> *pSupportedVersion=%u (req=%u) r=%d\n",
            pSupportedVersion?*pSupportedVersion:0, pSupportedVersion?*pSupportedVersion:0, (int)nr);
        return nr;
    }
    /* fallback: accept interface version 5 */
    if (pSupportedVersion && *pSupportedVersion > 5) *pSupportedVersion = 5;
    return VK_SUCCESS;
}
