/*
 * gfxtest_env_shim.c -- 不解包运行原厂 AppImage：在 exec/spawn 边界给
 * gfxtest 子进程换环境，等价于 dist 模式的 gfxtest 包装脚本。
 *
 * 原理：Firefox 的 GL/codec 探测子进程 gfxtest 需要一套特殊环境
 *   (LD_LIBRARY_PATH 前置 egl-fbspoof + mesa、LD_PRELOAD 只留
 *    ff-drm-device-shim、zink 变量、unset EGL_PLATFORM)。
 * 已验证的 dist 方案用一层 shell 包装脚本实现；AppImage 挂载点只读
 * 无法替换 gfxtest，故改为在本垫片里 hook posix_spawn/execve，
 * 仅当目标是 gfxtest 时把这套环境注入 envp，其余进程原样透传。
 *
 * 完全不碰任何字节；挂载点里的原厂 gfxtest 二进制原样执行。
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern char **environ;

typedef int (*execve_fn)(const char *, char *const[], char *const[]);
typedef int (*spawn_fn)(pid_t *, const char *, const posix_spawn_file_actions_t *,
                        const posix_spawnattr_t *, char *const[], char *const[]);

static execve_fn real_execve;
static spawn_fn  real_spawn;

/* 路径前缀，运行时由 HOME 拼出，与包装脚本一致 */
static char SPOOF[512], MESA[512], DRMSHIM[512];

__attribute__((constructor))
static void init(void) {
    /* deb 固定安装前缀；不依赖 HOME，多用户/提权场景一致 */
    snprintf(SPOOF,   sizeof SPOOF,   "/opt/mozilla-firefox/lib/egl-fbspoof");
    snprintf(MESA,    sizeof MESA,    "/opt/mozilla-firefox/lib/mesa-egl-glvnd");
    snprintf(DRMSHIM, sizeof DRMSHIM, "/opt/mozilla-firefox/lib/ff-drm-device-shim.so");

    real_execve = (execve_fn)dlsym(RTLD_NEXT, "execve");
    real_spawn  = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
}

/* basename == "gfxtest" 才算命中；其它一律透传 */
static int is_gfxtest(const char *path) {
    if (!path) return 0;
    const char *base = strrchr(path, '/');
    base = base ? base + 1 : path;
    return strcmp(base, "gfxtest") == 0;
}

/* 这些变量要先从继承的 envp 里删掉，再由我们重新给值 */
static int should_drop(const char *e) {
    static const char *keys[] = {
        "LD_PRELOAD=", "LD_LIBRARY_PATH=", "EGL_PLATFORM=",
        "GALLIUM_DRIVER=", "MESA_LOADER_DRIVER_OVERRIDE=",
        "LIBGL_KOPPER_DRI2=", "FF_DRM_SHIM=", "HISI_GBM_",
        NULL
    };
    for (int i = 0; keys[i]; i++)
        if (strncmp(e, keys[i], strlen(keys[i])) == 0) return 1;
    return 0;
}

static char *kv(const char *key, const char *val) {
    size_t n = strlen(key) + strlen(val) + 2;
    char *s = malloc(n);
    if (s) snprintf(s, n, "%s=%s", key, val);
    return s;
}

/* 从原 envp 取某变量的值（用于 LD_LIBRARY_PATH 追加） */
static const char *get_env_val(char *const envp[], const char *key) {
    size_t kl = strlen(key);
    for (int i = 0; envp && envp[i]; i++)
        if (strncmp(envp[i], key, kl) == 0 && envp[i][kl] == '=')
            return envp[i] + kl + 1;
    return NULL;
}

/*
 * 构造 gfxtest 专用环境。等价于 dist 包装脚本：
 *   LD_LIBRARY_PATH = SPOOF:MESA[:原值]
 *   LD_PRELOAD      = 仅 ff-drm-device-shim.so（剥离所有浏览器 shim）
 *   GALLIUM_DRIVER=zink / MESA_LOADER_DRIVER_OVERRIDE=zink
 *   LIBGL_KOPPER_DRI2=1 / FF_DRM_SHIM=1
 *   EGL_PLATFORM 及其余 HISI_GBM_* 由 should_drop 删除，不再补回
 */
static char **build_gfxtest_env(char *const envp[]) {
    if (!envp) envp = environ;

    const char *orig_ldlp = get_env_val(envp, "LD_LIBRARY_PATH");
    char *ldlp = malloc(2048);
    if (orig_ldlp && *orig_ldlp)
        snprintf(ldlp, 2048, "%s:%s:%s", SPOOF, MESA, orig_ldlp);
    else
        snprintf(ldlp, 2048, "%s:%s", SPOOF, MESA);

    int n = 0;
    while (envp[n]) n++;
    char **out = calloc(n + 16, sizeof(char *));
    int j = 0;
    for (int i = 0; i < n; i++)
        if (!should_drop(envp[i])) out[j++] = strdup(envp[i]);

    out[j++] = kv("LD_LIBRARY_PATH", ldlp);
    out[j++] = kv("LD_PRELOAD", DRMSHIM);
    out[j++] = strdup("GALLIUM_DRIVER=zink");
    out[j++] = strdup("MESA_LOADER_DRIVER_OVERRIDE=zink");
    out[j++] = strdup("LIBGL_KOPPER_DRI2=1");
    out[j++] = strdup("FF_DRM_SHIM=1");
    out[j] = NULL;

    free(ldlp);
    return out;
}

static char **patched_env(char *const envp[], const char *path) {
    (void)path;
    return build_gfxtest_env(envp);
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    if (!real_execve) real_execve = (execve_fn)dlsym(RTLD_NEXT, "execve");
    if (is_gfxtest(path)) {
        char **ne = patched_env(envp, path);
        return real_execve(path, argv, ne);
    }
    return real_execve(path, argv, envp);
}

int posix_spawn(pid_t *pid, const char *path,
                const posix_spawn_file_actions_t *fa,
                const posix_spawnattr_t *attr,
                char *const argv[], char *const envp[]) {
    if (!real_spawn) real_spawn = (spawn_fn)dlsym(RTLD_NEXT, "posix_spawn");
    if (is_gfxtest(path)) {
        char **ne = patched_env(envp, path);
        return real_spawn(pid, path, fa, attr, argv, ne);
    }
    return real_spawn(pid, path, fa, attr, argv, envp);
}
