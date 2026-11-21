#define _GNU_SOURCE
/*
 * egl_fbspoof.c - libEGL.so.1 interposer for Firefox's GL probe CHILD (gfxtest).
 *
 * Goal: make gfxtest report a non-Mesa VERSION and a non-software EGL device,
 * so GfxInfo.cpp does:
 *   mIsMesa = glVersion.Find("Mesa") != -1;   -> false
 *   -> skips the entire block that sets mIsAccelerated=false
 *   -> mIsAccelerated stays true (default from Init)
 *   -> probeHWDecode = mIsAccelerated && (status OK || force)
 *   -> GetDataVAAPI() runs -> VAAPI H264 hardware decode becomes available.
 *
 * Two hooks, both reached through eglGetProcAddress (that's how glxtest.cpp
 * resolves ALL of glGetString / eglQueryDeviceStringEXT):
 *   1) glGetString:
 *        GL_VERSION -> "OpenGL ES 3.2 B250"        (NO "Mesa" token)
 *        GL_VENDOR  -> "HUAWEI"
 *        GL_RENDERER-> "Maleoon 910"
 *      Other enums forwarded to the real driver.
 *   2) eglQueryDeviceStringEXT(EGL_EXTENSIONS):
 *      strip the "EGL_MESA_device_software" token so glxtest.cpp's
 *      strstr(...) misses -> no MESA_ACCELERATED=FALSE record, and it takes
 *      the else branch that captures the DRM render node for VAAPI's -d arg.
 *
 * Everything else is transparently forwarded to the real Mesa libEGL
 * (GLVND dispatcher), so zink context creation / WebRender detection is intact.
 *
 * This shim is installed ONLY into the gfxtest child via the wrapper's
 * LD_LIBRARY_PATH. Firefox's GPU process never loads it.
 *
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>

typedef int EGLint;
typedef unsigned int EGLenum;
typedef unsigned int EGLBoolean;
typedef void *EGLDisplay, *EGLDeviceEXT, *EGLNativeDisplayType;
typedef unsigned char GLubyte;
typedef unsigned int GLenum;

#define GL_VENDOR                   0x1F00
#define GL_RENDERER                 0x1F01
#define GL_VERSION                  0x1F02
#define GL_SHADING_LANGUAGE_VERSION 0x8B8C

#define EGL_EXTENSIONS                 0x3055
#define EGL_DRM_DEVICE_FILE_EXT        0x3233
#define EGL_DRM_RENDER_NODE_FILE_EXT   0x3377

#define DRM_RENDER_NODE "/dev/dri/renderD128"

#define MESA_EGL_PATH "/usr/lib/deepin-shared-libs/libegl1/libEGL.so.1.1.0"

typedef void *(*PFN_GPA)(const char *);
typedef GLubyte *(*PFN_GLGETSTRING)(GLenum);
typedef const char *(*PFN_EGLSDEVSTR)(EGLDeviceEXT, EGLint);

static void *h_mesa = NULL;
static PFN_GPA real_gpa = NULL;
static PFN_GLGETSTRING real_glGetString = NULL;
static PFN_EGLSDEVSTR real_eglQueryDeviceStringEXT = NULL;
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static void init(void) {
    static int done = 0;
    if (done) return;
    pthread_mutex_lock(&g_lock);
    if (!done) {
        h_mesa = dlopen(MESA_EGL_PATH, RTLD_NOW | RTLD_LOCAL);
        if (h_mesa) real_gpa = (PFN_GPA)dlsym(h_mesa, "eglGetProcAddress");
        done = 1;
    }
    pthread_mutex_unlock(&g_lock);
}

/* --- hook 1: fake glGetString ------------------------------------------- */
static GLubyte *fake_glGetString(GLenum name) {
    init();
    if (!real_glGetString && real_gpa)
        real_glGetString = (PFN_GLGETSTRING)real_gpa("glGetString");
    const char *out = NULL;
    switch (name) {
        case GL_VENDOR:                   out = "HUAWEI"; break;
        case GL_RENDERER:                 out = "Maleoon 910"; break;
        case GL_VERSION:                  out = "OpenGL ES 3.2 B250"; break;
        case GL_SHADING_LANGUAGE_VERSION: out = "OpenGL ES GLSL 3.20 B250"; break;
        default: break;
    }
    if (out) return (GLubyte *)out;
    return real_glGetString ? real_glGetString(name) : NULL;
}

/* --- hook 2: strip EGL_MESA_device_software from device extensions ------ */
static char g_extbuf[8192];
static const char *fake_eglQueryDeviceStringEXT(EGLDeviceEXT dev, EGLint name) {
    init();
    if (!real_eglQueryDeviceStringEXT && real_gpa)
        real_eglQueryDeviceStringEXT =
            (PFN_EGLSDEVSTR)real_gpa("eglQueryDeviceStringEXT");
    const char *r = real_eglQueryDeviceStringEXT
                        ? real_eglQueryDeviceStringEXT(dev, name)
                        : NULL;
    /* Maleoon's EGL device reports no DRM node. Feed the render node so
     * glxtest.cpp records DRM_RENDERDEVICE -> gfxVars::DrmRenderDevice, which
     * the RDD needs to open the VAAPI display. */
    if (name == EGL_DRM_RENDER_NODE_FILE_EXT) return DRM_RENDER_NODE;
    if (name == EGL_DRM_DEVICE_FILE_EXT) return DRM_RENDER_NODE;
    if (!r) return r;
    if (name == EGL_EXTENSIONS && strstr(r, "EGL_MESA_device_software")) {
        snprintf(g_extbuf, sizeof(g_extbuf), "%s", r);
        char *p = strstr(g_extbuf, "EGL_MESA_device_software");
        if (p) {
            /* blank out the token, keep surrounding spaces valid */
            memset(p, ' ', strlen("EGL_MESA_device_software"));
        }
        return g_extbuf;
    }
    return r;
}

/* --- the single exported entry glxtest.cpp dlsym()s --------------------- */
void *eglGetProcAddress(const char *name) {
    init();
    if (!name) return real_gpa ? real_gpa(name) : NULL;
    if (!strcmp(name, "glGetString")) return (void *)fake_glGetString;
    if (!strcmp(name, "eglQueryDeviceStringEXT")) return (void *)fake_eglQueryDeviceStringEXT;
    /* everything else: transparent forwarding to real Mesa GLVND */
    return real_gpa ? real_gpa(name) : NULL;
}
