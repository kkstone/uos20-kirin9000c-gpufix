/*
 * wayland_mini.h - minimal, self-contained Wayland client definitions for
 * rebuilding wheel_shim.so on UOS20 without any libwayland-dev headers.
 *
 * IMPORTANT: the struct wl_pointer_listener layout below is a byte-exact
 * replica of the Huawei-customized Wayland headers the ORIGINAL wheel_shim.so
 * was compiled against (verified by disassembling the original .so):
 *   - 12 listener callbacks, calloc size 0x78 (120)
 *   - field write offsets 16/24/40/56/72/88/104 inside struct wheel_ctx
 *   - enum axis_source: WHEEL=0, FINGER=1, CONTINUOUS=2, WHEEL_TILT=3
 * Stock libwayland-dev 1.21/1.26 headers only have 11 fields with a different
 * order (warp before axis_value120), so they must NOT be used here.
 */
#ifndef WAYLAND_MINI_H
#define WAYLAND_MINI_H

#include <stdint.h>

typedef int32_t wl_fixed_t;
#define wl_fixed_to_double(f)   ((double)(f) / 256.0)
#define wl_fixed_from_double(d) ((wl_fixed_t)((d) * 256.0))

/* Opaque protocol objects - only used as pointer types. */
struct wl_proxy;
struct wl_display;
struct wl_surface;
struct wl_pointer;
struct wl_event_queue;

/* wl_interface: the first field (name) is stable across all versions. */
struct wl_interface {
    const char *name;
    int         version;
    int         method_count;
    const void *methods;
    int         event_count;
    const void *events;
};

/* wl_object / wl_proxy: the interface pointer is the FIRST word of a proxy.
 * This ABI is stable in every libwayland version (0.x -> 1.26). */
struct wl_object {
    const struct wl_interface *interface;
    const void                *implementation;
    uint32_t                   id;
};
struct wl_proxy {
    struct wl_object  object;
    struct wl_event_queue *queue;
    void             *user_data;
    void             *dispatcher;
    uint32_t          version;
};

/* wl_pointer.axis_source values (Huawei/standard 1.21+ numbering). */
enum wl_pointer_axis_source {
    WL_POINTER_AXIS_SOURCE_WHEEL       = 0,
    WL_POINTER_AXIS_SOURCE_FINGER      = 1,
    WL_POINTER_AXIS_SOURCE_CONTINUOUS  = 2,
    WL_POINTER_AXIS_SOURCE_WHEEL_TILT  = 3,
};

/*
 * wl_pointer_listener - 12 fields, exact original order.
 * Field byte offsets: 0,8,16,24,32,40,48,56,64,72,80,88 (size 96).
 */
struct wl_pointer_listener {
    void (*enter)(void *, struct wl_pointer *, uint32_t,
                  struct wl_surface *, wl_fixed_t, wl_fixed_t);
    void (*leave)(void *, struct wl_pointer *, uint32_t,
                  struct wl_surface *);
    void (*motion)(void *, struct wl_pointer *, uint32_t,
                   wl_fixed_t, wl_fixed_t);
    void (*button)(void *, struct wl_pointer *, uint32_t,
                   uint32_t, uint32_t, uint32_t);
    void (*axis)(void *, struct wl_pointer *, uint32_t,
                 uint32_t, wl_fixed_t);
    void (*frame)(void *, struct wl_pointer *);
    void (*axis_source)(void *, struct wl_pointer *, uint32_t);
    void (*axis_stop)(void *, struct wl_pointer *, uint32_t, uint32_t);
    void (*axis_discrete)(void *, struct wl_pointer *, uint32_t, int32_t);
    void (*axis_value120)(void *, struct wl_pointer *, uint32_t, int32_t);
    void (*axis_relative_direction)(void *, struct wl_pointer *,
                                    uint32_t, uint32_t);
    void (*warp)(void *, struct wl_pointer *, wl_fixed_t, wl_fixed_t);
};

#endif
