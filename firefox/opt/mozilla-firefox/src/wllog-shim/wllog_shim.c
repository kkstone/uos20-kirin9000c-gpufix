#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>

/*
 * wllog_shim.so - filter benign Huawei libGFX Wayland lifecycle warnings.
 *
 * The Huawei libGFX_hisi destroys a wl_event_queue while some of its own
 * proxies (wl_callback / maleoon_buffer_sharing) are still attached, and
 * later compositor events hit that already-destroyed queue. libwayland logs
 * both cases via wl_log():
 *
 *   "queue ... destroyed while proxies still attached:"
 *   "  wl_callback#NN still attached"
 *   "Tried to add event to destroyed queue"
 *
 * GDK / Firefox install a wl_log handler that forwards EVERY message to
 * gfxCriticalError, so these harmless teardown warnings get recorded as
 * GraphicsCriticalError and eventually trigger a deliberate MOZ_CRASH
 * (SIGSEGV with pc in libpthread raise, stack libxul -> libwayland -> GDK).
 *
 * We interpose wl_log_set_handler_client(), wrap the caller-supplied handler,
 * and drop only this specific queue-lifecycle family. All other messages are
 * passed through verbatim, so real protocol errors are still reported.
 *
 */
typedef void (*wl_log_handler_t)(const char *fmt, va_list args);
typedef void (*set_handler_t)(wl_log_handler_t);
typedef void (*destroy_queue_t)(void *queue);

static wl_log_handler_t g_real_handler = NULL;

static int is_benign_queue_warning(const char *fmt) {
    if (!fmt) return 0;
    /* Queue destroyed while Huawei's own proxies are still attached. */
    if (strstr(fmt, "destroyed while proxies still attached")) return 1;
    if (strstr(fmt, "still attached")) return 1;
    if (strstr(fmt, "queue destroyed")) return 1;
    /* Compositor event delivered to a queue Huawei already destroyed. */
    if (strstr(fmt, "destroyed queue")) return 1;
    if (strstr(fmt, "Tried to add event")) return 1;
    return 0;
}

static void filter_handler(const char *fmt, va_list args) {
    if (!fmt) return;
    if (is_benign_queue_warning(fmt)) return;
    if (g_real_handler) g_real_handler(fmt, args);
}

/*
 * Huawei libGFX tears down a wl_event_queue that still has live proxies.
 * Destroying it is what produces both "still attached" warnings and later
 * "Tried to add event to destroyed queue" (events get dropped -> wheels and
 * frames can stall). Leaking that one queue at process teardown is harmless;
 * GDK/Firefox queues are left untouched. Caller is checked by module so only
 * libGFX_hisi's own call is ignored.
 */
void wl_event_queue_destroy(void *queue) {
    static destroy_queue_t real_destroy = NULL;
    if (!real_destroy)
        real_destroy = (destroy_queue_t)dlsym(RTLD_NEXT, "wl_event_queue_destroy");
    void *caller = __builtin_return_address(0);
    Dl_info info;
    if (dladdr(caller, &info) && info.dli_fname &&
        strstr(info.dli_fname, "libGFX_hisi")) {
        return;
    }
    if (real_destroy) real_destroy(queue);
}

void wl_log_set_handler_client(wl_log_handler_t handler) {
    static set_handler_t real_set = NULL;
    if (!real_set) {
        real_set = (set_handler_t)dlsym(RTLD_NEXT, "wl_log_set_handler_client");
    }
    /* Never wrap our own filter (idempotent). */
    if (handler == filter_handler) {
        if (real_set) real_set(handler);
        return;
    }
    g_real_handler = handler;
    if (real_set) real_set(filter_handler);
}
