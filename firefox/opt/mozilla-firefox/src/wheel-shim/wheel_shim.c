#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
/* Self-contained Wayland definitions (byte-exact ABI replica of the original
 * Huawei headers). Avoids depending on the trimmed libwayland-dev headers on
 * UOS20 and removes the undefined wl_proxy_get_interface / wl_pointer_interface
 * symbols that crashed Firefox at load time after a system reinstall. */
#include "wayland_mini.h"

/* Local equivalent of wl_proxy_get_interface() - the interface pointer is the
 * first word of every wl_proxy (stable ABI). No external symbol needed. */
static const struct wl_interface *wl_proxy_get_interface_local(struct wl_proxy *p) {
    return p ? p->object.interface : 0;
}
/* Local equivalent of the wl_pointer_interface data symbol: compare the
 * interface name instead of relying on a global symbol export. */
static int is_wl_pointer_interface(const struct wl_interface *iface) {
    return iface && iface->name && strcmp(iface->name, "wl_pointer") == 0;
}

/*
 * wheel_shim.so - make mouse-wheel axis events use Firefox's working
 * "continuous" (touchpad) scroll path.
 *
 * Observed on this machine:
 *   - touchpad two-finger scrolling (wl_pointer.axis_source = CONTINUOUS/FINGER)
 *     works perfectly;
 *   - mouse wheel (axis_source = WHEEL, plus axis_discrete / axis_value120)
 *     produces no DOM wheel at all in Firefox 160 on native Wayland/KWin.
 *
 * This shim wraps wl_pointer_add_listener(). It rewrites the axis_source
 * callback: WHEEL (0) -> CONTINUOUS (1), and drops axis_discrete /
 * axis_value120 events. The floating-point wl_pointer.axis value is still
 * delivered, so Firefox handles the wheel exactly like a touchpad.
 *
 * KWin's wheel axis value is much larger than a touchpad's per-frame
 * continuous delta, so only wheel-originated axis values are scaled (tracked
 * per frame via axis_source); touchpad events are never scaled. The scale is
 * compiled in as WHEEL_SCALE.
 */
typedef int (*add_listener_t)(struct wl_pointer *,
                              const struct wl_pointer_listener *, void *);
typedef int (*proxy_add_listener_t)(struct wl_proxy *, void (**)(void), void *);

struct wheel_ctx {
    const struct wl_pointer_listener *orig;
    void *data;
    /* libwayland keeps the implementation pointer, so the wrapped listener
     * must outlive this call - keep it in the heap-allocated context. */
    struct wl_pointer_listener wrapped;
    int saw_source;
    int is_wheel_frame;   /* current frame originated from a mouse wheel */
};

/* Scale factor applied to wheel-originated axis values only (compiled in). */
#define WHEEL_SCALE 0.30

static struct wheel_ctx *ctx_of(void *data) {
    return (struct wheel_ctx *)data;
}

static void w_enter(void *data, struct wl_pointer *p, uint32_t serial,
                    struct wl_surface *s, wl_fixed_t x, wl_fixed_t y) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->enter) c->orig->enter(c->data, p, serial, s, x, y);
}
static void w_leave(void *data, struct wl_pointer *p, uint32_t serial,
                    struct wl_surface *s) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->leave) c->orig->leave(c->data, p, serial, s);
}
static void w_motion(void *data, struct wl_pointer *p, uint32_t t,
                     wl_fixed_t x, wl_fixed_t y) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->motion) c->orig->motion(c->data, p, t, x, y);
}
static void w_button(void *data, struct wl_pointer *p, uint32_t serial,
                     uint32_t t, uint32_t b, uint32_t state) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->button) c->orig->button(c->data, p, serial, t, b, state);
}
static void w_axis(void *data, struct wl_pointer *p, uint32_t t,
                   uint32_t axis, wl_fixed_t value) {
    struct wheel_ctx *c = ctx_of(data);
    /* KWin may omit axis_source for mouse wheels. In that case synthesize a
     * CONTINUOUS source so Firefox cannot fall back to its broken WHEEL path.
     * We do NOT mark this as a wheel frame (touchpad may also omit source),
     * so touchpad events are never scaled. */
    if (!c->saw_source && c->orig->axis_source) {
        c->orig->axis_source(c->data, p, WL_POINTER_AXIS_SOURCE_CONTINUOUS);
    }

    /* Scale ONLY frames explicitly tagged as WHEEL by axis_source. */
    if (c->is_wheel_frame) {
        value = wl_fixed_from_double(wl_fixed_to_double(value) * WHEEL_SCALE);
    }
    if (c->orig->axis) c->orig->axis(c->data, p, t, axis, value);
}
static void w_frame(void *data, struct wl_pointer *p) {
    struct wheel_ctx *c = ctx_of(data);
    c->saw_source = 0;
    c->is_wheel_frame = 0;
    if (c->orig->frame) c->orig->frame(c->data, p);
}
static void w_axis_source(void *data, struct wl_pointer *p, uint32_t source) {
    struct wheel_ctx *c = ctx_of(data);
    c->saw_source = 1;
    if (source == WL_POINTER_AXIS_SOURCE_WHEEL ||
        source == WL_POINTER_AXIS_SOURCE_WHEEL_TILT) {
        c->is_wheel_frame = 1;
        source = WL_POINTER_AXIS_SOURCE_CONTINUOUS;
    } else {
        c->is_wheel_frame = 0;  /* CONTINUOUS / FINGER: touchpad, leave intact */
    }
    if (c->orig->axis_source) c->orig->axis_source(c->data, p, source);
}
static void w_axis_stop(void *data, struct wl_pointer *p, uint32_t t,
                        uint32_t axis) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->axis_stop) c->orig->axis_stop(c->data, p, t, axis);
}
/* Discrete step events are deliberately dropped: they are what makes Firefox
 * choose the broken WHEEL path instead of the working continuous path. */
static void w_axis_discrete(void *data, struct wl_pointer *p,
                            uint32_t axis, int32_t discrete) {
    (void)data; (void)p; (void)axis; (void)discrete;
}
static void w_axis_value120(void *data, struct wl_pointer *p,
                            uint32_t axis, int32_t v) {
    (void)data; (void)p; (void)axis; (void)v;
}
static void w_axis_relative_direction(void *data, struct wl_pointer *p,
                                      uint32_t axis, uint32_t dir) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->axis_relative_direction)
        c->orig->axis_relative_direction(c->data, p, axis, dir);
}
static void w_warp(void *data, struct wl_pointer *p,
                   wl_fixed_t x, wl_fixed_t y) {
    struct wheel_ctx *c = ctx_of(data);
    if (c->orig->warp) c->orig->warp(c->data, p, x, y);
}

/*
 * GDK/libxul call wl_proxy_add_listener() directly (the typed
 * wl_pointer_add_listener() is a static inline and gets inlined away), so the
 * real interception point is here. Only pointer proxies get the wrapped
 * listener; every other Wayland object is untouched.
 */
int wl_proxy_add_listener(struct wl_proxy *proxy, void (**implementation)(void),
                          void *data) {
    static proxy_add_listener_t real_add = NULL;
    if (!real_add)
        real_add = (proxy_add_listener_t)dlsym(RTLD_NEXT, "wl_proxy_add_listener");
    if (!real_add) return -1;

    if (!is_wl_pointer_interface(wl_proxy_get_interface_local(proxy)) || !implementation)
        return real_add(proxy, implementation, data);

    const struct wl_pointer_listener *listener =
        (const struct wl_pointer_listener *)implementation;
    struct wheel_ctx *c = calloc(1, sizeof(*c));
    if (!c) return real_add(proxy, implementation, data);
    c->orig = listener;
    c->data = data;

    c->wrapped = *listener;
    c->wrapped.enter = w_enter;
    c->wrapped.leave = w_leave;
    c->wrapped.motion = w_motion;
    c->wrapped.button = w_button;
    c->wrapped.axis = w_axis;
    c->wrapped.frame = w_frame;
    c->wrapped.axis_source = w_axis_source;
    c->wrapped.axis_stop = w_axis_stop;
    c->wrapped.axis_discrete = w_axis_discrete;
    c->wrapped.axis_value120 = w_axis_value120;
    c->wrapped.axis_relative_direction = w_axis_relative_direction;
    c->wrapped.warp = w_warp;

    return real_add(proxy, (void (**)(void))&c->wrapped, c);
}

int wl_pointer_add_listener(struct wl_pointer *p,
                            const struct wl_pointer_listener *listener,
                            void *data) {
    static add_listener_t real_add = NULL;
    if (!real_add)
        real_add = (add_listener_t)dlsym(RTLD_NEXT, "wl_pointer_add_listener");
    if (!listener || !real_add) return real_add(p, listener, data);

    struct wheel_ctx *c = calloc(1, sizeof(*c));
    if (!c) return real_add(p, listener, data);
    c->orig = listener;
    c->data = data;

    c->wrapped = *listener;
    c->wrapped.enter = w_enter;
    c->wrapped.leave = w_leave;
    c->wrapped.motion = w_motion;
    c->wrapped.button = w_button;
    c->wrapped.axis = w_axis;
    c->wrapped.frame = w_frame;
    c->wrapped.axis_source = w_axis_source;
    c->wrapped.axis_stop = w_axis_stop;
    c->wrapped.axis_discrete = w_axis_discrete;
    c->wrapped.axis_value120 = w_axis_value120;
    c->wrapped.axis_relative_direction = w_axis_relative_direction;
    c->wrapped.warp = w_warp;

    return real_add(p, &c->wrapped, c);
}
