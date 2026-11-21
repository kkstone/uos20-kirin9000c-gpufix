#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/*
 * csd_shim.so - Firefox-only window decoration shim.
 *
 * 1) Firefox 160 nsWindow::ToplevelUsesCSD() calls GDK's
 *    gdk_wayland_display_prefers_ssd() via dlsym(RTLD_DEFAULT). KWin advertises
 *    org_kde_kwin_server_decoration_manager.default_mode(2)=Server, so GDK
 *    returns TRUE -> Firefox falls back to a KWin-drawn server titlebar.
 *    Returning 0 makes Firefox draw its own CSD titlebar.
 *
 * 2) Firefox's GTK headerbar reads org.gnome.desktop.wm.preferences
 *    "button-layout" through GSettings. Changing that key globally with
 *    gsettings changes EVERY GTK app, which the user does not want. This
 *    shim interposes g_settings_get_string()/g_settings_get_value() only for
 *    the "button-layout" key and only inside Firefox (loaded via LD_PRELOAD
 *    from firefox-v12-wayland.sh), returning the three-button layout.
 *
 */
typedef struct _GdkDisplay GdkDisplay;
typedef struct _GSettings GSettings;
typedef struct _GVariant GVariant;

typedef char *(*settings_get_string_fn)(GSettings *, const char *);
typedef GVariant *(*settings_get_value_fn)(GSettings *, const char *);
typedef GVariant *(*variant_new_string_fn)(const char *);

#define BUTTON_LAYOUT "menu:minimize,maximize,close"

int gdk_wayland_display_prefers_ssd(GdkDisplay *display) {
    (void)display;
    return 0;  /* FALSE: client-side decorations */
}

static int is_button_layout(const char *key) {
    return key && strcmp(key, "button-layout") == 0;
}

char *g_settings_get_string(GSettings *settings, const char *key) {
    static settings_get_string_fn real_get = NULL;
    if (!real_get)
        real_get = (settings_get_string_fn)dlsym(RTLD_NEXT, "g_settings_get_string");
    if (is_button_layout(key)) return strdup(BUTTON_LAYOUT);
    return real_get ? real_get(settings, key) : NULL;
}

GVariant *g_settings_get_value(GSettings *settings, const char *key) {
    static settings_get_value_fn real_get = NULL;
    static variant_new_string_fn new_string = NULL;
    if (!real_get)
        real_get = (settings_get_value_fn)dlsym(RTLD_NEXT, "g_settings_get_value");
    if (is_button_layout(key)) {
        if (!new_string)
            new_string = (variant_new_string_fn)dlsym(RTLD_NEXT, "g_variant_new_string");
        if (new_string) return new_string(BUTTON_LAYOUT);
    }
    return real_get ? real_get(settings, key) : NULL;
}
