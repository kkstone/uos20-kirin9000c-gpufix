# Firefox 硬件加速适配 — 路线收口报告

> 目标平台：UOS20 / Kirin 9000c / Maleoon 910（aarch64）
> 收口日期：2026-11-19
> 一句话结论：**原厂 Firefox AppImage 原样运行，不解包、零字节补丁，全部能力靠启动器 +
> 纯垫片（LD_PRELOAD / LD_LIBRARY_PATH / 子进程环境注入）实现。**

---

## 1. 交付物坐标

| 项 | 值 |
|---|---|
| deb 包 | `firefox.appimage_160.0-2_arm64.deb`（91M） |
| Package / Version | `firefox.appimage` / `160.0-2`（与 Firefox 版本对齐） |
| Architecture | `arm64` |
| Depends | `libc6 (>= 2.28)`, `eglrun` |
| 启动命令 | `firefox`（原装 desktop 的 `Exec=firefox %u` 命中 `/usr/local/bin/firefox`） |
| 安装前缀 | `/opt/mozilla-firefox/{bin,lib,src}` |
| 属主 | 全部 `root:root` |

`eglrun` 依赖说明：纯 Wayland 输入法链路（KWin `InputMethod=wayland-im-shim` + CPIS/fcitx
自愈服务）由它部署，属合成器侧全局配置，Firefox 启动器自身不挂输入法垫片。

## 2. 最终架构（定稿）

```
用户 / desktop (Exec=firefox %u)
        │
        ▼
/usr/local/bin/firefox  ── 启动器 ────────────────────────────────
   ├─ 动态 DPI：FF_DPI_OVERRIDE → DDE scale-factor → Xft.dpi/96 → GNOME → 1.0
   ├─ GTK_THEME ← gtk-3.0/settings.ini（夜间模式）
   ├─ user.js / userChrome.css：硬件加速 + UI 偏好（每次写，首启也生效）
   ├─ LD_LIBRARY_PATH = egl-huawei-v12 : mesa-egl-glvnd
   ├─ LD_PRELOAD = wllog + csd + wheel + gbm-hw + gfxtest_env
   ├─ HISI_FF_LIBGFX = patch 掉析构 UAF 的 libGFX_hisi 副本
   ├─ GDK_BACKEND=wayland / MOZ_ENABLE_WAYLAND=1 / GPU 伪装 / VAAPI(vacore)
   └─ 沙箱关闭（与私有 shim、华为设备节点不兼容）
        │  exec（无字节补丁）
        ▼
/opt/mozilla-firefox/bin/firefox-bin  ── 原厂 AppImage，原样运行 ──
        │
        ├─ 主进程：WebGL / WebRender 走 Maleoon 910 硬渲染，CSD 自绘三按钮
        └─ gfxtest_env_shim 在 exec/spawn 边界给 gfxtest 探测子进程换环境
              （egl-fbspoof + Mesa 真库 + drm-shim）→ VAAPI/H264 探测通过
```

## 3. 能力清单（已验证）

- 原生 Wayland + Firefox 自绘 CSD 三按钮（最小化/最大化/关闭）
- WebGL / WebRender 使用 Maleoon 910 硬件渲染
- H.264 VAAPI 真硬解（vacore / vadec，终端可见输出）
- `about:support` 不崩；退出无段错误
- 首次语言引导 → 自动下载中文语言包；中文界面稳定
- 夜间模式跟随系统；DPI 动态跟随；滚轮修复
- 纯 Wayland 输入法（依赖 eglrun）

**WebGL 固定验证页**（不再更改）：
`https://threejs.org/examples/#webgl_animation_keyframes`

## 4. 设计原则

1. **原厂 AppImage 原样运行**：不解包、不持久解包；所有加速在环境层完成，二进制零改动。
2. **纯垫片实现**：WebGL/WebRender、VAAPI 硬解、Wayland 输入法、CSD 三按钮、滚轮修复
   全部通过 `LD_PRELOAD` / `LD_LIBRARY_PATH` / 子进程环境注入完成，不修改任何厂商文件。
3. **gfxtest 环境注入**：在 exec/spawn 边界给 GL/编解码探测子进程注入等价环境，
   使 VAAPI/H264 探测通过；挂载点原厂二进制原样执行。
4. **语言引导与 user.js 解耦**：语言由 OS locale + langpack 自动触发；首启状态只决定
   "是否传 URL"（带 URL 会跳过 about:welcome）。

## 5. 运行时开关速查

| 变量 | 作用 |
|---|---|
| `FF_DPI_OVERRIDE=1.25` | 强制指定 DPI 缩放（特殊机器兜底） |
| `FF_REPLAY_FIRST_RUN=1 firefox` | 已有 profile 上重放首次运行向导（清首启标记、不动语言与加速偏好） |
| `FF_EXTRA_ARGS="..."` | 追加任意 Firefox 命令行参数 |

## 6. 已知边界

- 夜间模式、DPI 与指针尺寸只在**启动时**读取系统设置；运行中切换需重启 Firefox。
- `FF_REPLAY_FIRST_RUN=1` 重放向导时，语言包已装、语言已匹配，语言引导卡片可能不再出现；
  向导其余卡片照常。
- 垫片中的路径固化在 `/opt/mozilla-firefox`，不可迁移前缀。
- 未安装 eglrun 时 dpkg 会报依赖缺失；输入法首次需注销重登一次让 KWin 读取配置。

## 7. 垫片与指针缩放实现

- `lib/wheel_shim.so`：自包含实现，无 `wl_*` 外部符号依赖；ABI 与滚轮行为不变。
  源码 `src/wheel-shim/`（含自带头 `wayland_mini.h`）。
- `usr/local/bin/firefox` + `src/firefox-launcher.sh`：每次启动按 `FF_DPI` 把光标尺寸
  （`24×DPI`）写入 GTK 标准键 `cursor-size`，指针与界面缩放一致；`FF_DPI_OVERRIDE`
  可手动覆盖。逻辑可移植，换机器/改缩放自动适配。

---

配套文档：`README.md`（使用）、`RESEARCH.md`（研究与排错全过程）。源码在 `src/`。
