#!/bin/bash
# ============================================================================
# firefox (deb) — UOS20 / Kirin9000c / Maleoon910 全硬件加速 Firefox 启动器
# 由 firefox.appimage deb 包安装到 /usr/local/bin/firefox
#
# 原厂 Firefox AppImage 原样运行（零字节补丁），全部硬件加速靠 LD_PRELOAD 垫片：
#   - WebRender / WebGL(Maleoon910) 硬件渲染
#   - H264 VAAPI 真硬解（vadec）
#   - 原生 Wayland + CSD 自绘三按钮 + 修复鼠标滚轮/退出段错误
# 安装前缀：/opt/mozilla-firefox/{bin,lib,src}
# ============================================================================
set -u

PREFIX=/opt/mozilla-firefox
BIN="$PREFIX/bin/firefox-bin"        # 原厂 AppImage
LIB="$PREFIX/lib"
SHIM="$LIB/egl-huawei-v12"
SPOOF="$LIB/egl-fbspoof"
MESA="$LIB/mesa-egl-glvnd"
DRMSHIM="$LIB/ff-drm-device-shim.so"
PROFILE="${HOME:-/home/UOS}/.mozilla/firefox/mozilla-firefox-appimage"

# ---- 目录准备 ---------------------------------------------------------------
# Firefox 的 -profile 要求目标目录必须已存在，否则报
# "Your Firefox profile cannot be loaded"。
if [ ! -d "$PROFILE" ]; then
  mkdir -p "$PROFILE"
fi

# ---- DPI 缩放：动态读取系统缩放，不写死 ----------------------------------
# 分发到其他机器时缩放比例可能不同，按以下优先级动态取值：
#   1) FF_DPI_OVERRIDE        手动覆盖（特殊机器兜底）
#   2) com.deepin.xsettings scale-factor   UOS/DDE 私有键（与 eglrun 同口径）
#   3) Xft.dpi / 96            X 侧缩放镜像
#   4) org.gnome.desktop.interface scaling-factor  GNOME 标准键
#   5) 1.0                     兜底（不缩放）
FF_DPI="${FF_DPI_OVERRIDE:-}"
if [ -z "$FF_DPI" ]; then
  FF_DPI=$(gsettings get com.deepin.xsettings scale-factor 2>/dev/null | tr -d "'[:space:]")
  case "$FF_DPI" in ''|*[!0-9.]*) FF_DPI="" ;; esac
fi
if [ -z "$FF_DPI" ]; then
  _XFT=$(xrdb -query 2>/dev/null | awk '/Xft\.dpi/{print $2; exit}')
  case "$_XFT" in ''|*[!0-9.]*) _XFT="" ;; esac
  [ -n "$_XFT" ] && FF_DPI=$(awk -v d="$_XFT" 'BEGIN{printf "%.2f", d/96}')
fi
if [ -z "$FF_DPI" ]; then
  _GSF=$(gsettings get org.gnome.desktop.interface scaling-factor 2>/dev/null | tr -d 'uint32[:space:]')
  case "$_GSF" in ''|*[!0-9.]*|0) _GSF="" ;; esac
  [ -n "$_GSF" ] && FF_DPI="$_GSF"
fi
[ -z "$FF_DPI" ] && FF_DPI="1.0"
case "$FF_DPI" in ''|*[!0-9.]*) FF_DPI="1.0" ;; esac

# ---- 指针尺寸：每次启动按当前系统 DPI 动态设置（可移植）-------------------
# 原生 Wayland 下 Firefox 的指针大小读 GTK 标准键
# org.gnome.desktop.interface cursor-size，而 UOS/DDE 把缩放写在私有键
# com.deepin.xsettings。重装 / dconf 重置后标准键会回落到 24(1x)，导致界面
# 已按 FF_DPI 放大、指针却是原始大小。这里每次启动都用上面算好的 FF_DPI
# 换算并写入标准键（24px × 缩放，四舍五入），换机器/改缩放均自动适配。
# 注意：gsettings/dconf 属于登录用户，必须以运行 firefox 的用户身份写入。
if command -v gsettings >/dev/null 2>&1; then
  _FF_CURSOR=$(awk -v d="$FF_DPI" 'BEGIN{printf "%d", 24*d+0.5}')
  case "$_FF_CURSOR" in ''|*[!0-9]*) _FF_CURSOR=24 ;; esac
  _FF_CUR=$(gsettings get org.gnome.desktop.interface cursor-size 2>/dev/null | tr -d 'uint32[:space:]')
  if [ -n "$_FF_CURSOR" ] && [ "$_FF_CUR" != "$_FF_CURSOR" ]; then
    gsettings set org.gnome.desktop.interface cursor-size "$_FF_CURSOR" 2>/dev/null || true
  fi
  unset _FF_CURSOR _FF_CUR
fi

# ---- 硬件加速 / UI 偏好：每次启动都注入 -----------------------------------
# 重要：语言引导由 OS locale + langpack 自动触发，与 user.js 里的 pref 无关；
# 而 DPI 与沙箱横幅隐藏都在 user.js 里。故 user.js 必须首次也写，
# 否则首次启动 DPI 不跟随、横幅出现。SHOW_WELCOME 只决定"是否传 URL"。
cat > "$PROFILE/user.js" <<'PREF'
// 关键：显式 false 覆盖可能持久化的 true（user.js 优先级最高）。
user_pref("gfx.webrender.all", false);
user_pref("gfx.webrender.software", false);
user_pref("gfx.webrender.force-software", false);
user_pref("gfx.webrender.reject-software-driver", false);
user_pref("gfx.x11-egl.force-enabled", false);
user_pref("widget.wayland.enable", true);
user_pref("webgl.force-enabled", true);
user_pref("media.hardware-video-decoding.enabled", true);
user_pref("media.hardware-video-decoding.force-enabled", true);
// 窗口：标签栏即标题栏 + Firefox 自绘三按钮（CSD shim 强制 request_mode(1)）
user_pref("browser.tabs.inTitlebar", 1);
// DPI：按系统缩放动态注入（见启动器 FF_DPI 计算），KWin wl_output 常误报 1x
user_pref("layout.css.devPixelsPerPx", "__FF_DPI__");
// 滚轮：关闭 system_scroll_override，用系统原生轴步长；smoothScroll 必须保持开启
user_pref("mousewheel.system_scroll_override.enabled", false);
user_pref("mousewheel.system_scroll_override_on_right_button.enabled", false);
user_pref("mousewheel.default.delta_multiplier_x", 200);
user_pref("mousewheel.default.delta_multiplier_y", 300);
user_pref("mousewheel.default.delta_multiplier_z", 200);
user_pref("mousewheel.min_line_scroll_amount", 5);
user_pref("general.smoothScroll.enabled", true);
user_pref("general.smoothScroll.mouseWheel.enabled", true);
user_pref("general.smoothScroll.msdPhysics.enabled", false);
// 崩溃清理阶段 UAF 无害，禁用崩溃弹窗
user_pref("toolkit.crashreporter.enabled", false);
user_pref("toolkit.startup.max_resumed_crashes", -1);
// 隐藏"沙箱已关闭"横幅（官方 pref + 用户样式表）
user_pref("toolkit.legacyUserProfileCustomizations.stylesheets", true);
user_pref("security.sandbox.warn_unprivileged_namespaces", false);
PREF
# 把动态 DPI 值代入 user.js（heredoc 用单引号防止其它 pref 被误展开）
sed -i "s/__FF_DPI__/$FF_DPI/" "$PROFILE/user.js"

mkdir -p "$PROFILE/chrome"
cat > "$PROFILE/chrome/userChrome.css" <<'CSS'
notification-message[value="sandbox-content-disabled"],
notification-message[value="sandbox-unprivileged-namespaces"],
notification[value="sandbox-content-disabled"],
notification[value="sandbox-unprivileged-namespaces"] {
  display: none !important;
  visibility: collapse !important;
}
CSS

rm -f "$PROFILE/lock" "$PROFILE/.parentlock"

# ---- 是否显示首次运行向导 --------------------------------------------------
# Firefox 的 about:welcome 首次向导（含"使用简体中文继续浏览"语言引导）由
# profile 里的首启标记决定，且带 URL 启动会跳过它。判定依据：
#   trailhead.firstrun.didSeeAboutWelcome / browser.aboutwelcome.didSeeFinalScreen
#   browser.startup.homepage_override.mstone（版本里程碑）
# 这三个 pref 在全新 profile 里都不存在 → 首次启动向导出现。
SHOW_WELCOME=0
if [ ! -f "$PROFILE/prefs.js" ]; then
  SHOW_WELCOME=1
fi

# ---- 可选：在已有 profile 上重放首次运行向导 -------------------------------
# 用法：FF_REPLAY_FIRST_RUN=1 firefox
# 清掉首启标记（不动语言包、不动硬件加速 pref），下次启动即重新弹出
# about:welcome 向导，用于复测首次启动的 DPI / 横幅 / 语言引导表现。
if [ "${FF_REPLAY_FIRST_RUN:-0}" = "1" ] && [ -f "$PROFILE/prefs.js" ]; then
  sed -i \
    -e '/trailhead\.firstrun\.didSeeAboutWelcome/d' \
    -e '/browser\.aboutwelcome\.didSeeFinalScreen/d' \
    -e '/browser\.startup\.homepage_override\.mstone/d' \
    -e '/browser\.startup\.homepage_override\.buildID/d' \
    "$PROFILE/prefs.js"
  SHOW_WELCOME=1
  echo "[firefox] 已重置首次运行标记，本次将显示 about:welcome 向导" >&2
fi

# ---- 库路径：华为 EGL 垫片置顶 ----
export LD_LIBRARY_PATH="$SHIM:$MESA${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

# ---- Firefox 专用华为库副本（patch 掉析构 UAF），只影响 Firefox ----
export HISI_FF_LIBGFX="$LIB/libGFX_hisi.so.0.8.0"

# ---- 原生 Wayland ----
export GDK_BACKEND=wayland MOZ_ENABLE_WAYLAND=1
export WAYLAND_DISPLAY="${WAYLAND_DISPLAY:-wayland-0}"
export LIBGL_ALWAYS_SOFTWARE=0 LIBGL_KOPPER_DRI2=1
unset GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE EGL_PLATFORM
unset __EGL_VENDOR_LIBRARY_FILENAMES EGL_VENDOR_LIBRARY_FILENAMES

# ---- 跟随系统深色主题 ------------------------------------------------------
# Deepin 深色状态写在 ~/.config/gtk-3.0/settings.ini 的 gtk-theme-name
# （deepin-dark = 深色 / deepin = 浅色）。Wayland 下 Firefox 走 portal/标准键，
# 读到的是浅色 Adwaita，故这里显式把当前 GTK 主题注入给 Firefox。
# 说明：只在启动时读取一次，运行中切换系统主题需重启 Firefox 生效。
SYS_GTK_THEME=$(sed -nE 's/^[[:space:]]*gtk-theme-name[[:space:]]*=[[:space:]]*([^[:space:]#]+).*/\1/p' \
                "${HOME:-/home/UOS}/.config/gtk-3.0/settings.ini" 2>/dev/null | head -n1)
if [ -n "$SYS_GTK_THEME" ]; then
  export GTK_THEME="$SYS_GTK_THEME"
fi

# ---- GPU 探测伪装：让 Firefox 认定 Maleoon 910 是硬件驱动 ----
export MOZ_GFX_SPOOF_GL_VERSION="4.5 (Compatibility Profile) 23.1.2"
export MOZ_GFX_SPOOF_GL_VENDOR="HUAWEI"
export MOZ_GFX_SPOOF_GL_RENDERER="Maleoon 910"

# ---- 华为 GBM ABI 桥 + VAAPI vacore 硬解 ----
export HISI_GBM_SHIM=1
export HISI_GBM_REPORT_MODIFIER=0 HISI_GBM_STRIDE_FIX=0 HISI_GBM_IMPORT_FIX=0 HISI_GBM_PLANE_FD=0

# ---- 垫片链（无字节补丁；gfxtest-env 在 exec/spawn 边界给探测子进程换环境）----
export LD_PRELOAD="$LIB/wllog_shim.so $LIB/csd_shim.so $LIB/wheel_shim.so $LIB/gbm-hw-shim.so $LIB/gfxtest_env_shim.so"
export LIBVA_DRIVER_NAME=vacore LIBVA_DRIVERS_PATH=/usr/lib
export VAAPI_CONNECT_DEVICE=/dev/dri/renderD128 LIBVA_DRI3_DISABLE=1

# ---- 自绘 CSD 三按钮 ----
export MOZ_GTK_TITLEBAR_DECORATION=client

# ---- 沙箱：实测与私有 shim/华为设备节点不兼容，必须关闭 ----
export MOZ_DISABLE_CONTENT_SANDBOX=1 MOZ_DISABLE_GMP_SANDBOX=1 MOZ_DISABLE_RDD_SANDBOX=1
export MOZ_CRASHREPORTER_DISABLE=1

URL="${1:-about:newtab}"
EXTRA_ARGS=()
if [ -n "${FF_EXTRA_ARGS:-}" ]; then read -r -a EXTRA_ARGS <<< "$FF_EXTRA_ARGS"; fi

if [ ! -x "$BIN" ]; then
  echo "[firefox] ERROR: AppImage 不存在: $BIN" >&2
  exit 1
fi

if [ "$SHOW_WELCOME" -eq 1 ]; then
  # 显示首次运行向导：不传 URL，让 about:welcome 语言引导出现
  exec "$BIN" -no-remote -profile "$PROFILE" "${EXTRA_ARGS[@]}"
fi
exec "$BIN" -no-remote -profile "$PROFILE" "${EXTRA_ARGS[@]}" "$URL"
