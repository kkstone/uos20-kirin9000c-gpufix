/*
 * wayland-im-shim
 *
 * A tiny Wayland input-method (zwp_input_method_v1) client for KWin 5.27.
 * It bridges native Wayland text input to the already-working UOS fcitx4
 * (Sogou) DBus frontend, so Chrome's native Wayland mode can type Chinese
 * without giving up VAAPI hardware video decode.
 *
 * Build:  ./build.sh
 * Run by KWin only (it passes WAYLAND_SOCKET=<fd>):  ./wayland-im-shim
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include <dbus/dbus.h>
#include <wayland-client.h>
#include <wayland-client-protocol.h>
#include <xkbcommon/xkbcommon.h>

#include "input-method-unstable-v1-client-protocol.h"

#define LOG_PATH "/tmp/wayland-im-shim.log"

static FILE *log_fp;
static void logmsg(const char *fmt, ...)
{
    if (!log_fp) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(log_fp, fmt, ap);
    fputc('\n', log_fp);
    fflush(log_fp);
    va_end(ap);
}

static struct wl_display *display;
static struct zwp_input_method_v1 *input_method;
static struct zwp_input_method_context_v1 *context;
static struct wl_keyboard *grab_keyboard;
static struct xkb_context *xkb_ctx;
static struct xkb_keymap *xkb_keymap;
static struct xkb_state *xkb_state;

static DBusConnection *bus;
static char ic_path[80];
static uint32_t current_serial;
static uint32_t mods_depressed, mods_latched, mods_locked, mods_group;

static char surrounding_text[8192];
static uint32_t surrounding_cursor, surrounding_anchor;
static char last_preedit[1024];

static void dbus_noarg(const char *method)
{
    if (!ic_path[0]) return;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        "org.fcitx.Fcitx", ic_path, "org.fcitx.Fcitx.InputContext", method);
    if (!msg) return;
    dbus_connection_send(bus, msg, NULL);
    dbus_message_unref(msg);
    (void)err;
}

static void dbus_set_surrounding(const char *text, uint32_t cursor, uint32_t anchor)
{
    if (!ic_path[0]) return;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.fcitx.Fcitx", ic_path, "org.fcitx.Fcitx.InputContext", "SetSurroundingText");
    if (!msg) return;
    const char *t = text ? text : "";
    uint32_t c = cursor, a = anchor;
    dbus_message_append_args(msg,
        DBUS_TYPE_STRING, &t,
        DBUS_TYPE_UINT32, &c,
        DBUS_TYPE_UINT32, &a,
        DBUS_TYPE_INVALID);
    dbus_connection_send(bus, msg, NULL);
    dbus_message_unref(msg);
}

static void dbus_set_cursor_rect(int32_t x, int32_t y, int32_t w, int32_t h)
{
    if (!ic_path[0]) return;
    DBusMessage *msg = dbus_message_new_method_call(
        "org.fcitx.Fcitx", ic_path, "org.fcitx.Fcitx.InputContext", "SetCursorRect");
    if (!msg) return;
    dbus_message_append_args(msg,
        DBUS_TYPE_INT32, &x, DBUS_TYPE_INT32, &y,
        DBUS_TYPE_INT32, &w, DBUS_TYPE_INT32, &h,
        DBUS_TYPE_INVALID);
    dbus_connection_send(bus, msg, NULL);
    dbus_message_unref(msg);
}

static int dbus_process_key(uint32_t sym, uint32_t xcode, uint32_t state, int type, uint32_t time)
{
    if (!ic_path[0]) return 0;
    DBusError err;
    dbus_error_init(&err);
    DBusMessage *msg = dbus_message_new_method_call(
        "org.fcitx.Fcitx", ic_path, "org.fcitx.Fcitx.InputContext", "ProcessKeyEvent");
    if (!msg) return 0;
    dbus_int32_t t = type;
    dbus_message_append_args(msg,
        DBUS_TYPE_UINT32, &sym,
        DBUS_TYPE_UINT32, &xcode,
        DBUS_TYPE_UINT32, &state,
        DBUS_TYPE_INT32, &t,
        DBUS_TYPE_UINT32, &time,
        DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, 1500, &err);
    dbus_message_unref(msg);
    int handled = 0;
    if (reply) {
        dbus_int32_t r = 0;
        if (dbus_message_get_args(reply, &err, DBUS_TYPE_INT32, &r, DBUS_TYPE_INVALID))
            handled = r ? 1 : 0;
        else
            logmsg("ProcessKeyEvent reply parse failed: %s", err.message ? err.message : "?");
        dbus_message_unref(reply);
    } else {
        logmsg("ProcessKeyEvent failed: %s", err.name ? err.name : (err.message ? err.message : "timeout"));
    }
    if (dbus_error_is_set(&err)) dbus_error_free(&err);
    return handled;
}

static void send_preedit(const char *text, int32_t cursor)
{
    if (!context) return;
    if (!text) text = "";
    if (!strcmp(text, last_preedit)) return;
    snprintf(last_preedit, sizeof(last_preedit), "%s", text);
    if (cursor < 0) cursor = 0;
    zwp_input_method_context_v1_preedit_cursor(context, cursor);
    zwp_input_method_context_v1_preedit_string(context, current_serial, text, "");
    logmsg("preedit '%s' cursor=%d serial=%u", text, cursor, current_serial);
}

static DBusHandlerResult dbus_filter(DBusConnection *c, DBusMessage *msg, void *data)
{
    (void)c; (void)data;
    if (!context || !dbus_message_has_interface(msg, "org.fcitx.Fcitx.InputContext"))
        return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;

    if (dbus_message_is_signal(msg, "org.fcitx.Fcitx.InputContext", "CommitString")) {
        const char *text = NULL;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INVALID) && text && *text) {
            zwp_input_method_context_v1_commit_string(context, current_serial, text);
            logmsg("commit '%s' serial=%u", text, current_serial);
            last_preedit[0] = '\0';
        }
    } else if (dbus_message_is_signal(msg, "org.fcitx.Fcitx.InputContext", "UpdatePreedit")) {
        const char *text = NULL;
        dbus_int32_t cursor = 0;
        if (dbus_message_get_args(msg, NULL, DBUS_TYPE_STRING, &text, DBUS_TYPE_INT32, &cursor, DBUS_TYPE_INVALID))
            send_preedit(text, cursor);
    } else if (dbus_message_is_signal(msg, "org.fcitx.Fcitx.InputContext", "UpdateFormattedPreedit")) {
        DBusMessageIter it;
        if (dbus_message_iter_init(msg, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_ARRAY) {
            DBusMessageIter arr;
            char text[1024];
            text[0] = '\0';
            dbus_message_iter_recurse(&it, &arr);
            while (dbus_message_iter_get_arg_type(&arr) == DBUS_TYPE_STRUCT) {
                DBusMessageIter st;
                dbus_message_iter_recurse(&arr, &st);
                if (dbus_message_iter_get_arg_type(&st) == DBUS_TYPE_STRING) {
                    const char *s = NULL;
                    dbus_message_iter_get_basic(&st, &s);
                    if (s) strncat(text, s, sizeof(text) - strlen(text) - 1);
                }
                dbus_message_iter_next(&arr);
            }
            dbus_message_iter_next(&it);
            dbus_int32_t cursor = 0;
            if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32)
                dbus_message_iter_get_basic(&it, &cursor);
            send_preedit(text, cursor);
        }
    }
    return DBUS_HANDLER_RESULT_NOT_YET_HANDLED;
}

static void fcitx_create_ic(void)
{
    DBusError err;
    dbus_error_init(&err);
    const char *app = "chrome";
    dbus_int32_t pid = getpid();
    DBusMessage *msg = dbus_message_new_method_call(
        "org.fcitx.Fcitx", "/inputmethod", "org.fcitx.Fcitx.InputMethod", "CreateICv3");
    if (!msg) return;
    dbus_message_append_args(msg, DBUS_TYPE_STRING, &app, DBUS_TYPE_INT32, &pid, DBUS_TYPE_INVALID);
    DBusMessage *reply = dbus_connection_send_with_reply_and_block(bus, msg, 2000, &err);
    dbus_message_unref(msg);
    ic_path[0] = '\0';
    if (!reply) {
        logmsg("CreateICv3 failed: %s", err.name ? err.name : (err.message ? err.message : "?"));
        if (dbus_error_is_set(&err)) dbus_error_free(&err);
        return;
    }
    DBusMessageIter it;
    dbus_int32_t idx = -1;
    dbus_bool_t ok = 0;
    if (dbus_message_iter_init(reply, &it) && dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_INT32)
        dbus_message_iter_get_basic(&it, &idx);
    if (dbus_message_iter_has_next(&it)) {
        dbus_message_iter_next(&it);
        if (dbus_message_iter_get_arg_type(&it) == DBUS_TYPE_BOOLEAN)
            dbus_message_iter_get_basic(&it, &ok);
    }
    dbus_message_unref(reply);
    if (idx < 0 || !ok) {
        logmsg("CreateICv3 bad reply idx=%d ok=%d", idx, ok);
        return;
    }
    snprintf(ic_path, sizeof(ic_path), "/inputcontext_%d", (int)idx);
    char rule[256];
    snprintf(rule, sizeof(rule), "type='signal',interface='org.fcitx.Fcitx.InputContext',path='%s'", ic_path);
    dbus_bus_add_match(bus, rule, &err);
    if (dbus_error_is_set(&err)) {
        logmsg("add match failed: %s", err.message);
        dbus_error_free(&err);
    }
    logmsg("created %s", ic_path);
    dbus_noarg("EnableIC");
    dbus_noarg("FocusIn");
    dbus_set_cursor_rect(100, 100, 200, 30);
    dbus_set_surrounding(surrounding_text, surrounding_cursor, surrounding_anchor);
}

static uint32_t fallback_sym(uint32_t code)
{
    if (code >= 30 && code <= 53) return 0x61 + (code - 30);          /* A-M */
    if (code >= 48 && code <= 57) return (code == 48) ? 0x3b : 0x30 + (code - 2); /* ; and 0-9 */
    switch (code) {
    case 2: return 0x31; case 3: return 0x32; case 4: return 0x33; case 5: return 0x34;
    case 6: return 0x35; case 7: return 0x36; case 8: return 0x37; case 9: return 0x38;
    case 10: return 0x39; case 11: return 0x30; case 12: return 0x2d; case 13: return 0x3d;
    case 14: return 0xff08; case 28: return 0xff0d; case 57: return 0x20;
    case 42: return 0xffe1; case 54: return 0xffe2; case 29: return 0xffe3; case 97: return 0xffe4;
    case 56: return 0xffe9; case 103: return 0xff52; case 108: return 0xff51;
    case 105: return 0xff53; case 106: return 0xff54;
    default: return 0;
    }
}

static void keyboard_keymap(void *data, struct wl_keyboard *kb, uint32_t format, int32_t fd, uint32_t size)
{
    (void)data; (void)kb;
    if (format != WL_KEYBOARD_KEYMAP_FORMAT_XKB_V1 || size == 0) {
        logmsg("unsupported keymap format %u size %u", format, size);
        close(fd);
        return;
    }
    char *buf = mmap(NULL, size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (buf == MAP_FAILED) { logmsg("mmap keymap failed: %s", strerror(errno)); return; }
    if (xkb_state) { xkb_state_unref(xkb_state); xkb_state = NULL; }
    if (xkb_keymap) { xkb_keymap_unref(xkb_keymap); xkb_keymap = NULL; }
    xkb_keymap = xkb_keymap_new_from_string(xkb_ctx, buf, XKB_KEYMAP_FORMAT_TEXT_V1, XKB_KEYMAP_COMPILE_NO_FLAGS);
    munmap(buf, size);
    if (!xkb_keymap) { logmsg("xkb keymap parse failed"); return; }
    xkb_state = xkb_state_new(xkb_keymap);
    xkb_state_update_mask(xkb_state, mods_depressed, mods_latched, mods_locked, 0, 0, mods_group);
    logmsg("keymap installed (%u bytes)", size);
}

static void keyboard_enter(void *data, struct wl_keyboard *kb, uint32_t serial,
                           struct wl_surface *surface, struct wl_array *keys)
{ (void)data; (void)kb; (void)serial; (void)surface; (void)keys; }

static void keyboard_leave(void *data, struct wl_keyboard *kb, uint32_t serial, struct wl_surface *surface)
{ (void)data; (void)kb; (void)serial; (void)surface; }

static void keyboard_key(void *data, struct wl_keyboard *kb, uint32_t serial,
                         uint32_t time, uint32_t keycode, uint32_t state)
{
    (void)data; (void)kb;
    uint32_t xcode = keycode + 8;
    xkb_keysym_t sym = 0;
    if (xkb_state) {
        if (state == WL_KEYBOARD_KEY_STATE_PRESSED)
            xkb_state_update_key(xkb_state, xcode, XKB_KEY_DOWN);
        sym = xkb_state_key_get_one_sym(xkb_state, xcode);
        if (state == WL_KEYBOARD_KEY_STATE_RELEASED)
            xkb_state_update_key(xkb_state, xcode, XKB_KEY_UP);
    }
    if (!sym) sym = fallback_sym(keycode);
    uint32_t fstate = mods_depressed | mods_latched | mods_locked;
    int type = (state == WL_KEYBOARD_KEY_STATE_PRESSED) ? 0 : 1;
    int handled = dbus_process_key((uint32_t)sym, xcode, fstate, type, time);
    dbus_connection_read_write_dispatch(bus, 0);
    if (!handled && context) {
        zwp_input_method_context_v1_key(context, serial, 0, keycode, state);
        logmsg("forward key code=%u state=%u sym=%x", keycode, state, (unsigned)sym);
    } else {
        logmsg("handled key code=%u state=%u sym=%x", keycode, state, (unsigned)sym);
    }
}

static void keyboard_modifiers(void *data, struct wl_keyboard *kb, uint32_t serial,
                               uint32_t depressed, uint32_t latched, uint32_t locked, uint32_t group)
{
    (void)data; (void)kb;
    mods_depressed = depressed;
    mods_latched = latched;
    mods_locked = locked;
    mods_group = group;
    if (xkb_state)
        xkb_state_update_mask(xkb_state, depressed, latched, locked, 0, 0, group);
    if (context)
        zwp_input_method_context_v1_modifiers(context, serial, depressed, latched, locked, group);
    logmsg("modifiers dep=%u lat=%u lock=%u grp=%u", depressed, latched, locked, group);
}

static void keyboard_repeat_info(void *data, struct wl_keyboard *kb, int32_t rate, int32_t delay)
{ (void)data; (void)kb; (void)rate; (void)delay; }

static const struct wl_keyboard_listener keyboard_listener = {
    .keymap = keyboard_keymap,
    .enter = keyboard_enter,
    .leave = keyboard_leave,
    .key = keyboard_key,
    .modifiers = keyboard_modifiers,
    .repeat_info = keyboard_repeat_info,
};

static void ctx_surrounding_text(void *data, struct zwp_input_method_context_v1 *c,
                                 const char *text, uint32_t cursor, uint32_t anchor)
{
    (void)data; (void)c;
    snprintf(surrounding_text, sizeof(surrounding_text), "%s", text ? text : "");
    surrounding_cursor = cursor;
    surrounding_anchor = anchor;
    dbus_set_surrounding(surrounding_text, cursor, anchor);
}

static void ctx_reset(void *data, struct zwp_input_method_context_v1 *c)
{ (void)data; (void)c; dbus_noarg("Reset"); last_preedit[0] = '\0'; }

static void ctx_content_type(void *data, struct zwp_input_method_context_v1 *c, uint32_t hint, uint32_t purpose)
{ (void)data; (void)c; (void)hint; (void)purpose; }

static void ctx_invoke_action(void *data, struct zwp_input_method_context_v1 *c, uint32_t button, uint32_t index)
{ (void)data; (void)c; (void)button; (void)index; }

static void ctx_commit_state(void *data, struct zwp_input_method_context_v1 *c, uint32_t serial)
{ (void)data; (void)c; current_serial = serial; logmsg("commit_state serial=%u", serial); }

static void ctx_preferred_language(void *data, struct zwp_input_method_context_v1 *c, const char *language)
{ (void)data; (void)c; (void)language; }

static const struct zwp_input_method_context_v1_listener context_listener = {
    .surrounding_text = ctx_surrounding_text,
    .reset = ctx_reset,
    .content_type = ctx_content_type,
    .invoke_action = ctx_invoke_action,
    .commit_state = ctx_commit_state,
    .preferred_language = ctx_preferred_language,
};

static void im_activate(void *data, struct zwp_input_method_v1 *im,
                        struct zwp_input_method_context_v1 *new_context)
{
    (void)data; (void)im;
    logmsg("activate");
    if (context) {
        zwp_input_method_context_v1_destroy(context);
        context = NULL;
    }
    context = new_context;
    current_serial = 0;
    last_preedit[0] = '\0';
    zwp_input_method_context_v1_add_listener(context, &context_listener, NULL);
    surrounding_text[0] = '\0';
    surrounding_cursor = surrounding_anchor = 0;
    fcitx_create_ic();
    grab_keyboard = zwp_input_method_context_v1_grab_keyboard(context);
    if (grab_keyboard)
        wl_keyboard_add_listener(grab_keyboard, &keyboard_listener, NULL);
    else
        logmsg("grab_keyboard returned NULL");
}

static void im_deactivate(void *data, struct zwp_input_method_v1 *im,
                          struct zwp_input_method_context_v1 *c)
{
    (void)data; (void)im;
    logmsg("deactivate");
    dbus_noarg("FocusOut");
    dbus_noarg("CloseIC");
    dbus_noarg("DestroyIC");
    ic_path[0] = '\0';
    if (grab_keyboard) { wl_keyboard_destroy(grab_keyboard); grab_keyboard = NULL; }
    if (context == c) { zwp_input_method_context_v1_destroy(context); context = NULL; }
    last_preedit[0] = '\0';
}

static const struct zwp_input_method_v1_listener im_listener = {
    .activate = im_activate,
    .deactivate = im_deactivate,
};

static void registry_global(void *data, struct wl_registry *registry, uint32_t name,
                            const char *interface, uint32_t version)
{
    (void)data;
    logmsg("global %u %s v%u", name, interface, version);
    if (!strcmp(interface, "zwp_input_method_v1") && !input_method)
        input_method = wl_registry_bind(registry, name, &zwp_input_method_v1_interface, 1);
}

static void registry_global_remove(void *data, struct wl_registry *registry, uint32_t name)
{ (void)data; (void)registry; (void)name; }

static const struct wl_registry_listener registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int main(void)
{
    log_fp = fopen(LOG_PATH, "w");
    if (!log_fp) return 1;
    logmsg("wayland-im-shim starting pid=%d", getpid());

    const char *sock = getenv("WAYLAND_SOCKET");
    if (!sock || !*sock) {
        logmsg("WAYLAND_SOCKET not set - this process must be started by KWin");
        return 2;
    }
    int fd = atoi(sock);
    display = wl_display_connect_to_fd(fd);
    if (!display) { logmsg("wl_display_connect_to_fd(%d) failed: %s", fd, strerror(errno)); return 3; }

    bus = dbus_bus_get(DBUS_BUS_SESSION, NULL);
    if (!bus) { logmsg("session bus connect failed"); return 4; }
    dbus_connection_add_filter(bus, dbus_filter, NULL, NULL);

    xkb_ctx = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!xkb_ctx) logmsg("xkb_context_new failed");

    struct wl_registry *registry = wl_display_get_registry(display);
    wl_registry_add_listener(registry, &registry_listener, NULL);
    wl_display_roundtrip(display);
    if (!input_method) {
        logmsg("zwp_input_method_v1 global not available on this connection");
        return 5;
    }
    zwp_input_method_v1_add_listener(input_method, &im_listener, NULL);
    logmsg("input method ready");

    struct pollfd pfd = { .fd = wl_display_get_fd(display), .events = POLLIN };
    while (1) {
        if (wl_display_flush(display) < 0 && errno != EAGAIN) {
            logmsg("wl_display_flush failed: %s", strerror(errno));
            break;
        }
        int r = poll(&pfd, 1, 25);
        if (r < 0 && errno == EINTR) continue;
        if (r > 0 && (pfd.revents & POLLIN)) {
            if (wl_display_dispatch(display) < 0) {
                logmsg("wl_display_dispatch failed: %s", strerror(errno));
                break;
            }
        }
        dbus_connection_read_write_dispatch(bus, 0);
    }
    logmsg("exiting");
    return 0;
}
