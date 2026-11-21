#define _GNU_SOURCE
/*
 * egl_huawei_shim.c - libEGL.so.1 interposer
 *
 * Exports the complete EGL symbol set that Firefox's GLLibraryEGL resolves
 * at runtime (PR_FindFunctionSymbol), but forwards every call to the
 * HUAWEI vendor EGL (libGFX_hisi), which exposes EGL_EXT_image_dma_buf_import
 * needed by VAAPI DMABUF -> GL texture import in the RDD headless context.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <string.h>

#define HW_EGL "/usr/lib/aarch64-linux-gnu/libGFX_hisi.so.0.8.0"
#define MESA_EGL "/usr/lib/deepin-shared-libs/libegl1/libEGL.so.1.1.0"
static void *hw = NULL;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
EGLDisplay eglGetDisplay(EGLNativeDisplayType d);  /* forward decl */
static int use_huawei = 0;   /* 1 = RDD process (HUAWEI), 0 = others (Mesa zink) */
/* RDD-only: HUAWEI EGL cannot use EGL_DEFAULT_DISPLAY (returns nil).
 * The only working display is eglGetPlatformDisplayEXT(WAYLAND_KHR=0x31D8, wl_display).
 * These are initialised ONLY in the RDD branch; GPU/Mesa path is untouched. */
static void *g_wl = NULL;       /* wl_display* connected to wayland-0 */
static void *g_gpd_ext = NULL;  /* real eglGetPlatformDisplayEXT from HUAWEI GPA */

static void hwinit(void){
  if(hw) return;
  pthread_mutex_lock(&lock);
  if(!hw){
    /* All processes use HUAWEI EGL (libGFX_hisi), which exposes
     * EGL_EXT_image_dma_buf_import for VAAPI DMABUF -> GL texture import.
     * A Firefox-only patched copy (HISI_FF_LIBGFX) avoids a teardown UAF. */
    use_huawei = 1;
    const char *path = use_huawei ? HW_EGL : MESA_EGL;
    const char *gfx_override = getenv("HISI_FF_LIBGFX");
    if(use_huawei && gfx_override && gfx_override[0]) path = gfx_override;
    /* RTLD_LOCAL keeps HUAWEI's generic-named symbols out of the global
     * namespace so they do not collide with libva/vacore/ffmpeg VAAPI probing. */
    int flags = use_huawei ? (RTLD_NOW | RTLD_LOCAL) : (RTLD_NOW | RTLD_GLOBAL);
    hw = dlopen(path, flags);
    if(!hw){ hw = dlopen(use_huawei?MESA_EGL:HW_EGL, RTLD_NOW|RTLD_LOCAL); }
    if(!hw) fprintf(stderr,"[egl-hw-shim] ERROR: dlopen failed for backend path=%s (%s)\n",
                    path, dlerror());
    if(use_huawei && hw){
      /* RDD ONLY: build a real HUAWEI display for the headless DMABuf GL context.
       * Do NOT depend on WAYLAND_DISPLAY env (VAAPI launcher unsets it) - connect
       * to the compositor's wayland-0 socket directly. */
      void *wlh = dlopen("libwayland-client.so.0", RTLD_NOW|RTLD_LOCAL);
      void *wc = wlh?dlsym(wlh,"wl_display_connect"):NULL;
      if(wc){
        g_wl = ((void*(*)(const char*))wc)("wayland-0");
        if(!g_wl) g_wl = ((void*(*)(const char*))wc)(NULL); /* env fallback */
      }
      void *gpa = dlsym(hw,"eglGetProcAddress");
      if(gpa) g_gpd_ext = ((void*(*)(const char*))gpa)("eglGetPlatformDisplayEXT");
    }
  }
  pthread_mutex_unlock(&lock);
}

typedef EGLBoolean (*PFN_eglInitialize)(EGLDisplay d, EGLint *a, EGLint *b);
EGLBoolean eglInitialize(EGLDisplay d, EGLint *a, EGLint *b){
  hwinit();
  static PFN_eglInitialize fn=NULL;
  if(!fn) fn=(PFN_eglInitialize)dlsym(hw,"eglInitialize");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglInitialize\n"); return (EGLBoolean)0; }
  return fn(d,a,b);
}

typedef EGLBoolean (*PFN_eglTerminate)(EGLDisplay d);
EGLBoolean eglTerminate(EGLDisplay d){
  hwinit();
  static PFN_eglTerminate fn=NULL;
  if(!fn) fn=(PFN_eglTerminate)dlsym(hw,"eglTerminate");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglTerminate\n"); return (EGLBoolean)0; }
  return fn(d);
}

typedef EGLint (*PFN_eglGetError)(void);
EGLint eglGetError(void){
  hwinit();
  static PFN_eglGetError fn=NULL;
  if(!fn) fn=(PFN_eglGetError)dlsym(hw,"eglGetError");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetError\n"); return (EGLint)0; }
  return fn();
}

typedef const char * (*PFN_eglQueryString)(EGLDisplay d, EGLint name);
static char g_extbuf[16384];
const char * eglQueryString(EGLDisplay d, EGLint name){
  hwinit();
  static PFN_eglQueryString fn=NULL;
  if(!fn) fn=(PFN_eglQueryString)dlsym(hw,"eglQueryString");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglQueryString\n"); return (const char *)0; }
  const char* r=fn(d,name);
  /* HUAWEI EGL lacks EXT_platform_device/EXT_device_enumeration which Firefox
   * GetAndInitDeviceDisplay requires for the headless context in RDD. Declare them. */
  if(use_huawei && name==0x3055 && r){  /* EGL_EXTENSIONS */
    snprintf(g_extbuf,sizeof(g_extbuf),"%s EGL_EXT_platform_device EGL_EXT_device_enumeration EGL_KHR_platform_wayland EGL_EXT_platform_wayland EGL_ANGLE_platform_angle",r);
    return g_extbuf;
  }
  return r;
}

typedef EGLBoolean (*PFN_eglGetConfigs)(EGLDisplay d, EGLConfig *cfg, EGLint n, EGLint *out);
EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *cfg, EGLint n, EGLint *out){
  hwinit();
  static PFN_eglGetConfigs fn=NULL;
  if(!fn) fn=(PFN_eglGetConfigs)dlsym(hw,"eglGetConfigs");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetConfigs\n"); return (EGLBoolean)0; }
  return fn(d,cfg,n,out);
}

typedef EGLBoolean (*PFN_eglChooseConfig)(EGLDisplay d, const EGLint *attrib, EGLConfig *cfg, EGLint n, EGLint *out);
EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attrib, EGLConfig *cfg, EGLint n, EGLint *out){
  hwinit();
  static PFN_eglChooseConfig fn=NULL;
  if(!fn) fn=(PFN_eglChooseConfig)dlsym(hw,"eglChooseConfig");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglChooseConfig\n"); return (EGLBoolean)0; }
  EGLBoolean r=fn(d,attrib,cfg,n,out);
  return r;
}

typedef EGLBoolean (*PFN_eglGetConfigAttrib)(EGLDisplay d, EGLConfig cfg, EGLint attr, EGLint *val);
EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig cfg, EGLint attr, EGLint *val){
  hwinit();
  static PFN_eglGetConfigAttrib fn=NULL;
  if(!fn) fn=(PFN_eglGetConfigAttrib)dlsym(hw,"eglGetConfigAttrib");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetConfigAttrib\n"); return (EGLBoolean)0; }
  return fn(d,cfg,attr,val);
}

typedef EGLSurface (*PFN_eglCreateWindowSurface)(EGLDisplay d, EGLConfig cfg, EGLNativeWindowType w, const EGLint *attrib);
EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig cfg, EGLNativeWindowType w, const EGLint *attrib){
  hwinit();
  static PFN_eglCreateWindowSurface fn=NULL;
  if(!fn) fn=(PFN_eglCreateWindowSurface)dlsym(hw,"eglCreateWindowSurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreateWindowSurface\n"); return (EGLSurface)0; }
  return fn(d,cfg,w,attrib);
}

typedef EGLSurface (*PFN_eglCreatePbufferSurface)(EGLDisplay d, EGLConfig cfg, const EGLint *attrib);
EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig cfg, const EGLint *attrib){
  hwinit();
  static PFN_eglCreatePbufferSurface fn=NULL;
  if(!fn) fn=(PFN_eglCreatePbufferSurface)dlsym(hw,"eglCreatePbufferSurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreatePbufferSurface\n"); return (EGLSurface)0; }
  return fn(d,cfg,attrib);
}

typedef EGLSurface (*PFN_eglCreatePixmapSurface)(EGLDisplay d, EGLConfig cfg, EGLNativePixmapType p, const EGLint *attrib);
EGLSurface eglCreatePixmapSurface(EGLDisplay d, EGLConfig cfg, EGLNativePixmapType p, const EGLint *attrib){
  hwinit();
  static PFN_eglCreatePixmapSurface fn=NULL;
  if(!fn) fn=(PFN_eglCreatePixmapSurface)dlsym(hw,"eglCreatePixmapSurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreatePixmapSurface\n"); return (EGLSurface)0; }
  return fn(d,cfg,p,attrib);
}

typedef EGLBoolean (*PFN_eglDestroySurface)(EGLDisplay d, EGLSurface s);
EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s){
  hwinit();
  static PFN_eglDestroySurface fn=NULL;
  if(!fn) fn=(PFN_eglDestroySurface)dlsym(hw,"eglDestroySurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglDestroySurface\n"); return (EGLBoolean)0; }
  return fn(d,s);
}

typedef EGLBoolean (*PFN_eglQuerySurface)(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val);
EGLBoolean eglQuerySurface(EGLDisplay d, EGLSurface s, EGLint attr, EGLint *val){
  hwinit();
  static PFN_eglQuerySurface fn=NULL;
  if(!fn) fn=(PFN_eglQuerySurface)dlsym(hw,"eglQuerySurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglQuerySurface\n"); return (EGLBoolean)0; }
  return fn(d,s,attr,val);
}

typedef EGLBoolean (*PFN_eglBindAPI)(EGLenum api);
EGLBoolean eglBindAPI(EGLenum api){
  hwinit();
  static PFN_eglBindAPI fn=NULL;
  if(!fn) fn=(PFN_eglBindAPI)dlsym(hw,"eglBindAPI");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglBindAPI\n"); return (EGLBoolean)0; }
  return fn(api);
}

typedef EGLenum (*PFN_eglQueryAPI)(void);
EGLenum eglQueryAPI(void){
  hwinit();
  static PFN_eglQueryAPI fn=NULL;
  if(!fn) fn=(PFN_eglQueryAPI)dlsym(hw,"eglQueryAPI");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglQueryAPI\n"); return (EGLenum)0; }
  return fn();
}

typedef EGLBoolean (*PFN_eglWaitClient)(void);
EGLBoolean eglWaitClient(void){
  hwinit();
  static PFN_eglWaitClient fn=NULL;
  if(!fn) fn=(PFN_eglWaitClient)dlsym(hw,"eglWaitClient");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglWaitClient\n"); return (EGLBoolean)0; }
  return fn();
}

typedef EGLBoolean (*PFN_eglReleaseThread)(void);
EGLBoolean eglReleaseThread(void){
  hwinit();
  static PFN_eglReleaseThread fn=NULL;
  if(!fn) fn=(PFN_eglReleaseThread)dlsym(hw,"eglReleaseThread");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglReleaseThread\n"); return (EGLBoolean)0; }
  return fn();
}

typedef EGLSurface (*PFN_eglCreatePbufferFromClientBuffer)(EGLDisplay d, EGLenum bt, EGLClientBuffer b, EGLConfig cfg, const EGLint *attrib);
EGLSurface eglCreatePbufferFromClientBuffer(EGLDisplay d, EGLenum bt, EGLClientBuffer b, EGLConfig cfg, const EGLint *attrib){
  hwinit();
  static PFN_eglCreatePbufferFromClientBuffer fn=NULL;
  if(!fn) fn=(PFN_eglCreatePbufferFromClientBuffer)dlsym(hw,"eglCreatePbufferFromClientBuffer");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreatePbufferFromClientBuffer\n"); return (EGLSurface)0; }
  return fn(d,bt,b,cfg,attrib);
}

typedef EGLBoolean (*PFN_eglSurfaceAttrib)(EGLDisplay d, EGLSurface s, EGLint attr, EGLint val);
EGLBoolean eglSurfaceAttrib(EGLDisplay d, EGLSurface s, EGLint attr, EGLint val){
  hwinit();
  static PFN_eglSurfaceAttrib fn=NULL;
  if(!fn) fn=(PFN_eglSurfaceAttrib)dlsym(hw,"eglSurfaceAttrib");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglSurfaceAttrib\n"); return (EGLBoolean)0; }
  return fn(d,s,attr,val);
}

typedef EGLBoolean (*PFN_eglBindTexImage)(EGLDisplay d, EGLSurface s, EGLint buf);
EGLBoolean eglBindTexImage(EGLDisplay d, EGLSurface s, EGLint buf){
  hwinit();
  static PFN_eglBindTexImage fn=NULL;
  if(!fn) fn=(PFN_eglBindTexImage)dlsym(hw,"eglBindTexImage");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglBindTexImage\n"); return (EGLBoolean)0; }
  return fn(d,s,buf);
}

typedef EGLBoolean (*PFN_eglReleaseTexImage)(EGLDisplay d, EGLSurface s, EGLint buf);
EGLBoolean eglReleaseTexImage(EGLDisplay d, EGLSurface s, EGLint buf){
  hwinit();
  static PFN_eglReleaseTexImage fn=NULL;
  if(!fn) fn=(PFN_eglReleaseTexImage)dlsym(hw,"eglReleaseTexImage");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglReleaseTexImage\n"); return (EGLBoolean)0; }
  return fn(d,s,buf);
}

typedef EGLBoolean (*PFN_eglSwapInterval)(EGLDisplay d, EGLint iv);
EGLBoolean eglSwapInterval(EGLDisplay d, EGLint iv){
  hwinit();
  static PFN_eglSwapInterval fn=NULL;
  if(!fn) fn=(PFN_eglSwapInterval)dlsym(hw,"eglSwapInterval");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglSwapInterval\n"); return (EGLBoolean)0; }
  return fn(d,iv);
}

typedef EGLContext (*PFN_eglCreateContext)(EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attrib);
EGLContext eglCreateContext(EGLDisplay d, EGLConfig cfg, EGLContext share, const EGLint *attrib){
  hwinit();
  static PFN_eglCreateContext fn=NULL;
  if(!fn) fn=(PFN_eglCreateContext)dlsym(hw,"eglCreateContext");
  if(!use_huawei) return fn?fn(d,cfg,share,attrib):0;  /* Mesa: pure passthrough, no error-state pollution */
  /* HUAWEI EGL rejects EGL_CONTEXT_FLAGS_KHR(0x31bd)/RELEASE_BEHAVIOR(0x30fc)
   * with EGL_BAD_CONFIG(0x3004). Retry with a minimal attrib set (client version only). */
  EGLContext r=fn?fn(d,cfg,share,attrib):0;
  if(fn) eglGetError();  /* clear pending error before retry */
  if(!r && attrib){  /* retry on ANY failure (0x3004/0x300c/...) */
    EGLint minimal[8]; int m=0;
    for(int i=0; attrib[i]!=0x3038 && i<24 && m<6; i+=2){
      if(attrib[i]==0x3098){ minimal[m++]=attrib[i]; minimal[m++]=attrib[i+1]; }
    }
    if(m==0){ minimal[m++]=0x3098; minimal[m++]=3; }  /* ensure a client version */
    minimal[m++]=0x3038;
    r=fn(d,cfg,share,minimal);
    eglGetError();
  }
  return r;
}

typedef EGLBoolean (*PFN_eglDestroyContext)(EGLDisplay d, EGLContext c);
EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c){
  hwinit();
  static PFN_eglDestroyContext fn=NULL;
  if(!fn) fn=(PFN_eglDestroyContext)dlsym(hw,"eglDestroyContext");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglDestroyContext\n"); return (EGLBoolean)0; }
  return fn(d,c);
}

typedef EGLBoolean (*PFN_eglMakeCurrent)(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c);
EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface draw, EGLSurface read, EGLContext c){
  hwinit();
  static PFN_eglMakeCurrent fn=NULL;
  if(!fn) fn=(PFN_eglMakeCurrent)dlsym(hw,"eglMakeCurrent");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglMakeCurrent\n"); return (EGLBoolean)0; }
  return fn(d,draw,read,c);
}

typedef EGLContext (*PFN_eglGetCurrentContext)(void);
EGLContext eglGetCurrentContext(void){
  hwinit();
  static PFN_eglGetCurrentContext fn=NULL;
  if(!fn) fn=(PFN_eglGetCurrentContext)dlsym(hw,"eglGetCurrentContext");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetCurrentContext\n"); return (EGLContext)0; }
  return fn();
}

typedef EGLSurface (*PFN_eglGetCurrentSurface)(EGLint which);
EGLSurface eglGetCurrentSurface(EGLint which){
  hwinit();
  static PFN_eglGetCurrentSurface fn=NULL;
  if(!fn) fn=(PFN_eglGetCurrentSurface)dlsym(hw,"eglGetCurrentSurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetCurrentSurface\n"); return (EGLSurface)0; }
  return fn(which);
}

typedef EGLDisplay (*PFN_eglGetCurrentDisplay)(void);
EGLDisplay eglGetCurrentDisplay(void){
  hwinit();
  static PFN_eglGetCurrentDisplay fn=NULL;
  if(!fn) fn=(PFN_eglGetCurrentDisplay)dlsym(hw,"eglGetCurrentDisplay");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetCurrentDisplay\n"); return (EGLDisplay)0; }
  return fn();
}

typedef EGLBoolean (*PFN_eglQueryContext)(EGLDisplay d, EGLContext c, EGLint attr, EGLint *val);
EGLBoolean eglQueryContext(EGLDisplay d, EGLContext c, EGLint attr, EGLint *val){
  hwinit();
  static PFN_eglQueryContext fn=NULL;
  if(!fn) fn=(PFN_eglQueryContext)dlsym(hw,"eglQueryContext");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglQueryContext\n"); return (EGLBoolean)0; }
  return fn(d,c,attr,val);
}

typedef EGLBoolean (*PFN_eglWaitGL)(void);
EGLBoolean eglWaitGL(void){
  hwinit();
  static PFN_eglWaitGL fn=NULL;
  if(!fn) fn=(PFN_eglWaitGL)dlsym(hw,"eglWaitGL");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglWaitGL\n"); return (EGLBoolean)0; }
  return fn();
}

typedef EGLBoolean (*PFN_eglWaitNative)(EGLint eng);
EGLBoolean eglWaitNative(EGLint eng){
  hwinit();
  static PFN_eglWaitNative fn=NULL;
  if(!fn) fn=(PFN_eglWaitNative)dlsym(hw,"eglWaitNative");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglWaitNative\n"); return (EGLBoolean)0; }
  return fn(eng);
}

typedef EGLBoolean (*PFN_eglSwapBuffers)(EGLDisplay d, EGLSurface s);
EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s){
  hwinit();
  static PFN_eglSwapBuffers fn=NULL;
  if(!fn) fn=(PFN_eglSwapBuffers)dlsym(hw,"eglSwapBuffers");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglSwapBuffers\n"); return (EGLBoolean)0; }
  return fn(d,s);
}

typedef EGLBoolean (*PFN_eglCopyBuffers)(EGLDisplay d, EGLSurface s, EGLNativePixmapType p);
EGLBoolean eglCopyBuffers(EGLDisplay d, EGLSurface s, EGLNativePixmapType p){
  hwinit();
  static PFN_eglCopyBuffers fn=NULL;
  if(!fn) fn=(PFN_eglCopyBuffers)dlsym(hw,"eglCopyBuffers");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCopyBuffers\n"); return (EGLBoolean)0; }
  return fn(d,s,p);
}

typedef EGLImageKHR (*PFN_eglCreateImageKHR)(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer buf, const EGLint *attrib);
EGLImageKHR eglCreateImageKHR(EGLDisplay d, EGLContext c, EGLenum target, EGLClientBuffer buf, const EGLint *attrib){
  hwinit();
  static PFN_eglCreateImageKHR fn=NULL;
  if(!fn) fn=(PFN_eglCreateImageKHR)dlsym(hw,"eglCreateImageKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreateImageKHR\n"); return (EGLImageKHR)0; }
  return fn(d,c,target,buf,attrib);
}

typedef EGLBoolean (*PFN_eglDestroyImageKHR)(EGLDisplay d, EGLImageKHR img);
EGLBoolean eglDestroyImageKHR(EGLDisplay d, EGLImageKHR img){
  hwinit();
  static PFN_eglDestroyImageKHR fn=NULL;
  if(!fn) fn=(PFN_eglDestroyImageKHR)dlsym(hw,"eglDestroyImageKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglDestroyImageKHR\n"); return (EGLBoolean)0; }
  return fn(d,img);
}

typedef EGLSyncKHR (*PFN_eglCreateSyncKHR)(EGLDisplay d, EGLenum type, const EGLint *attrib);
EGLSyncKHR eglCreateSyncKHR(EGLDisplay d, EGLenum type, const EGLint *attrib){
  hwinit();
  static PFN_eglCreateSyncKHR fn=NULL;
  if(!fn) fn=(PFN_eglCreateSyncKHR)dlsym(hw,"eglCreateSyncKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreateSyncKHR\n"); return (EGLSyncKHR)0; }
  return fn(d,type,attrib);
}

typedef EGLBoolean (*PFN_eglDestroySyncKHR)(EGLDisplay d, EGLSyncKHR s);
EGLBoolean eglDestroySyncKHR(EGLDisplay d, EGLSyncKHR s){
  hwinit();
  static PFN_eglDestroySyncKHR fn=NULL;
  if(!fn) fn=(PFN_eglDestroySyncKHR)dlsym(hw,"eglDestroySyncKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglDestroySyncKHR\n"); return (EGLBoolean)0; }
  return fn(d,s);
}

typedef EGLint (*PFN_eglClientWaitSyncKHR)(EGLDisplay d, EGLSyncKHR s, EGLint flags, EGLTimeKHR t);
EGLint eglClientWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint flags, EGLTimeKHR t){
  hwinit();
  static PFN_eglClientWaitSyncKHR fn=NULL;
  if(!fn) fn=(PFN_eglClientWaitSyncKHR)dlsym(hw,"eglClientWaitSyncKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglClientWaitSyncKHR\n"); return (EGLint)0; }
  return fn(d,s,flags,t);
}

typedef EGLBoolean (*PFN_eglGetSyncAttribKHR)(EGLDisplay d, EGLSyncKHR s, EGLint attr, EGLint *val);
EGLBoolean eglGetSyncAttribKHR(EGLDisplay d, EGLSyncKHR s, EGLint attr, EGLint *val){
  hwinit();
  static PFN_eglGetSyncAttribKHR fn=NULL;
  if(!fn) fn=(PFN_eglGetSyncAttribKHR)dlsym(hw,"eglGetSyncAttribKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetSyncAttribKHR\n"); return (EGLBoolean)0; }
  return fn(d,s,attr,val);
}

typedef EGLBoolean (*PFN_eglWaitSyncKHR)(EGLDisplay d, EGLSyncKHR s, EGLint flags);
EGLBoolean eglWaitSyncKHR(EGLDisplay d, EGLSyncKHR s, EGLint flags){
  hwinit();
  static PFN_eglWaitSyncKHR fn=NULL;
  if(!fn) fn=(PFN_eglWaitSyncKHR)dlsym(hw,"eglWaitSyncKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglWaitSyncKHR\n"); return (EGLBoolean)0; }
  return fn(d,s,flags);
}

typedef EGLBoolean (*PFN_eglSetDamageRegionKHR)(EGLDisplay d, EGLSurface s, EGLint *r, EGLint n);
EGLBoolean eglSetDamageRegionKHR(EGLDisplay d, EGLSurface s, EGLint *r, EGLint n){
  hwinit();
  static PFN_eglSetDamageRegionKHR fn=NULL;
  if(!fn) fn=(PFN_eglSetDamageRegionKHR)dlsym(hw,"eglSetDamageRegionKHR");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglSetDamageRegionKHR\n"); return (EGLBoolean)0; }
  return fn(d,s,r,n);
}

typedef EGLBoolean (*PFN_eglQueryDmaBufFormatsEXT)(EGLDisplay d, EGLint max, EGLint *fmt, EGLint *out);
EGLBoolean eglQueryDmaBufFormatsEXT(EGLDisplay d, EGLint max, EGLint *fmt, EGLint *out){
  hwinit();
  static PFN_eglQueryDmaBufFormatsEXT fn=NULL;
  if(!fn) fn=(PFN_eglQueryDmaBufFormatsEXT)dlsym(hw,"eglQueryDmaBufFormatsEXT");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglQueryDmaBufFormatsEXT\n"); return (EGLBoolean)0; }
  return fn(d,max,fmt,out);
}

typedef EGLBoolean (*PFN_eglQueryDmaBufModifiersEXT)(EGLDisplay d, EGLint fmt, EGLint max, EGLuint64KHR *mod, EGLBoolean *ext, EGLint *out);
EGLBoolean eglQueryDmaBufModifiersEXT(EGLDisplay d, EGLint fmt, EGLint max, EGLuint64KHR *mod, EGLBoolean *ext, EGLint *out){
  hwinit();
  static PFN_eglQueryDmaBufModifiersEXT fn=NULL;
  if(!fn) fn=(PFN_eglQueryDmaBufModifiersEXT)dlsym(hw,"eglQueryDmaBufModifiersEXT");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglQueryDmaBufModifiersEXT\n"); return (EGLBoolean)0; }
  return fn(d,fmt,max,mod,ext,out);
}

typedef EGLDisplay (*PFN_eglGetPlatformDisplay)(EGLenum plat, void *nd, const EGLAttrib *attrib);
EGLDisplay eglGetPlatformDisplay(EGLenum plat, void *nd, const EGLAttrib *attrib){
  hwinit();
  static PFN_eglGetPlatformDisplay fn=NULL;
  if(!fn) fn=(PFN_eglGetPlatformDisplay)dlsym(hw,"eglGetPlatformDisplay");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglGetPlatformDisplay\n"); return (EGLDisplay)0; }
  return fn(plat,nd,attrib);
}

typedef EGLSurface (*PFN_eglCreatePlatformWindowSurface)(EGLDisplay d, EGLConfig cfg, void *w, const EGLAttrib *attrib);
EGLSurface eglCreatePlatformWindowSurface(EGLDisplay d, EGLConfig cfg, void *w, const EGLAttrib *attrib){
  hwinit();
  static PFN_eglCreatePlatformWindowSurface fn=NULL;
  if(!fn) fn=(PFN_eglCreatePlatformWindowSurface)dlsym(hw,"eglCreatePlatformWindowSurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreatePlatformWindowSurface\n"); return (EGLSurface)0; }
  return fn(d,cfg,w,attrib);
}

typedef EGLSurface (*PFN_eglCreatePlatformPixmapSurface)(EGLDisplay d, EGLConfig cfg, void *p, const EGLAttrib *attrib);
EGLSurface eglCreatePlatformPixmapSurface(EGLDisplay d, EGLConfig cfg, void *p, const EGLAttrib *attrib){
  hwinit();
  static PFN_eglCreatePlatformPixmapSurface fn=NULL;
  if(!fn) fn=(PFN_eglCreatePlatformPixmapSurface)dlsym(hw,"eglCreatePlatformPixmapSurface");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglCreatePlatformPixmapSurface\n"); return (EGLSurface)0; }
  return fn(d,cfg,p,attrib);
}

typedef EGLBoolean (*PFN_eglWaitSync)(EGLDisplay d, EGLSync s, EGLint flags);
EGLBoolean eglWaitSync(EGLDisplay d, EGLSync s, EGLint flags){
  hwinit();
  static PFN_eglWaitSync fn=NULL;
  if(!fn) fn=(PFN_eglWaitSync)dlsym(hw,"eglWaitSync");
  if(!fn){ fprintf(stderr,"[egl-hw-shim] ERROR MISSING eglWaitSync\n"); return (EGLBoolean)0; }
  return fn(d,s,flags);
}

typedef __eglMustCastToProperFunctionPointerType (*PFN_GPA)(const char*);
typedef EGLDisplay (*PFN_FAKEPLAT)(unsigned int, void*, const void*);
static EGLDisplay fake_eglGetPlatformDisplay(unsigned int platform, void* nd, const void* attrib){
  /* Honor Firefox's real native display (the main Wayland connection) when
   * present. HUAWEI EGL binds maleoon_buffer_sharing and creates wl_buffer
   * object IDs on the connection it is given; those IDs must live in the same
   * namespace as the wl_surface the compositor attaches them to. Only the
   * headless RDD snapshot path (EGL_DEFAULT_DISPLAY) falls back to g_wl. */
  void *use_nd = nd;
  if(!use_nd || use_nd == (void*)EGL_DEFAULT_DISPLAY) use_nd = g_wl;
  if(g_gpd_ext){
    EGLDisplay r=((EGLDisplay(*)(unsigned int,void*,const void*))g_gpd_ext)(0x31D8, use_nd, NULL);
    return r;
  }
  return eglGetDisplay((EGLNativeDisplayType)use_nd);
}
/* fake device enumeration: HUAWEI EGL has NO device functions. Firefox's
 * GetAndInitDeviceDisplay() (used by headless context in RDD where gdk_display
 * is NULL) needs these. We expose one synthetic device bound to renderD128. */
static void *g_fake_device = (void*)0x48495731;  /* 'HWI1' sentinel */
#define LOCAL_EGL_DRM_RENDER_NODE_FILE_EXT 0x323B
typedef unsigned int (*PFN_QDEV)(int, void**, int*);
typedef const char* (*PFN_QDEVSTR)(void*, int);
static unsigned int fake_eglQueryDevicesEXT(int max, void** devs, int* num){
  if(num) *num = 1;
  if(devs && max>0) devs[0] = g_fake_device;
  return 1;
}
static const char* fake_eglQueryDeviceStringEXT(void* dev, int name){
  if(name==LOCAL_EGL_DRM_RENDER_NODE_FILE_EXT) return "/dev/dri/renderD128";
  if(name==0x3233) return "/dev/dri/renderD128";  /* DRM_DEVICE_FILE */
  return "";
}
__eglMustCastToProperFunctionPointerType eglGetProcAddress(const char *n){
  hwinit();
  static PFN_GPA fn=NULL;
  if(!fn) fn=(PFN_GPA)dlsym(hw,"eglGetProcAddress");
  void *r = fn?fn(n):NULL;
  /* HUAWEI EGL lacks eglGetPlatformDisplay; Firefox CreateHeadless() needs it
   * to build a surfaceless display. Supply a shim that returns the HUAWEI
   * DEFAULT display regardless of requested platform. */
  if(use_huawei && n){
    /* RDD: some GLLibraryEGL paths resolve eglCreateContext via GPA -> return
     * OUR filtered export (attrib scrubbing) instead of HUAWEI raw function. */
    if(!strcmp(n,"eglCreateContext")){
      return (void*)eglCreateContext;
    }
    if(!strcmp(n,"eglGetPlatformDisplay") || !strcmp(n,"eglGetPlatformDisplayEXT")){
      return (void*)fake_eglGetPlatformDisplay;
    }
    if(!strcmp(n,"eglQueryDevicesEXT")){
      return (void*)fake_eglQueryDevicesEXT;
    }
    if(!strcmp(n,"eglQueryDeviceStringEXT")){
      return (void*)fake_eglQueryDeviceStringEXT;
    }
  }
  return r;
}
typedef EGLDisplay (*PFN_GETDISPLAY)(EGLNativeDisplayType);
EGLDisplay eglGetDisplay(EGLNativeDisplayType d){
  hwinit();
  static PFN_GETDISPLAY fn=NULL;
  if(!fn) fn=(PFN_GETDISPLAY)dlsym(hw,"eglGetDisplay");
  EGLDisplay r;
  if(use_huawei){
    /* Compositor path: use Firefox's main wl_display from `d`. Only the headless
     * RDD path (EGL_DEFAULT_DISPLAY) falls back to g_wl. */
    void *use_nd = (d && d != (EGLNativeDisplayType)EGL_DEFAULT_DISPLAY)
                   ? (void*)d : g_wl;
    r = fake_eglGetPlatformDisplay(0x31D8, use_nd, NULL);
  } else {
    /* Mesa (GPU/content): pass Firefox's original native display unchanged. */
    r = fn?fn(d):0;
  }
  return r;
}
