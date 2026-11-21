# UOS20 / Kirin 9000c / Maleoon 910 — 浏览器图形加速适配

本仓库收录在 **UOS 20 / 华为海思 Kirin 9000c / Maleoon 910（aarch64）** 上，让 Chrome 157 与
Firefox 160 获得真硬件 WebGL 渲染、硬件画面合成与 H.264 VAAPI 硬解的三个最终交付组件：

| 组件 | 最终版本 | 作用 |
|---|---|---|
| **eglrun** | 2.0.2 | Chrome / Electron 通用 GPU 启动器（X11 + ANGLE/Vulkan 硬件合成 + VAAPI 修复） |
| **uoshim-wayland-im** | 1.2 | 独立的 KWin 原生 Wayland 输入法桥（搜狗 / fcitx） |
| **firefox.appimage** | 160.0-2 | 原厂 Firefox AppImage + 硬件加速垫片包 |

目标平台的关键约束：华为深度定制的图形栈（`libEGL` / `libGLESv2` / `libgbm` 被替换为
`libGFX_hisi`，无标准 GLVND 符号；Maleoon 的 Vulkan 由私有 `libhvgr.so` 提供，缺若干标准扩展；
VAAPI 由 `vacore` 驱动提供但只报告 VA-API 1.14）。因此所有加速都在启动器 + 垫片层完成，
**不修改任何上游二进制的字节**。

## 三个组件

### eglrun 2.0.2

接收一个 Chrome / Electron 可执行文件路径，为它应用已验证的 **X11 + ANGLE/Vulkan** GPU 栈后原样 `exec`。
拦截只有两层：

- **Vulkan ICD wrapper**（`lib/icd-wrap.so`，经 `VK_ICD_FILENAMES` 加载）：补齐 Maleoon 缺失的
  `VK_EXT_provoking_vertex` 等能力（否则 ANGLE 会把 WebGL2 降为 WebGL1），并用真驱动导出 dma-buf。
- **VAAPI 垫片**（`lib/vaapi-shim.so`，经 `LD_PRELOAD` 加载）：把 `vacore` 报告的 VA-API 1.14 提升为
  1.17 以通过 Chrome 的版本门槛；`dlsym` 钩子只包裹 `vaInitialize`，其余符号透传。

```sh
eglrun /opt/google-chrome/chrome                      # 启动 Chrome
eglrun /opt/google-chrome/chrome https://example.com  # 附带 URL
eglrun /opt/some-electron-app/app --flag value        # 启动 Electron 应用
```

显示、DPI、用户数据目录均来自调用方环境或应用默认值。详见 [`eglrun/opt/eglrun/README.md`](eglrun/opt/eglrun/README.md)。

### uoshim-wayland-im 1.2

通过 KWin 的 `[Wayland] InputMethod` 启动一个 wayland-im-shim 客户端，桥接 UOS 的 fcitx / 搜狗输入法，
实现纯 Wayland 会话下的中文输入。以 `.desktop` 描述符方式注册（KWin 5.27 只认 desktop 文件，裸可执行路径会被忽略），
CPIS uinput DBus 策略由 postinst 安装。与 eglrun 完全解耦。详见 [`uoshim/`](uoshim/)。

### firefox.appimage 160.0-2

原厂 Firefox AppImage **原样运行**，全部加速靠 `LD_PRELOAD` / `LD_LIBRARY_PATH` / 子进程环境注入实现：
原生 Wayland + 自绘 CSD 三按钮、WebRender / WebGL 硬件渲染（Maleoon 910）、H.264 VAAPI 真硬解。
详见 [`firefox/opt/mozilla-firefox/README.md`](firefox/opt/mozilla-firefox/README.md) 与
[`RESEARCH.md`](firefox/opt/mozilla-firefox/RESEARCH.md)。

`wheel_shim.so` 为自包含实现：`wl_proxy_get_interface` / `wl_pointer_interface` 由内置逻辑
提供，不依赖 libwayland 的符号导出，可跨 libwayland 版本运行。ABI 与原版一致，滚轮行为不变。
源码在 `src/wheel-shim/`，自带 `wayland_mini.h`，无需 libwayland-dev 即可重建。

启动器每次启动读取系统缩放（`FF_DPI`），换算 `24 × DPI` 写入 GTK 标准键
`org.gnome.desktop.interface cursor-size`（2x → 48px），指针与界面缩放始终一致。

包内所有文件与目录属主均为 `root:root`。

## 目录结构

```
.
├── eglrun/                       eglrun 2.0.2 完整构建树（DEBIAN 脚本 + bin + lib + src + README）
│   └── opt/eglrun/
│       ├── bin/eglrun             启动器
│       ├── lib/                   icd-wrap.so / maleoon-wrap.json / vaapi-shim.so
│       └── src/                   两份 .so 的 C 源码 + 重建脚本
├── uoshim/                       uoshim-wayland-im 1.2 构建树（DEBIAN + bin + config + src/ime）
├── firefox/                      Firefox 160 垫片包构建树
│   └── opt/mozilla-firefox/
│       ├── README.md / RESEARCH.md / FINAL.md
│       ├── lib/                   各垫片 .so + 华为 EGL/Mesa 分流库
│       └── src/                   全部垫片 C 源码 + 启动器源码
├── releases/                     最终 deb 安装包（两个 >100MB 的用 Git LFS 存储）
├── docs/                         开发过程文档与逐日工作日志
│   ├── DEVELOPMENT.md            开发主线：关键转折与结论
│   ├── CHROME果冻问题-完整交接文档.md
│   ├── DELIVERY-*.md
│   └── devlog/                   2026-11-16 ~ 11-21 原始工作日志
└── CHECKSUMS.md                  最终 deb 的 MD5
```

## 安装

```sh
sudo dpkg -i releases/uoshim-wayland-im_1.2_arm64.deb
sudo dpkg -i releases/eglrun_2.0.2_arm64.deb
sudo dpkg -i releases/google-chrome-eglrun_157.0.8103.78-2_arm64.deb   # Chrome（大文件，需 git lfs pull）
sudo dpkg -i releases/firefox.appimage_160.0-2_arm64.deb               # Firefox（大文件，需 git lfs pull）
```

克隆后先拉取 LFS 对象：`git lfs pull`。所有安装文件与目录属主为 `root:root`。

## 硬件加速验证判据

- 浏览器 GPU/图形状态：`gpu_compositing` / `webgl` / `rasterization` / `video_decode` 均为 `enabled`。
- WebGL renderer 显示为 `ANGLE (ARM, Vulkan ... Maleoon 910)`（Chrome）或 Maleoon 910 / HUAWEI（Firefox）。
- GPU 进程映射了 `libhvgr.so`、`icd-wrap.so`（Chrome）与 `vaapi-shim.so`，**没有** SwiftShader。
- GPU 进程持有 dma-buf 文件描述符（`/proc/<pid>/fd` 中的 `dmabuf:*`）——这是硬件合成的硬判据。
- 固定 WebGL 验证页：`https://threejs.org/examples/#webgl_animation_keyframes`。

## 开发过程

见 [`docs/DEVELOPMENT.md`](docs/DEVELOPMENT.md) 与 [`docs/devlog/`](docs/devlog/) 下的逐日原始日志，
记录了从 Wayland 相位锁定、EGL/GBM 路线，到最终确定 X11 + ANGLE/Vulkan + ICD wrapper 的完整探索、
关键证伪与端到端验证数据。

## 许可与二进制说明

上游 Chrome / Firefox AppImage、华为 `libhvgr` / `libGFX_hisi`、Mesa 库等二进制的版权归各自所有者，
本仓库仅收录为在本机适配而制作的垫片源码与打包产物，供个人备份与技术交流使用。
