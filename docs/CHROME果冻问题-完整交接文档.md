# Chrome / Maleoon 910 画面果冻问题 —— 跨模型完整交接文档

> 生成时间：2026-11-20 14:58；15:23 补充「附录 A：三个生产垫片完整源码」使其自包含。
> **基线版本校正（2026-11-20 修订）**：正文所有版本号已统一为当前系统实际安装的 **eglrun 1.0.11**（`dpkg -l eglrun` 与 `/opt/eglrun/VERSION` 均确认）。此前文中出现的 "1.0.17 aggressive 档" 属于历史实验包，从未成为当前系统基线；附录内嵌源码 md5 已逐一核对，与系统 1.0.11 源码**完全一致**（gbm 无 glFinish、wire 无自适应参数/默认固定 4ms 延迟）。请以 1.0.11 为唯一事实基线。
> 用途：把本任务的**全部上下文**交接给另一个 AI 模型，使其读完即可独立接手，不重复已走过的死路。
> 阅读约定：本文把信息分为三类 —— **【铁证】**＝有实测数据支撑，请勿凭推理推翻；**【结论】**＝基于铁证的判断，可质疑但请先复现数据；**【待探索】**＝尚未走过的方向，留给接手者。
> 用户最新明确指示：**不要 fence 路线**，基于**当前系统已安装的 eglrun** 继续找新方法。
>
> **⚠️ 交接对象为网页 AI，无法读取本机任何文件。本文档已完全自包含：**
> - 三个生产垫片的**完整源码全文**见 **附录 A**（`gbm-hw-shim.c` / `cursor-wire-shim.c` / `wire-damage-shim.c` / `build.sh`），无需访问 `/opt/eglrun/src/`。
> - 去果冻垫片的 A/B 启动脚本全文见 **附录 B**。
> - 文中出现的 `/opt/...`、`~/Desktop/...`、`/tmp/...` 等路径仅为历史记录与操作指引；网页 AI 无法读取，请勿尝试。你只能**分析/给方案**，实际改文件、编译、启动需由用户在本机手动执行。
> - 日志数据（wire-trace、glFinish 对比等）的关键数值已在 §5 直接给出，无需读取 `/tmp/*.log`。

---

## 0. 一句话现状

在一台 UOS 20 / Kirin 9000c / Maleoon 910 的国产平台上，Firefox 160（走 EGL window surface + 驱动内部闭环）画面丝滑不抖；Chrome 157（走 ANGLE surfaceless → GBM/dma-buf → Wayland dmabuf，跨进程无 fence）在滑动时出现**果冻 / 残留 / 闪烁**。系统当前安装的是 eglrun **1.0.11**（3 个垫片），能把抖动"大幅度减轻"但未根治。所有显式/隐式 fence、KWin 同步、flag toggle、DPI 路线均已被数据证伪或引发崩溃。

---

## 1. 硬件与系统环境（精确，已实测）

| 项 | 值 |
|---|---|
| 系统 | UOS 20（DDE 深度桌面） |
| CPU / GPU | Kirin 9000c / Maleoon 910（华为自研，ARM64） |
| 内核 | `5.10.97-arm64-desktop-full` |
| 屏幕 | 2880×1920，DDE `scale-factor = 2.0` |
| 合成器 | KWin 5.27（DDE 定制版，`/usr/bin/kwin_wayland_wrapper -platform wayland-org.kde.kwin.qpa`），**与华为驱动高度耦合，不可升级** |
| Chrome | `Google Chrome 157.0.8103.78`，AppImage 原样运行 |
| Firefox（对照组，不抖） | 160.0 deb，见 §9 |

---

## 2. 当前系统安装状态（唯一可信基线 —— 一切以此为准）

### 2.1 安装布局
```
/usr/local/bin/eglrun  -> /opt/eglrun/bin/eglrun       (符号链接)
/opt/eglrun/bin/eglrun                                 主启动器
/opt/eglrun/bin/env.sh                                 环境变量 + LD_PRELOAD 拼装
/opt/eglrun/lib/cursor-wire-shim.so                    光标大小修复
/opt/eglrun/lib/gbm-hw-shim.so                         GBM ABI 桥接 + VAAPI 伪装 + stride 修复（1.0.11 无 glFinish）
/opt/eglrun/lib/wire-damage-shim.so                    逐帧提交延迟（"果冻垫片"）
/opt/eglrun/src/cursor-wire-shim.c                    ↑ 三个生产垫片的源码（系统内随包安装，可改可重编译）
/opt/eglrun/src/gbm-hw-shim.c
/opt/eglrun/src/wire-damage-shim.c
/opt/eglrun/src/build.sh                              源码编译脚本（见下方注意）
/root/... (无)    KWin helper 未被任何 fence 垫片注入（已核实 kwin_wayland_helper 无 KWF 标记）
```

> **三个生产垫片的源码全文已内嵌在本文「附录 A」**——网页 AI 无法读取本机文件，请直接看附录 A，不要试图访问 `/opt/eglrun/src/`。
> **注意**：系统现存 `build.sh` 只编译 gbm + cursor + wayland-im，**漏了 wire-damage-shim**（1.0.11 官方包即如此）。要重编译 wire-damage 需自行加：
> `gcc -shared -fPIC -O2 -ldl -o ../lib/wire-damage-shim.so wire-damage-shim.c`

### 2.2 MD5（改动前务必记录，用于判断是否被污染）
```
8d6f7e58c22043aae0e52333c61df129  /opt/eglrun/bin/eglrun
3205c4402b083790e9ca882f4598d80b  /opt/eglrun/bin/env.sh
ad0adfb7a14aed59f7f09c8c1f19dbb3  /opt/eglrun/lib/cursor-wire-shim.so
34d0adb6cef8ad32b8ae1e8883678a00  /opt/eglrun/lib/gbm-hw-shim.so
f8cf0ea18dc7ff0d00114786b0f65abd  /opt/eglrun/lib/wire-damage-shim.so
6a8068b79da54778c94e66d9534504d0  ~/Downloads/Google-Chrome-stable-157.0.8103.78-1-aarch64.AppImage
52842629a8daceda9ad8f8b5d3b63a36  ~/Desktop/firefox-abtest/deb-build/firefox.appimage_160.0_arm64.deb
```

### 2.3 实际传给 Chrome 的完整命令（`eglrun --dry-run --chrome` 原文）
```
LD_PRELOAD=/opt/eglrun/bin/../lib/wire-damage-shim.so:/opt/eglrun/bin/../lib/cursor-wire-shim.so:/opt/eglrun/bin/../lib/gbm-hw-shim.so
CMD: /opt/google-chrome/chrome \
  --use-angle=gl \
  --enable-gpu-compositing \
  --render-node-override=/dev/dri/renderD128 \
  --disable-features=AcceleratedVideoDecodeLinuxZeroCopyGL \
  --enable-features=VaapiVideoDecoder,VaapiIgnoreDriverChecks \
  --enable-wayland-ime --wayland-text-input-version=1 \
  --force-device-scale-factor=2.0 \
  --class=google-chrome
```
（新 profile 且无已存窗口矩形时，eglrun 额外加 `--window-size=1200,800 --window-position=100,100`）

### 2.4 三个垫片的作用与固化参数
- **wire-damage-shim.so**：在 Wayland 字节流（Chrome 内嵌 libwayland，只能 syscall 级拦截）层，对每个 commit 前插入延迟。**当前 1.0.11 是固定延迟**：`env.sh` 里 `export WIRE_COMMIT_DELAY_US="${WIRE_COMMIT_DELAY_US:-4000}"`（默认 4000µs，可用环境变量覆盖），`WIRE_DAMAGE_FULL=0`（关掉 damage 扩全屏，实测有害）。源码 `opt_delay_us()` 只读这一个环境变量、**没有任何自适应/gain/alpha/headroom 参数**。此前所称的 `DELAY_BASE_US=2500, DELAY_MAX_US=9000, DELAY_GAIN=0.15, DELAY_HEADROOM_US=2500, DELAY_ALPHA=0.20, MIN_GAP_US=2000` 属于未安装在系统的历史实验版 1.0.17，不适用于当前基线。**原理是盲等时序，不真正知道 buffer 何时写完 → 只能缓解不能根治。**
- **gbm-hw-shim.so**：① HUAWEI GBM ABI 桥接（缺它窗口无法 map）；② 把 vacore 的 VAAPI 1.14 伪装成 1.17 过 Chrome gate；③ `repair_stride()` 修正 HUAWEI 对 HISI-modifier bo 报出的错误 stride（应为 width*4）。**当前 1.0.11 的 gbm 源码里没有 `glFinish()` 调用**（已 grep 核实）；所谓"getfd 点调 ANGLE glFinish、2ms 节流"是历史实验版行为，不在当前系统。源码另有一处已知但无害的小缺陷：`repair_stride()` 的守卫 `if (!bo || got >= 4) return got;` 会让 got=32 这类错误 stride 因 `>=4` 被当成合理值直接返回、实际不修复——网页 AI 指出属实，但当前系统保持原样，勿据此假设 stride 已被修正。
- **cursor-wire-shim.so**：Wayland 字节流层把光标的 `set_buffer_scale(2)` 改成 1（修光标过大）。与果冻无关。

### 2.5 启动链路
用户通过 `eglrun` 启动 Chrome（桌面图标 / 命令行）。AppImage 原样运行，runtime 会把 `LD_PRELOAD` 与 `LIBVA_*` 透传给内部 Chrome，垫片生效，无需解包。

---

## 3. 问题定义（用户最终澄清的精确症状，勿用旧描述）

**未加果冻垫片时（去掉 wire-damage-shim）的状态：**
1. **快速上下/左右滑动页面**：文字、图片**残留**；鼠标移动后消失。
2. 上述文字残留**只在 `chrome://gpu` 页面**出现。
3. **Chrome 自身 UI（标签栏/地址栏/菜单）会闪烁**。
4. **WebGL（three.js）与视频有果冻状抖动**。

**固定验证页**：`https://threejs.org/examples/#webgl_animation_keyframes`（LittlestTokyo GLTF 动画场景，用户强制约定，不再更改）。
**用户观感判据**：59–60fps 流畅；一旦掉到 55fps 以下果冻感出现；40fps 左右果冻明显。
**注意**：不全屏也抖；Vulkan 已测（勿重复）；锁 30fps 不可接受。

---

## 4. 最关键的对照组：Firefox 为什么不抖（理解这个问题的钥匙）

- **Firefox 不抖路径**：HUAWEI **EGL window surface** + `maleoon_buffer_sharing v5` + `wl_surface.attach`，**驱动内部闭环同步**。
- **Chrome 抖路径**：ANGLE GL **surfaceless** → GBM **dma-buf** → `zwp_linux_dmabuf_v1.create_immed`，**跨进程**（GPU 进程写 buffer、Browser 进程 commit），**无 fence 握手**。

**【结论】** 两类现象同源但不同进程路径：chrome://gpu 残留 + Chrome UI 闪烁 = Browser 进程 UI 合成 → KWin 扫描；WebGL/视频果冻 = GPU 进程 dma-buf → KWin 扫描。核心差异是"写入完成"与"合成器扫描"之间缺握手。这是理解所有后续数据的框架。

---

## 5. 铁证与数据（请勿凭推理推翻，要质疑先复现）

### 5.1 【铁证】damage 零漏报（wire-trace 垫片实测）
写了只读 Wayland 字节流垫片记录 `ATTACH→DAMAGE→COMMIT` 时序：
- 普通网页 scrolltest.html（纯 DOM 文字+色块，516 帧）：每帧严格 `ATTACH→DAMAGE→COMMIT`，**零漏报**，全屏 damage `w=2036 h=3534`，buffer 三缓冲轮转（52/53/57）。commit 间隔 p50=16.6ms / p90=19ms / p99=63ms，254/560 帧超一帧预算。
- chrome://gpu 真实滚轮（1026 帧）：三缓冲 53/58/55，每帧全屏 damage，commit 间隔 p90=21.6ms，527/1025 帧超预算。
- **【结论】** 残留/果冻**不是 damage 漏报**；且纯 DOM 低负载页面也抖，**否定了"2x DPI 渲染负载超预算"这一单一归因**（这个错误结论曾被得出过，已推翻）。

### 5.2 【铁证】两个 glFinish 天差地别（同 context 对比）
- **ANGLE glFinish 真实有效**：重帧能等 23ms、31ms，极端 133930µs。轻帧 0–6µs 是"无活可等"的正确行为。
- **华为原生 libGFX glFinish 空转**（1–12µs）：独立 `dlopen` 的实例看不到 ANGLE 的 command buffer。**"hw 模式"已证伪，勿用。**
- **【结论·仅适用于曾做过该实验的版本】** 在 getfd 点调 ANGLE glFinish 是真实有效的；"glFinish 打错对象/空壳失效"的推测站不住。**但注意：当前系统 1.0.11 的 gbm-hw-shim 并未包含 getfd glFinish（§2.4），本节数据来自历史诊断垫片 nbf-shim，不是当前基线行为，接手者勿据此认为系统已在做 GPU 同步。**

### 5.3 【铁证】1x DPI 全程 60fps 无果冻（但不可用）
- 1x DPI 下 glFinish 峰值仅 833µs，>1000µs 计数为 0；用户实测全程 60fps、无果冻。
- **代价**：UI/文字变小；**最大化窗口崩溃**（SIGABRT + `libwayland: listener function for opcode 11 of wl_pointer is NULL`，诊断钩子已排除为崩溃因）。
- **【结论】** 负载降低确实能消果冻，但全局降 DPI 这条路因 UI + 崩溃双重代价堵死。

### 5.4 【铁证】dma-buf 隐式 fence 不存在且反相关（fence-diag，18 组配对）
- angle glFinish=133930µs（GPU 狂忙）时，dma-buf SYNC 等待仅 4µs；angle=7750µs 时 dmabuf=2µs；反之 dmabuf=677µs 时 angle 仅 14µs。
- 统计：dmabuf p50=11 / p90=742 / max=762 µs；angle p50=6 / p90=2336 / max=133930 µs。
- **【结论】** 华为驱动**没有把渲染 fence 挂到 dma-buf reservation object**；`DMA_BUF_IOCTL_SYNC` 等的 ~700µs 是别的固定开销。

### 5.5 【铁证】显式 fence 四端全断
| 环节 | 需要 | 本机现状 |
|---|---|---|
| 内核造 fence | `CONFIG_SW_SYNC` / `/dev/sw_sync` | **关闭、无节点** |
| fence 转跨进程 fd | `DMA_BUF_IOCTL_IMPORT/EXPORT_FENCE` | **5.10 内核没有** |
| 驱动导出 fence | `eglDupNativeFenceFDANDROID` | **华为 libGFX 没有**（`glFenceSync`/`eglCreateSyncKHR` 有符号但产不出可用 fence） |
| KWin 消费 fence | `wp_linux_drm_syncobj` / `linux_explicit_synchronization_v1` / `wp_presentation` | **KWin 5.27 全无** |

**【结论】** 用户态无法"写一个 fence"对象交给 KWin。用户已据此**明确否决 fence 路线**（含跨进程软件 fence 方案）。

### 5.6 【铁证】flag A/B 定量排名不可行（CDP rAF 采样）
纯标准库 CDP 采样 rAF 帧间隔，同一 base 配置连跑三次：**54.9 → 43.0 → 37.0 fps**（over33：48→123→166）。机器状态波动远大于任何 flag 差异。`no-rendernode` 的"60fps"是 GPU 进程崩溃、rAF 空转的假象。
**【结论】** 不要用自动 rAF 采样给 flag 排名；果冻最终必须用户肉眼判定。视频相关 flag（ZeroCopy/Vaapi）只作用于 `<video>`，three.js 是纯 WebGL canvas，完全不经过 → 可排除。

---

## 6. 已证伪 / 已否决路径总表（禁止重复劳动）

| # | 路径 | 结论 | 证据 |
|---|---|---|---|
| 1 | Vulkan | 用户已测，无效 | 用户明确勿重复 |
| 2 | 锁 30fps | 不可接受 | 用户否决 |
| 3 | 升级 compositor/KWin | 不可行 | 破坏 Firefox maleoon 闭环；用户否决 |
| 4 | X11 路径 | 证伪 | `DRM buffer errno=-22` / `fourcc format invalid` |
| 5 | `--in-process-gpu` | 证伪 | `Failed to get system egl display` / `eglInitialize OpenGL failed` |
| 6 | `UseBeginFramePresentationFeedback` | 不可用 | KWin 无 `wp_presentation` |
| 7 | KWin pageflip 点调 glFinish | 空转 | KWin context 看不到 Chrome 渲染，约 3µs |
| 8 | 显式 fence（4 端） | 全断 | §5.5 |
| 9 | dma-buf 隐式 fence | 不存在 | §5.4 反相关 |
| 10 | flag toggle 定量排名 | 方法证伪 | §5.6 噪声 >> 信号 |
| 11 | stages / stages-max / waitable | 果冻仍在或最大化崩溃 | `--run-all-compositor-stages-before-draw`、`--enable-waitable-swap-chain` |
| 12 | NBF/NADAPT 12ms sleep | 制造闪黑 | maximize 时 3+ 新 buffer 各睡 12ms 累积阻塞扫描，已删 |
| 13 | 华为原生 glFinish | 空转 | §5.2 |
| 14 | 全局 1x DPI | 消果冻但 UI 变小+最大化崩溃 | §5.3 |
| 15 | 跨进程软件 fence（KWin 垫片） | 用户最新否决 | 用户："不要 fence" |

---

## 7. 曾尝试但有部分效果 / 需谨慎

- **当前系统 wire-damage 固定 4ms 延迟档（1.0.11）**：用户评价"抖动大幅度减轻"，是现有最佳稳定状态。曾在 59–60fps 流畅，但掉帧到 55 以下果冻仍现。历史实验版 1.0.17 的 aggressive 自适应档曾带来更好观感，但该版本**已回滚、未安装在当前系统**，不要假设它在生效。
- **8ms / 8000 系列参数**：曾"有缓解但仍会抖"（历史反馈）。
- **chrome-nojelly.sh**（`~/Desktop/chrome-nojelly.sh`）：去掉 wire-damage、只留 cursor + gbm 的启动脚本（因无 root 权限改 env.sh 而用等效方案）。用户正在用它做"去果冻垫片"的 A/B 验证。注意：脚本曾因 `exec` 写在 `env` 后报错，已修复。
- **KWin 垫片风险记录**：早期 epoxy 指针注入版曾导致 KWin 无限崩溃，用户进 tty 执行 uninstall 才恢复；后来的 libdrm hook 版（kwin-pageflip）验证安全。用户表示可以承担 KWin 垫片崩溃风险（可进 tty 卸载），但**当前已否决 fence 方向**，不要擅自再注 KWin。

---

## 8. 用户硬约束（必须遵守）

1. **原厂 AppImage 原样运行，零字节补丁**；不要解包改二进制、不要热补丁。
2. **不要源码重编译 Chrome**。
3. **不要反复自动实测**——用户会自测并反馈；自动采样（CDP rAF）已被证明噪声大且误导。
4. **研究阶段不要每次打包**——直接打开配置好的 Chrome 供用户确认，定稿后再统一打包。
5. **Vulkan 已测，勿重复**。
6. **锁 30fps 不行**；**不全屏也抖**。
7. **升级 compositor 不行**（系统与华为驱动高度耦合）。
8. **不要 fence 路线**（最新指示）。
9. **Chrome 不需要滚轮垫片**（滚轮正常）——滚轮垫片 `wheel_shim.so` 是 Firefox 专属，勿加到 Chrome。
10. **不要浪费 token**：先想清楚再动手，优先复用本文数据。

---

## 9. 关键文件与目录索引

```
~/Desktop/chrome-adaptive-shim/            Chrome 研究主目录
├── wire-trace/                            Wayland damage/commit 抓取垫片 + 滚动测试页（§5.1 工具）
│   ├── wire-trace.c / .so
│   ├── trace_scroll.py / trace_real_scroll.py
│   └── scrolltest.html
├── flag-ab/                               CDP rAF 采样 + flag A/B（§5.6 工具，方法已证伪，仅存档）
│   ├── cdp_raf.py  (纯标准库)
│   └── flagab.sh
├── nbf-shim/                              glFinish / 双 finish / 1x DPI / dma-buf fence 诊断（§5.2/5.3/5.4）
│   ├── gbm-hw-shim-nbf.c / .so            clean 版：context 检查 + 零 sleep
│   ├── gbm-hw-shim-fencediag.c / .so      dma-buf SYNC 诊断
│   ├── nbf-test.sh  (diag/diag1x/smooth1x/clean/off/stop)
│   └── fence-diag.sh
├── fence-probe/                           EGL fence / glFinish 独立探测
├── kwin-pageflip/                         KWin libdrm hook（glFinish 空转，已证伪；libdrm 版安全）
├── fence-shim/  fence-shm/                【用户已否决】跨进程软件 fence 实验，勿接入系统
├── glfinish-diag/  eglrun-*/  chrome-x11-probe/  等其他历史实验（存档）
├── eglrun_1.0.11_arm64.deb                当前系统基线包（md5 7e61f85e4926af0aec543fdb3c09d0b5，在 ~/Desktop/）
└── eglrun_1.0.17_arm64.deb                历史实验版（已回滚，非当前基线；md5 cacc9ca168ac2724f0f28271683274e6）

~/Desktop/chrome-nojelly.sh                去掉 wire-damage 的等效启动脚本（A/B 用）
~/Desktop/firefox-abtest/                  Firefox 对照组（不抖的参考实现）
└── deb-build/firefox.appimage_160.0_arm64.deb   md5 52842629a8daceda9ad8f8b5d3b63a36
```

**工作日志（完整研究过程）**：`/home/UOS/WorkBuddy/2026-11-16-20-13-03/.workbuddy/memory/2026-11-20.md`（还有 11-19 及更早）。

---

## 10. 【待探索】给接手模型的新方向（还没走过的路）

以下是基于本文铁证、仍未尝试的方向，按"值得优先"排序。请自行判断可行性，优先纯垫片/配置层，避免触发 §8 硬约束。

1. **只降 WebGL canvas drawing buffer，保 UI 2x 清晰**（此前多次提出但从未实现，最被看好）
   - 目标：只把 three.js 的 canvas drawing buffer 从 2x 降到 1x，UI 文字保持 2x 锐利，不动全局 DPI、不动窗口尺寸（避开最大化崩溃）。
   - 思路：页面注入 / 垫片 hook `window.devicePixelRatio` 或 `renderer.setPixelRatio`，使 three.js `setPixelRatio` 取到 0.5；或用 CDP `Emulation.setDeviceMetricsOverride` 的思路做局部化。与 §5.3（1x 消果冻）的差别在于范围仅限 canvas。
   - 风险：页面注入需解决注入时机（three.js 在模块加载时读 DPR）；可能有站点兼容问题。

2. **削减 dma-buf 轮转深度（triple → double buffer）**
   - §5.1 观测到 buffer 在 3 个间轮转；用户曾反馈"改变窗口大小后不抖"（改尺寸会重建 buffer）。
   - 假设：更浅的轮转降低 KWin 扫到"未写完帧"的概率。
   - 风险：改 gbm 层 buffer 管理有崩溃前科（waitable 最大化崩过）；需谨慎小步验证。

3. **让 Chrome 走 Firefox 的 EGL window surface 路径（仿 maleoon 闭环）**
   - 根本差异在 §4：Firefox 用 EGL surface + 驱动内闭环，Chrome 用 surfaceless + dma-buf。
   - 思路：探索 `--ozone-platform` / `--use-gl` / ANGLE EGL surface 组合，或 EGL 垫片让 ANGLE 用 HUAWEI EGL window surface 而非 surfaceless，绕开跨进程 dma-buf。
   - 这是理论上最治本的方向，但改动大、风险高。

4. **ANGLE swap / frame pacing 行为调整**
   - 系统已有 wire-damage 在 Wayland 层做延迟；可探索 ANGLE 层或 Chrome 合成器的 swap 时机 flag（在不触发 §6 #11 崩溃的前提下）。

5. **页面级 rAF 帧调度整形**
   - 用 CDP/垫片对 three.js 的 rAF 做轻整形（非锁帧），平滑掉帧波动。注意 §5.6：自动采样噪声大，任何效果需用户肉眼确认。

---

## 11. 复现命令速查

```bash
# 查看系统实际启动命令（改任何东西前先跑一次存档）
eglrun --dry-run --chrome /opt/google-chrome/chrome

# 启动系统基线（当前最佳稳定状态）
eglrun ~/Downloads/Google-Chrome-stable-157.0.8103.78-1-aarch64.AppImage \
  'https://threejs.org/examples/#webgl_animation_keyframes'

# 去果冻垫片对照（A/B 用）
~/Desktop/chrome-nojelly.sh 'https://threejs.org/examples/#webgl_animation_keyframes'

# 1x DPI 诊断（会崩最大化，仅诊断）
cd ~/Desktop/chrome-adaptive-shim/nbf-shim && ./nbf-test.sh diag1x

# damage 抓取（§5.1）
cd ~/Desktop/chrome-adaptive-shim/wire-trace && trace_scroll.py <CDP端口> 12

# 校验系统垫片未被污染
md5sum /opt/eglrun/lib/*.so /opt/eglrun/bin/eglrun /opt/eglrun/bin/env.sh
```

**重要**：所有测试请用隔离 profile（`--user-data-dir=/tmp/...`）与独立调试端口；启动新实例前先 `pkill` 清掉旧测试进程，否则新参数会被转发给旧实例而不生效。清理时避免 `pkill -f` 匹配到自身命令。

---

## 12. 交接 checklist（接手模型请先做）

- [ ] 通读 §1–§8，尤其是 §5 铁证与 §6 禁区表。
- [ ] 跑一次 §11 的 dry-run 和 md5sum，确认系统基线未被污染。
- [ ] 与用户确认：当前 wire-damage 固定 4ms 延迟档（1.0.11）是否仍在用、chrome-nojelly.sh 的 A/B 结论。
- [ ] 从 §10 选 1 个方向（建议先 #1 只降 canvas buffer），先说明方案与风险，再动手。
- [ ] 遵守 §8 全部约束；研究阶段不打包、不反复自动实测，交给用户肉眼判定。

---

# 附录 A / B：完整源码（自包含，网页 AI 请直接阅读本节）

> 以下为三个生产垫片 + 编译脚本 + A/B 脚本的**完整源码原文**，从本机 `/opt/eglrun/src/` 与 `~/Desktop/` 逐字读取，未经删改。
> 网页 AI 无法读取本机文件，本附录即源码的唯一交付形式。改源码→编译→替换 so→重启 Chrome 需由用户在本机手动完成。


---

## 附录 A.1  gbm-hw-shim.c（GBM ABI 桥接 + VAAPI 伪装 + stride 修复；当前 1.0.11 无 glFinish）

源码路径（仅历史记录）：`/opt/eglrun/src/gbm-hw-shim.c`

````c
/*
 * gbm-hw-shim.so -- enable Chrome 157 hardware compositing on UOS 20 /
 * Kirin 9000c / Maleoon 910 (HUAWEI libGFX_hisi GBM/EGL).
 *
 * ROOT CAUSE (verified with real stderr):
 *   Chrome's GbmPixmapWayland::InitializeBuffer() calls
 *   gbm_bo_create(w,h,format, flags=Scanout|Rendering|Texturing = 37) for
 *   the real window pixmap. HUAWEI GBM rejects those flags for every RGB
 *   format -> gbm_pixmap_wayland.cc:104 "Cannot create bo" -> GPU process
 *   crash loop -> Chrome silently degrades to software compositing.
 *
 *   HUAWEI GBM CAN create the same buffer via
 *   gbm_bo_create_with_modifiers(..., modifier=0x0900000000000002), which
 *   is the only modifier KWin advertises for XR24/AR24 (verified with
 *   wl-dmabuf-query.c and gbm-fd-test.c).
 *
 * ABI ISSUES BRIDGED:
 *   1. Chromium resolves gbm_* via dlsym(handle,...) after dlopen, so plain
 *      LD_PRELOAD symbol interposition is bypassed. We hook dlsym().
 *   2. gbm_bo_get_fd_for_plane is a WEAK import HUAWEI doesn't export ->
 *      drmPrimeHandleToFD fallback -> EBADF flood. We supply it via
 *      HUAWEI's gbm_bo_get_fd().
 *   3. gbm_bo_get_modifier on HUAWEI bo returns 0; we report KWin's modifier
 *      so Chrome's gbm_bo_import carries a modifier KWin accepts.
 *   4. gbm_import_fd_modifier_data: Chrome writes modifier at byte 56,
 *      HUAWEI reads at byte 64. We write to BOTH in a local copy.
 *   5. JITTER ROOT CAUSE: HUAWEI reports stride=0 / stride_for_plane=32 for
 *      HISI-modifier bos (correct is width*4). Wrong stride -> every row
 *      sampled at the wrong offset -> moving video/WebGL jitters. We repair
 *      implausible strides to width*4. See repair_stride().
 *
 * Toggles: HISI_GBM_SHIM=0 (all off), HISI_GBM_SHIM_DEBUG=1,
 *          HISI_GBM_MODIFIER=0x..., HISI_GBM_CREATE_FIX=0,
 *          HISI_GBM_PLANE_FD=0, HISI_GBM_REPORT_MODIFIER=0,
 *          HISI_GBM_IMPORT_FIX=0, HISI_GBM_STRIDE_FIX=0
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <pthread.h>
#include <sys/stat.h>

typedef struct gbm_bo gbm_bo;
typedef struct gbm_device gbm_device;
union gbm_bo_handle { void *ptr; uint32_t u32; };

typedef int (*fn_plane_fd)(gbm_bo *, int);
typedef int (*fn_gbm_bo_get_fd)(gbm_bo *);
typedef uint64_t (*fn_gbm_bo_get_modifier)(gbm_bo *);
typedef gbm_device *(*fn_gbm_bo_get_device)(gbm_bo *);
typedef int (*fn_gbm_device_get_fd)(gbm_device *);
typedef union gbm_bo_handle (*fn_gbm_bo_get_handle_for_plane)(gbm_bo *, int);
typedef int (*fn_drm_prime)(int, uint32_t, uint32_t, int *);
typedef gbm_bo *(*fn_gbm_bo_create)(gbm_device *, uint32_t, uint32_t,
                                    uint32_t, uint32_t);
typedef gbm_bo *(*fn_gbm_bo_create_with_modifiers)(gbm_device *, uint32_t,
        uint32_t, uint32_t, const uint64_t *, uint32_t);
typedef gbm_bo *(*fn_gbm_bo_import)(gbm_device *, uint32_t, void *, uint32_t);
typedef uint32_t (*fn_gbm_bo_get_stride)(gbm_bo *);
typedef uint32_t (*fn_gbm_bo_get_stride_for_plane)(gbm_bo *, int);
typedef uint32_t (*fn_gbm_bo_get_width)(gbm_bo *);

/* ---- VAAPI version spoof (merged: only ONE dlsym hook allowed) ------- */
typedef int VAStatus;
typedef void *VADisplay;
#define VA_STATUS_SUCCESS 0
static VAStatus (*captured_real_vaInitialize)(VADisplay, int *, int *) = NULL;
VAStatus vaInitialize(VADisplay dpy, int *major, int *minor);

#define KWIN_MOD_DEFAULT 0x0900000000000002ULL
#define GBM_BO_IMPORT_FD_MODIFIER 0x5504

/* Resolve the REAL dlsym via a versioned symbol (proven in vaapi spoof).
 * __libc_dlsym(RTLD_NEXT, ...) segfaults on this glibc. */
static void *(*real_dlsym_fn(void))(void *, const char *) {
    static void *(*r)(void *, const char *) = NULL;
    if (!r) r = (void *(*)(void *, const char *))
            dlvsym(RTLD_NEXT, "dlsym", "GLIBC_2.17");
    return r;
}
static void *real_sym(const char *name) {
    void *(*rd)(void *, const char *) = real_dlsym_fn();
    return rd ? rd(RTLD_NEXT, name) : NULL;
}

__attribute__((constructor)) static void gbm_hw_loaded(void) {
    /* Diagnostic trace is OPT-IN only. Writing one line per process to a
     * shared file and, more importantly, fprintf(stderr) on every create /
     * import / prime call in the render hot path BLOCKS on terminal I/O when
     * Chrome is launched from a TTY. That synchronous logging stalled GPU
     * compositing and made even static pages (chrome://gpu) lag while
     * scrolling. Default off; enable with HISI_GBM_SHIM_DEBUG=1. */
    if (getenv("HISI_GBM_SHIM_DEBUG")) {
        FILE *f = fopen("/tmp/gbm-hw-loaded.txt", "a");
        if (f) {
            fprintf(f, "pid=%d debug=%s shim=%s ldpreload=%s\n", (int)getpid(),
                    getenv("HISI_GBM_SHIM_DEBUG") ?: "NULL",
                    getenv("HISI_GBM_SHIM") ?: "NULL",
                    getenv("LD_PRELOAD") ?: "NULL");
            fclose(f);
        }
    }
    /* Chrome's zygote scrubs these; restore VAAPI env in every child. */
    if (!getenv("LIBVA_DRIVER_NAME")) setenv("LIBVA_DRIVER_NAME", "vacore", 1);
    if (!getenv("LIBVA_DRIVERS_PATH")) setenv("LIBVA_DRIVERS_PATH", "/usr/lib", 1);
    setenv("LIBVA_DRI3_DISABLE", "1", 1);
    /* NOTE: allow_rgb10_configs is intentionally NOT set. It is a stale knob
     * that only emits "default value overridden by environment" noise. */
}

static int env_flag(const char *name, int def_on) {
    const char *v = getenv(name);
    if (!v) return def_on;
    if (strcmp(v, "0") == 0) return 0;
    if (strcmp(v, "1") == 0) return 1;
    return def_on;
}
static int debug_enabled(void) { return env_flag("HISI_GBM_SHIM_DEBUG", 0); }

static uint64_t target_modifier(void) {
    static uint64_t m = 0;
    if (!m) {
        const char *v = getenv("HISI_GBM_MODIFIER");
        m = v ? strtoull(v, NULL, 0) : KWIN_MOD_DEFAULT;
        if (!m) m = KWIN_MOD_DEFAULT;
    }
    return m;
}

/* ---------- fd -> bo cache (HUAWEI gbm_bo_import is a no-op) ------------
 * HUAWEI libGFX's gbm_bo_import() returns NULL for EVERY type (verified with
 * gbmimport.c on this machine). Chrome creates a bo via gbm_bo_create*, gets
 * its dma-buf fd, then calls gbm_bo_import to turn that fd back into a bo for
 * Wayland. We remember every bo we create together with its dma-buf identity,
 * and on import return the ORIGINAL bo instead of calling the broken HUAWEI
 * function.
 *
 * IMPORTANT: HUAWEI gbm_bo_get_fd() returns a NEW fd on EVERY call (verified
 * with diagnostics/gbm-fd-inode.c: 4 calls -> fd 5,6,7,8 but the SAME st_ino).
 * Keying the cache by fd number therefore NEVER matches the fd Chrome passes
 * to gbm_bo_import -> every import fell back to a freshly-created empty bo,
 * which shows up as frame jitter/tearing. Key by (st_dev, st_ino) instead.
 */
#define CACHE_MAX 256
static struct { dev_t dev; ino_t ino; gbm_bo *bo; uint32_t w, h, fmt; } bo_cache[CACHE_MAX];
static int bo_cache_n;
static pthread_mutex_t cache_lock = PTHREAD_MUTEX_INITIALIZER;

static void cache_bo(gbm_bo *bo, uint32_t w, uint32_t h, uint32_t fmt) {
    if (!bo) return;
    static fn_gbm_bo_get_fd gfd;
    if (!gfd) gfd = (fn_gbm_bo_get_fd)real_sym("gbm_bo_get_fd");
    int fd = gfd ? gfd(bo) : -1;
    if (fd < 0) return;
    struct stat st; memset(&st, 0, sizeof(st));
    int fr = fstat(fd, &st);
    close(fd);                 /* it's a dup; don't leak one per create */
    if (fr != 0) return;
    pthread_mutex_lock(&cache_lock);
    /* refresh an existing inode entry (same bo re-created) else append */
    int slot = -1;
    for (int i = 0; i < bo_cache_n; i++)
        if (bo_cache[i].dev == st.st_dev && bo_cache[i].ino == st.st_ino) { slot = i; break; }
    if (slot < 0 && bo_cache_n < CACHE_MAX) { slot = bo_cache_n++; }
    if (slot >= 0) {
        bo_cache[slot].dev = st.st_dev;
        bo_cache[slot].ino = st.st_ino;
        bo_cache[slot].bo  = bo;
        bo_cache[slot].w = w; bo_cache[slot].h = h; bo_cache[slot].fmt = fmt;
    }
    pthread_mutex_unlock(&cache_lock);
    if (debug_enabled())
        fprintf(stderr, "[gbm-hw] cache ino=%llu fd=%d bo=%p %ux%u fmt=0x%x\n",
                (unsigned long long)st.st_ino, fd, (void *)bo, w, h, fmt);
}
/* Look up the original bo for an imported dma-buf fd.
 *
 * Keying by (st_dev, st_ino) alone is NOT sufficient on HUAWEI libGFX:
 * gbm_bo_get_fd() hands out a NEW dup every call, and the kernel recycles
 * dma-buf inodes aggressively when Chrome streams many scroll tiles. If the
 * inode is reused, an import of a fresh bo can match a STALE cache entry and
 * get back an old bo -> a tile keeps showing scrolled-away content until a
 * mouse hover forces Chrome to repaint it.
 *
 * FIX: additionally require the imported geometry (w/h/fmt) to match the
 * cached entry, and prefer the newest matching slot (highest index). Callers
 * that don't know the geometry pass w=h=0, which keeps the old behaviour.
 */
static gbm_bo *lookup_bo(int fd, uint32_t w, uint32_t h, uint32_t fmt) {
    if (fd < 0) return NULL;
    struct stat st; memset(&st, 0, sizeof(st));
    if (fstat(fd, &st) != 0) return NULL;
    gbm_bo *found = NULL;
    int found_slot = -1;
    pthread_mutex_lock(&cache_lock);
    for (int i = 0; i < bo_cache_n; i++) {
        if (bo_cache[i].dev != st.st_dev || bo_cache[i].ino != st.st_ino)
            continue;
        /* geometry guard: reject stale inode reuse with a different buffer */
        if (w && h && (bo_cache[i].w != w || bo_cache[i].h != h ||
                       bo_cache[i].fmt != fmt))
            continue;
        found = bo_cache[i].bo;
        found_slot = i;          /* keep scanning: newest match wins */
    }
    pthread_mutex_unlock(&cache_lock);
    if (debug_enabled() && found)
        fprintf(stderr, "[gbm-hw] lookup fd=%d ino=%llu -> slot=%d bo=%p%s\n",
                fd, (unsigned long long)st.st_ino, found_slot, (void *)found,
                (w && h) ? " (geo-checked)" : "");
    return found;
}

/* ---------- wrapper: gbm_bo_get_fd_for_plane --------------------------- */
int gbm_bo_get_fd_for_plane(gbm_bo *bo, int plane) {
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_PLANE_FD", 1)) {
        fn_plane_fd legacy = (fn_plane_fd)real_sym("gbm_bo_get_fd_for_plane");
        return legacy ? legacy(bo, plane) : -1;
    }
    static fn_gbm_bo_get_fd gfd;
    static fn_gbm_bo_get_device gd;
    static fn_gbm_device_get_fd gdfd;
    static fn_gbm_bo_get_handle_for_plane gh;
    static fn_drm_prime gp;
    if (!gfd) {
        gfd  = (fn_gbm_bo_get_fd)real_sym("gbm_bo_get_fd");
        gd   = (fn_gbm_bo_get_device)real_sym("gbm_bo_get_device");
        gdfd = (fn_gbm_device_get_fd)real_sym("gbm_device_get_fd");
        gh   = (fn_gbm_bo_get_handle_for_plane)real_sym("gbm_bo_get_handle_for_plane");
        gp   = (fn_drm_prime)real_sym("drmPrimeHandleToFD");
    }
    if (plane == 0 && gfd) {
        int fd = gfd(bo);
        if (debug_enabled())
            fprintf(stderr, "[gbm-hw] plane0 fd -> %d (%s)\n", fd,
                    fd >= 0 ? "OK" : strerror(errno));
        if (fd >= 0) return fd;
    }
    if (gd && gdfd && gh && gp) {
        gbm_device *dev = gd(bo);
        int devfd = dev ? gdfd(dev) : -1;
        uint32_t h = gh(bo, plane).u32;
        int fd = -1;
        int r = gp(devfd, h, O_CLOEXEC | O_RDWR, &fd);
        if (debug_enabled())
            fprintf(stderr, "[gbm-hw] prime devfd=%d handle=0x%x r=%d fd=%d\n",
                    devfd, h, r, fd);
        if (r == 0 && fd >= 0) return fd;
    }
    return -1;
}

/* ---------- wrapper: gbm_bo_create_with_modifiers --------------------- */
gbm_bo *gbm_bo_create_with_modifiers(gbm_device *dev, uint32_t w, uint32_t h,
        uint32_t format, const uint64_t *modifiers, uint32_t count) {
    static fn_gbm_bo_create_with_modifiers real;
    if (!real) real = (fn_gbm_bo_create_with_modifiers)real_sym("gbm_bo_create_with_modifiers");
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_CREATE_FIX", 1))
        return real ? real(dev, w, h, format, modifiers, count) : NULL;

    uint64_t fixed[8];
    uint32_t n = count > 8 ? 8 : count;
    int changed = 0;
    for (uint32_t i = 0; i < n; i++) {
        fixed[i] = modifiers[i];
        if (fixed[i] == 0 || fixed[i] == 0x00FFFFFFFFFFFFFFULL) {
            fixed[i] = target_modifier();
            changed = 1;
        }
    }
    if (n == 0) { fixed[0] = target_modifier(); n = 1; changed = 1; }
    gbm_bo *bo = real ? real(dev, w, h, format, changed ? fixed : modifiers, n) : NULL;
    if (debug_enabled())
        fprintf(stderr, "[gbm-hw] create_with_mod %ux%u fmt=0x%x count=%u "
                "mod0=0x%llx changed=%d -> %p\n",
                w, h, format, count,
                (unsigned long long)(n ? (changed ? fixed[0] : modifiers[0]) : 0),
                changed, (void *)bo);
    cache_bo(bo, w, h, format);
    return bo;
}

/* ---------- wrapper: gbm_bo_create (THE real-window fix) -------------- */
gbm_bo *gbm_bo_create(gbm_device *dev, uint32_t width, uint32_t height,
                      uint32_t format, uint32_t flags) {
    static fn_gbm_bo_create real;
    static fn_gbm_bo_create_with_modifiers real_mod;
    if (!real) real = (fn_gbm_bo_create)real_sym("gbm_bo_create");
    if (!real_mod) real_mod = (fn_gbm_bo_create_with_modifiers)real_sym("gbm_bo_create_with_modifiers");
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_CREATE_FIX", 1))
        return real ? real(dev, width, height, format, flags) : NULL;

    /* Real window pixmaps are >=64x64 and RGB formats. HUAWEI rejects
     * Scanout|Rendering|Texturing (flags=37) outright; route through the
     * modifiers entry point with KWin's only accepted modifier. */
    uint64_t m = target_modifier();
    if (real_mod && width >= 64 && height >= 64) {
        gbm_bo *bo = real_mod(dev, width, height, format, &m, 1);
        if (debug_enabled())
            fprintf(stderr, "[gbm-hw] create %ux%u fmt=0x%x flags=0x%x "
                    "-> with_mod 0x%llx -> %p\n",
                    width, height, format, flags, (unsigned long long)m, (void *)bo);
        if (bo) { cache_bo(bo, width, height, format); return bo; }
    }
    gbm_bo *bo = real ? real(dev, width, height, format, flags) : NULL;
    if (debug_enabled())
        fprintf(stderr, "[gbm-hw] create %ux%u fmt=0x%x flags=0x%x -> %p\n",
                width, height, format, flags, (void *)bo);
    cache_bo(bo, width, height, format);
    return bo;
}

/* ---------- wrapper: gbm_bo_get_modifier ------------------------------ */
uint64_t gbm_bo_get_modifier(gbm_bo *bo) {
    static fn_gbm_bo_get_modifier real;
    if (!real) real = (fn_gbm_bo_get_modifier)real_sym("gbm_bo_get_modifier");
    uint64_t m = real ? real(bo) : 0;
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_REPORT_MODIFIER", 1))
        return m;
    if (m == 0 || m == 0x00FFFFFFFFFFFFFFULL) {
        uint64_t want = target_modifier();
        if (debug_enabled())
            fprintf(stderr, "[gbm-hw] get_modifier 0x%llx -> 0x%llx\n",
                    (unsigned long long)m, (unsigned long long)want);
        return want;
    }
    return m;
}

/* ---------- wrapper: stride repair (THE jitter root cause) ------------
 * HUAWEI libGFX returns a BROKEN stride for bos created with the HISI
 * modifier (the only modifier KWin accepts):
 *   LINEAR bo: gbm_bo_get_stride()=1024, stride_for_plane(0)=1024
 *   HISI   bo: gbm_bo_get_stride()=0,    stride_for_plane(0)=32
 * for a 256x144 AR24 buffer, where 1024 (=256*4) is correct.
 * Chrome uses these values to lay out pixels and to build the dmabuf params
 * it hands to KWin. A wrong stride means every row is sampled at the wrong
 * offset -> static frames look fine but moving video/WebGL visibly jitters.
 * Repair any implausible stride (< width) to width*4 (Chrome window buffers
 * are 32bpp: AR24/XR24/AB24). HISI_GBM_STRIDE_FIX=0 disables for A/B.
 */
static uint32_t repair_stride(gbm_bo *bo, uint32_t got, const char *which) {
    if (!env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_STRIDE_FIX", 1))
        return got;
    if (!bo || got >= 4) return got;           /* plausible already */
    static fn_gbm_bo_get_width gw;
    if (!gw) gw = (fn_gbm_bo_get_width)real_sym("gbm_bo_get_width");
    uint32_t w = gw ? gw(bo) : 0;
    if (!w) return got;
    uint32_t fixed = w * 4;
    if (debug_enabled())
        fprintf(stderr, "[gbm-hw] %s %u -> %u (w=%u)\n", which, got, fixed, w);
    return fixed;
}

uint32_t gbm_bo_get_stride(gbm_bo *bo) {
    static fn_gbm_bo_get_stride real;
    if (!real) real = (fn_gbm_bo_get_stride)real_sym("gbm_bo_get_stride");
    uint32_t s = real ? real(bo) : 0;
    return repair_stride(bo, s, "stride");
}

uint32_t gbm_bo_get_stride_for_plane(gbm_bo *bo, int plane) {
    static fn_gbm_bo_get_stride_for_plane real;
    if (!real) real = (fn_gbm_bo_get_stride_for_plane)real_sym("gbm_bo_get_stride_for_plane");
    uint32_t s = real ? real(bo, plane) : 0;
    return repair_stride(bo, s, "stride_plane");
}

/* ---------- wrapper: gbm_bo_import ------------------------------------ */
gbm_bo *gbm_bo_import(gbm_device *dev, uint32_t type, void *buffer,
                      uint32_t flags) {
    static fn_gbm_bo_import real;
    if (!real) real = (fn_gbm_bo_import)real_sym("gbm_bo_import");
    if (type != GBM_BO_IMPORT_FD_MODIFIER || !buffer ||
        !env_flag("HISI_GBM_SHIM", 1) || !env_flag("HISI_GBM_IMPORT_FIX", 1))
        return real ? real(dev, type, buffer, flags) : NULL;

    /* The incoming fd is at byte offset 16 of gbm_import_fd_modifier_data;
     * width/height/format at 0/4/8. Pass geometry so lookup_bo can reject a
     * stale inode that now belongs to a differently-sized scroll tile. */
    int in_fd = *(int *)((unsigned char *)buffer + 16);
    uint32_t in_w = *(uint32_t *)((unsigned char *)buffer + 0);
    uint32_t in_h = *(uint32_t *)((unsigned char *)buffer + 4);
    uint32_t in_fmt = *(uint32_t *)((unsigned char *)buffer + 8);
    gbm_bo *cached = lookup_bo(in_fd, in_w, in_h, in_fmt);
    if (cached) {
        if (debug_enabled())
            fprintf(stderr, "[gbm-hw] import fd=%d %ux%u fmt=0x%x -> cached bo=%p OK\n",
                    in_fd, in_w, in_h, in_fmt, (void *)cached);
        return cached;
    }

    unsigned char local[80];
    memset(local, 0, sizeof(local));
    memcpy(local, buffer, 64);
    uint64_t chrome_mod = *(uint64_t *)((unsigned char *)buffer + 56);
    if (chrome_mod == 0 || chrome_mod == 0x00FFFFFFFFFFFFFFULL)
        chrome_mod = target_modifier();
    *(uint64_t *)(local + 56) = chrome_mod;
    *(uint64_t *)(local + 64) = chrome_mod;
    gbm_bo *bo = real ? real(dev, type, local, flags) : NULL;
    if (debug_enabled())
        fprintf(stderr, "[gbm-hw] import fd=%d mod=0x%llx -> %p%s\n",
                in_fd, (unsigned long long)chrome_mod, (void *)bo,
                bo ? " OK" : " FAILED");
    /* Last resort: a fresh bo of the same geometry (import is broken here). */
    if (!bo) {
        uint32_t w = *(uint32_t *)((unsigned char *)buffer + 0);
        uint32_t h = *(uint32_t *)((unsigned char *)buffer + 4);
        uint32_t fmt = *(uint32_t *)((unsigned char *)buffer + 8);
        static fn_gbm_bo_create_with_modifiers cwm;
        if (!cwm) cwm = (fn_gbm_bo_create_with_modifiers)real_sym("gbm_bo_create_with_modifiers");
        if (cwm && w && h) {
            uint64_t m = chrome_mod;
            bo = cwm(dev, w, h, fmt, &m, 1);
            if (debug_enabled())
                fprintf(stderr, "[gbm-hw] import fallback fresh %ux%u fmt=0x%x -> %p\n",
                        w, h, fmt, (void *)bo);
        }
    }
    return bo;
}

/* ---------- dlsym hook: hand Chrome our GBM wrappers ------------------ */
void *dlsym(void *handle, const char *symbol) {
    void *(*rd)(void *, const char *) = real_dlsym_fn();
    if (!rd) return NULL;
    if (!env_flag("HISI_GBM_SHIM", 1)) return rd(handle, symbol);
    if (handle && symbol) {
        if (strcmp(symbol, "vaInitialize") == 0 && handle != RTLD_NEXT &&
            env_flag("HISI_VAAPI_SPOOF", 1)) {
            void *real = rd(handle, symbol);
            if (real)
                captured_real_vaInitialize =
                    (VAStatus (*)(VADisplay, int *, int *))real;
            if (debug_enabled())
                fprintf(stderr, "[gbm-hw] dlsym(vaInitialize) wrapped\n");
            return (void *)&vaInitialize;
        }
        if (strncmp(symbol, "gbm_", 4) == 0 && debug_enabled())
            fprintf(stderr, "[gbm-hw] dlsym(%s)\n", symbol);
        if (strcmp(symbol, "gbm_bo_get_fd_for_plane") == 0)
            return (void *)&gbm_bo_get_fd_for_plane;
        if (strcmp(symbol, "gbm_bo_create_with_modifiers") == 0)
            return (void *)&gbm_bo_create_with_modifiers;
        if (strcmp(symbol, "gbm_bo_create") == 0)
            return (void *)&gbm_bo_create;
        if (strcmp(symbol, "gbm_bo_get_modifier") == 0)
            return (void *)&gbm_bo_get_modifier;
        if (strcmp(symbol, "gbm_bo_get_stride") == 0)
            return (void *)&gbm_bo_get_stride;
        if (strcmp(symbol, "gbm_bo_get_stride_for_plane") == 0)
            return (void *)&gbm_bo_get_stride_for_plane;
        if (strcmp(symbol, "gbm_bo_import") == 0)
            return (void *)&gbm_bo_import;
    }
    return rd(handle, symbol);
}

/* ---- VAAPI vaInitialize wrapper (report 1.17 to pass Chrome's gate) ---- */
VAStatus vaInitialize(VADisplay dpy, int *major, int *minor)
{
    static VAStatus (*real)(VADisplay, int *, int *);
    if (!real) real = captured_real_vaInitialize;
    if (!real) real = (VAStatus (*)(VADisplay, int *, int *))
            real_dlsym_fn()(RTLD_NEXT, "vaInitialize");
    VAStatus st;
    if (real) st = real(dpy, major, minor);
    else { if (major) *major = 1; if (minor) *minor = 17; return VA_STATUS_SUCCESS; }
    if (st == VA_STATUS_SUCCESS && major && minor && *major == 1 && *minor < 17) {
        if (debug_enabled())
            fprintf(stderr, "[gbm-hw] vaInitialize 1.%d -> 1.17\n", *minor);
        *minor = 17;
    }
    return st;
}
````


---

## 附录 A.2  cursor-wire-shim.c（原生 Wayland 光标大小修复）

源码路径（仅历史记录）：`/opt/eglrun/src/cursor-wire-shim.c`

````c
/*
 * cursor-wire-shim.so -- fix Chrome 157 Wayland cursor size at the wire level
 *
 * Chrome's bundled libGLESv2.so statically embeds its own libwayland-client
 * and calls its own marshal functions directly, so LD_PRELOAD hooks on
 * wl_proxy_marshal_* cannot reach the cursor requests. Both copies, however,
 * send bytes to the compositor through write(2) on the Wayland socket.
 *
 * This shim parses outgoing Wayland messages, learns object ids for
 * wl_compositor.create_surface and wl_seat.get_pointer, records the surface
 * used by wl_pointer.set_cursor, and rewrites that surface's
 * wl_surface.set_buffer_scale from DSF (2) to 1 before the bytes go out.
 * Chrome's 24*DSF px cursor buffer is then displayed at native size instead
 * of being halved into a 24dp logical cursor. Hotspot stays correct because
 * it is tip-relative. No Chrome binary patching.
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/socket.h>

#define MAX_IDS 256

static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_writev)(int, const struct iovec *, int);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*real_send)(int, const void *, size_t, int);
static int ids_ready;
static uint32_t registries[MAX_IDS]; static int n_reg;
static uint32_t seats[MAX_IDS]; static int n_seat;
static uint32_t compositors[MAX_IDS]; static int n_comp;
static uint32_t surfaces[MAX_IDS]; static int n_surf;
static uint32_t pointers[MAX_IDS]; static int n_ptr;
static uint32_t shms[MAX_IDS]; static int n_shm;
static uint32_t pools[MAX_IDS]; static int n_pool;
static uint32_t buffers[MAX_IDS]; static int n_buf;
static uint32_t buf_wh[MAX_IDS];
static uint32_t cursors[MAX_IDS]; static int n_cur;

static void logline(const char *fmt, ...)
{
    static FILE *fp;
    va_list ap;
    if (!fp) {
        fp = fopen("/tmp/cursor-wire-shim.log", "a");
        if (fp) setvbuf(fp, NULL, _IOLBF, 0);
    }
    if (!fp) return;
    va_start(ap, fmt);
    vfprintf(fp, fmt, ap);
    va_end(ap);
}

static void add_id(uint32_t *arr, int *n, uint32_t id)
{
    if (*n >= MAX_IDS) return;
    for (int i = 0; i < *n; i++) if (arr[i] == id) return;
    arr[(*n)++] = id;
}

static int has_id(uint32_t *arr, int n, uint32_t id)
{
    for (int i = 0; i < n; i++) if (arr[i] == id) return 1;
    return 0;
}

static void init_real(void)
{
    if (ids_ready) return;
    ids_ready = 1;
    real_write = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    real_writev = (ssize_t (*)(int, const struct iovec *, int))dlsym(RTLD_NEXT, "writev");
    real_sendmsg = (ssize_t (*)(int, const struct msghdr *, int))dlsym(RTLD_NEXT, "sendmsg");
    real_send = (ssize_t (*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
    add_id(registries, &n_reg, 2); /* default wl_registry id in most clients */
}

/* Pass 1: learn object ids. Pass 2: rewrite cursor set_buffer_scale. */
static int scan(const unsigned char *p, size_t len, int patch)
{
    size_t off = 0;
    int changed = 0;
    while (off + 8 <= len) {
        uint32_t id;
        uint16_t op, sz;
        memcpy(&id, p + off, 4);
        memcpy(&op, p + off + 4, 2);
        memcpy(&sz, p + off + 6, 2);
        if (sz < 8 || (size_t)(off + sz) > len) break;
        const unsigned char *a = p + off + 8;

        /* wl_display.get_registry(op0): uint new_id */
        if (id == 1 && op == 0 && sz >= 12) {
            uint32_t nid; memcpy(&nid, a, 4);
            add_id(registries, &n_reg, nid);
        }
        /* wl_registry.bind(op0): uint name, string iface, uint ver, uint new_id */
        if (has_id(registries, n_reg, id) && op == 0) {
            size_t q = 4;
            if (q + 4 <= (size_t)sz - 8) {
                uint32_t slen; memcpy(&slen, a + q, 4);
                q += 4;
                if (slen > 0 && slen < 128 && q + slen <= (size_t)sz - 8) {
                    char name[128];
                    memcpy(name, a + q, slen);
                    name[slen - 1] = 0;
                    q += (slen + 3) & ~3u;
                    if (q + 8 <= (size_t)sz - 8) {
                        uint32_t nid; memcpy(&nid, a + q + 4, 4);
                        if (strcmp(name, "wl_compositor") == 0) add_id(compositors, &n_comp, nid);
                        else if (strcmp(name, "wl_seat") == 0) add_id(seats, &n_seat, nid);
                        else if (strcmp(name, "wl_shm") == 0) add_id(shms, &n_shm, nid);
                    }
                }
            }
        }
        /* wl_shm.create_pool(op0): uint new_id, fd, int size */
        if (has_id(shms, n_shm, id) && op == 0 && sz >= 20) {
            uint32_t nid; memcpy(&nid, a, 4);
            add_id(pools, &n_pool, nid);
        }
        /* wl_shm_pool.create_buffer(op0): uint new_id, int offset, int width, int height, int stride, uint format */
        if (has_id(pools, n_pool, id) && op == 0 && sz >= 28) {
            uint32_t nid; memcpy(&nid, a, 4);
            int32_t w, h; memcpy(&w, a + 8, 4); memcpy(&h, a + 12, 4);
            if (n_buf < MAX_IDS) {
                buffers[n_buf] = nid;
                buf_wh[n_buf] = ((uint32_t)(uint16_t)w << 16) | (uint16_t)h;
                n_buf++;
            }
        }
        /* wl_compositor.create_surface(op0): uint new_id */
        if (has_id(compositors, n_comp, id) && op == 0 && sz >= 12) {
            uint32_t nid; memcpy(&nid, a, 4);
            add_id(surfaces, &n_surf, nid);
        }
        /* wl_seat.get_pointer(op0): uint new_id */
        if (has_id(seats, n_seat, id) && op == 0 && sz >= 12) {
            uint32_t nid; memcpy(&nid, a, 4);
            add_id(pointers, &n_ptr, nid);
        }
        /* wl_surface.attach(op1): object buffer, int x, int y */
        if (has_id(surfaces, n_surf, id) && op == 1 && sz >= 20) {
            uint32_t bid; memcpy(&bid, a, 4);
            for (int i = 0; i < n_buf; i++) {
                if (buffers[i] == bid) {
                    int w = (int)(buf_wh[i] >> 16), h = (int)(buf_wh[i] & 0xffff);
                    if (w > 0 && h > 0 && w <= 96 && h <= 96) {
                        add_id(cursors, &n_cur, id);
                        logline("[pid %d] cursor surface#%u from %dx%d buffer\n", getpid(), id, w, h);
                    }
                    break;
                }
            }
        }
        /* wl_pointer.set_cursor(op0): uint serial, object surface, int hx, int hy */
        if (has_id(pointers, n_ptr, id) && op == 0 && sz >= 24) {
            uint32_t surf; memcpy(&surf, a + 4, 4);
            add_id(cursors, &n_cur, surf);
        }
        /* wl_surface.set_buffer_scale(op8): int scale */
        if (patch && has_id(cursors, n_cur, id) && op == 8 && sz >= 12) {
            int32_t scale;
            memcpy(&scale, a, 4);
            if (scale != 1) {
                int32_t one = 1;
                memcpy((void *)a, &one, 4);
                changed++;
                logline("[pid %d] patched cursor surface#%u set_buffer_scale %d -> 1\n",
                        getpid(), id, scale);
            }
        }
        off += sz;
    }
    return changed;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    init_real();
    if (real_write && count >= 8 && count <= 1 << 20 && buf) {
        const unsigned char *p = buf;
        scan(p, count, 0);
        if (n_cur > 0) {
            unsigned char *copy = malloc(count);
            if (copy) {
                memcpy(copy, p, count);
                if (scan(copy, count, 1) > 0) {
                    ssize_t r = syscall(SYS_write, fd, copy, count);
                    free(copy);
                    return r;
                }
                free(copy);
            }
        }
    }
    return real_write ? real_write(fd, buf, count) :
        (ssize_t)syscall(SYS_write, fd, buf, count);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    init_real();
    if (real_writev && iovcnt > 0) {
        size_t total = 0;
        for (int i = 0; i < iovcnt; i++) total += iov[i].iov_len;
        if (total >= 8 && total <= 1 << 20) {
            unsigned char *flat = malloc(total);
            if (flat) {
                size_t o = 0;
                for (int i = 0; i < iovcnt; i++) {
                    memcpy(flat + o, iov[i].iov_base, iov[i].iov_len);
                    o += iov[i].iov_len;
                }
                scan(flat, total, 0);
                if (n_cur > 0 && scan(flat, total, 1) > 0) {
                    ssize_t r = syscall(SYS_write, fd, flat, total);
                    free(flat);
                    return r;
                }
                free(flat);
            }
        }
    }
    return real_writev ? real_writev(fd, iov, iovcnt) :
        (ssize_t)syscall(SYS_writev, fd, iov, iovcnt);
}


/* Wayland socket flush uses sendmsg(MSG_NOSIGNAL) on Linux. */
ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    init_real();
    if (real_sendmsg && msg && msg->msg_iovlen > 0) {
        size_t total = 0;
        for (int i = 0; i < msg->msg_iovlen; i++) total += msg->msg_iov[i].iov_len;
        if (total >= 8 && total <= 1 << 20) {
            unsigned char *flat = malloc(total);
            if (flat) {
                size_t o = 0;
                for (int i = 0; i < msg->msg_iovlen; i++) {
                    memcpy(flat + o, msg->msg_iov[i].iov_base, msg->msg_iov[i].iov_len);
                    o += msg->msg_iov[i].iov_len;
                }
                scan(flat, total, 0);
                if (n_cur > 0 && scan(flat, total, 1) > 0) {
                    ssize_t r = syscall(SYS_sendmsg, fd, &(struct msghdr){
                        .msg_iov = &(struct iovec){ .iov_base = flat, .iov_len = total },
                        .msg_iovlen = 1
                    }, flags);
                    free(flat);
                    return r;
                }
                free(flat);
            }
        }
    }
    return real_sendmsg ? real_sendmsg(fd, msg, flags) :
        (ssize_t)syscall(SYS_sendmsg, fd, msg, flags);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    init_real();
    if (real_send && buf && len >= 8 && len <= 1 << 20) {
        const unsigned char *p = buf;
        scan(p, len, 0);
        if (n_cur > 0) {
            unsigned char *copy = malloc(len);
            if (copy) {
                memcpy(copy, p, len);
                if (scan(copy, len, 1) > 0) {
                    ssize_t r = syscall(SYS_sendto, fd, copy, len, flags, NULL, 0);
                    free(copy);
                    return r;
                }
                free(copy);
            }
        }
    }
    return real_send ? real_send(fd, buf, len, flags) :
        (ssize_t)syscall(SYS_sendto, fd, buf, len, flags, NULL, 0);
}
````


---

## 附录 A.3  wire-damage-shim.c（逐帧提交延迟 = 「果冻垫片」；当前 1.0.11 为固定 4ms 延迟，无自适应参数）

源码路径（仅历史记录）：`/opt/eglrun/src/wire-damage-shim.c`

````c
/*
 * wire-commit-shim.so -- wire-level (syscall) interception of Chrome's
 * embedded libwayland, to mitigate stale-buffer presentation on
 * Kirin/Maleoon + DDE KWin (no wp_linux_drm_syncobj -> implicit sync -> KWin
 * samples a stale dma-buf; a tiny commit delay lets the EGL fence settle).
 *
 * WHY WIRE LEVEL: Chrome 157 statically embeds libwayland-client in
 * libGLESv2.so; LD_PRELOAD hooks on wl_proxy_marshal_* see ZERO calls
 * (damage-full-shim v1..v5). But both libwayland copies flush bytes via
 * write()/sendmsg(). cursor-wire-shim.so proves this path works.
 *
 * CRITICAL SAFETY RULE (learned the hard way): NEVER change the byte LENGTH
 * of a Wayland flush. Chrome's embedded libwayland tracks how many bytes it
 * wrote; injecting a 24-byte damage desynchronises its event demarshaller and
 * KWin replies produce "Too many bits for size_t". So this version only:
 *   (a) DELAYS a commit-bearing flush by N microseconds (no byte change),
 *       and/or
 *   (b) REWRITES IN PLACE the x/y/w/h of existing wl_surface.damage messages
 *       to a full-screen rectangle (same length) [WIRE_DAMAGE_FULL=1].
 * Original message order, count and length are always preserved.
 *
 * HOW THE WAYLAND FD IS CONFIRMED: per-fd state; a fd is "wayland" once a
 * get_registry or a bind("wl_compositor"/"wl_shm"/"wl_seat") is seen on it.
 * No /proc/net/unix (Chrome uses an abstract/inherited socket).
 *
 * Env: WIRE_DAMAGE_FULL=1|0 (in-place damage widening, default 1),
 *      WIRE_COMMIT_DELAY_US=N (default 0), WIRE_DEBUG=1, WIRE_FORCE_FD=1(test).
 * Build: gcc -shared -fPIC -O2 -o wire-commit-shim.so wire-commit-shim.c -ldl
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdarg.h>
#include <sys/uio.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sys/syscall.h>
#include <sys/socket.h>

#define MAX_FDS 4096

static ssize_t (*real_write)(int, const void *, size_t);
static ssize_t (*real_writev)(int, const struct iovec *, int);
static ssize_t (*real_sendmsg)(int, const struct msghdr *, int);
static ssize_t (*real_send)(int, const void *, size_t, int);
static int real_ready;

/* per-fd: only need a "confirmed wayland" flag; no object tracking required
 * because we never inject, only rewrite damage in place and delay. */
static unsigned char fd_wl[MAX_FDS];

static unsigned long n_damage_rewritten, n_delay_us, n_flush_seen, n_commit_seen;

static void logline(const char *fmt, ...)
{
    static FILE *fp; va_list ap;
    if (!fp) { fp = fopen("/tmp/wire-commit-shim.log", "a"); if (fp) setvbuf(fp, NULL, _IOLBF, 0); }
    if (!fp) return;
    va_start(ap, fmt); vfprintf(fp, fmt, ap); va_end(ap);
}

static void init_real(void)
{
    if (real_ready) return;
    real_ready = 1;
    real_write   = (ssize_t (*)(int, const void *, size_t))dlsym(RTLD_NEXT, "write");
    real_writev  = (ssize_t (*)(int, const struct iovec *, int))dlsym(RTLD_NEXT, "writev");
    real_sendmsg = (ssize_t (*)(int, const struct msghdr *, int))dlsym(RTLD_NEXT, "sendmsg");
    real_send    = (ssize_t (*)(int, const void *, size_t, int))dlsym(RTLD_NEXT, "send");
}

static int opt_damage(void) {
    static int v = -1;
    /* DEFAULT OFF: full-screen damage widening was proven HARMFUL (forces
     * KWin full re-upload every frame -> widens the GPU/scanout race window;
     * user saw stronger jelly, worst at top-left = scanout order). Only enable
     * explicitly with WIRE_DAMAGE_FULL=1. */
    if (v < 0) { const char *e = getenv("WIRE_DAMAGE_FULL"); v = (e && strcmp(e,"1")==0) ? 1 : 0; }
    return v;
}
static long opt_delay_us(void) {
    static int init = 0; static long v = 0;
    if (!init) { init = 1; const char *e = getenv("WIRE_COMMIT_DELAY_US"); v = e ? atol(e) : 0; if (v<0) v=0; }
    return v;
}
static int opt_debug(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("WIRE_DEBUG"); v = (e && strcmp(e,"1")==0) ? 1 : 0; }
    return v;
}

static void delay_us(long us)
{
    if (us <= 0) return;
    struct timespec ts;
    ts.tv_sec = us / 1000000L; ts.tv_nsec = (us % 1000000L) * 1000L;
    while (nanosleep(&ts, &ts) == -1 && errno == EINTR) { }
    n_delay_us += (unsigned long)us;
}

/* Strict: the ENTIRE buffer must be a well-formed wayland message stream. */
static int valid_stream(const unsigned char *p, size_t len)
{
    size_t off = 0; int nmsg = 0;
    while (off + 8 <= len) {
        uint16_t sz; memcpy(&sz, p + off + 6, 2);
        if (sz < 8 || (sz & 3) != 0 || (size_t)(off + sz) > len) return 0;
        off += sz; nmsg++;
        if (nmsg > 4096) return 0;
    }
    return off == len && nmsg > 0;
}

/*
 * Process a copied buffer in place (caller passes a mutable copy).
 *  - confirm "wayland" fd from handshake messages seen in the stream
 *  - if confirmed: widen any wl_surface.damage(op2, size24) rect in place
 *  - report whether a plausible commit(op6, size8) occurred (for delay)
 * NEVER changes byte count. Returns number of damage rects rewritten.
 */
static int process(unsigned char *p, size_t len, int fd, int *saw_commit)
{
    if (!valid_stream(p, len)) return 0;
    size_t off = 0; int rewrote = 0; int confirmed = fd_wl[fd];
    int32_t FW = 32767;   /* full-surface-ish positive rect, KWin clips it */

    while (off + 8 <= len) {
        uint32_t id; uint16_t op, sz;
        memcpy(&id, p + off, 4);
        memcpy(&op, p + off + 4, 2);
        memcpy(&sz, p + off + 6, 2);
        const unsigned char *a = p + off + 8;

        /* wl_display.get_registry(id=1,op0) -> confirm */
        if (id == 1 && op == 0) confirmed = 1;
        /* wl_registry.bind(op0, string iface, ...) -> confirm wayland globals */
        if (op == 0) {
            size_t q = 4;
            if (q + 4 <= (size_t)sz - 8) {
                uint32_t slen; memcpy(&slen, a + q, 4); q += 4;
                if (slen > 2 && slen < 64 && q + slen <= (size_t)sz - 8) {
                    char nm[64]; memcpy(nm, a + q, slen); nm[slen-1] = 0;
                    if (!strcmp(nm,"wl_compositor") || !strcmp(nm,"wl_shm") ||
                        !strcmp(nm,"wl_seat") || !strcmp(nm,"wl_registry"))
                        confirmed = 1;
                }
            }
        }
        /* wl_surface.damage(op2): 4x int32 => size exactly 24.
         * On a confirmed wayland fd this is the MOST RELIABLE per-frame signal
         * in Chrome 157 (the op6 commit is frequently flushed separately or
         * not emitted with sz==8). Use it to drive the commit delay. */
        if (confirmed && op == 2 && sz == 24) {
            *saw_commit = 1;   /* frame signal, regardless of damage widening */
            if (opt_damage()) {
                int32_t x,y; memcpy(&x,a,4); memcpy(&y,a+4,4);
                if (x >= 0 && y >= 0) {
                    int32_t zero = 0;
                    memcpy((void*)a,     &zero, 4);
                    memcpy((void*)a + 4, &zero, 4);
                    memcpy((void*)a + 8, &FW, 4);
                    memcpy((void*)a +12, &FW, 4);
                    rewrote++;
                }
            }
        }
        /* plausible commit: op6, size 8 (secondary frame signal) */
        if (op == 6 && confirmed && sz == 8) *saw_commit = 1;
        off += sz;
    }
    if (confirmed && fd < MAX_FDS) fd_wl[fd] = 1;
    if (rewrote) {
        n_damage_rewritten += (unsigned long)rewrote;
        if (opt_debug())
            logline("[pid %d] widened %d damage rect(s) len=%zu fd=%d\n",
                    getpid(), rewrote, len, fd);
    }
    return rewrote;
}

/* For write()/send(): make a mutable copy, process in place, send via real
 * function with ORIGINAL length. Returns real's result. */
static ssize_t emit(int fd, const void *buf, size_t len,
                    ssize_t (*real)(int, const void *, size_t))
{
    int force = getenv("WIRE_FORCE_FD") != NULL;
    if (!force && !fd_wl[fd] && !valid_stream((const unsigned char*)buf, len))
        return real(fd, buf, len);
    unsigned char *copy = (unsigned char *)malloc(len);
    if (!copy) return real(fd, buf, len);
    memcpy(copy, buf, len);
    int saw_commit = 0;
    process(copy, len, fd, &saw_commit);
    if (saw_commit && fd_wl[fd]) {
        long d = opt_delay_us();
        delay_us(d); n_commit_seen++;
        if (opt_debug()) logline("[pid %d] DELAY %ldus fd=%d len=%zu (frame signal)\n",
                                 getpid(), d, fd, len);
    }
    ssize_t r = real(fd, copy, len);   /* SAME length; real preserves ancillary for send */
    free(copy);
    if (r >= 0) n_flush_seen++;
    return r;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    init_real();
    if (!real_write || !buf || count < 8 || count > (1u<<20))
        return real_write ? real_write(fd, buf, count) : (ssize_t)syscall(SYS_write, fd, buf, count);
    return emit(fd, buf, count, real_write);
}

ssize_t send(int fd, const void *buf, size_t len, int flags)
{
    init_real();
    if (flags != 0 || !real_write || !buf || len < 8 || len > (1u<<20))
        return real_send ? real_send(fd, buf, len, flags) : (ssize_t)syscall(SYS_sendto, fd, buf, len, flags, NULL, 0);
    /* flags==0: write() and send() are equivalent on a socket; use real_write
     * through emit so the helper keeps a uniform 3-arg signature. */
    return emit(fd, buf, len, real_write);
}

ssize_t writev(int fd, const struct iovec *iov, int iovcnt)
{
    init_real();
    if (!real_writev || iovcnt <= 0) return real_writev ? real_writev(fd, iov, iovcnt) : (ssize_t)syscall(SYS_writev, fd, iov, iovcnt);
    size_t total = 0; for (int i=0;i<iovcnt;i++) total += iov[i].iov_len;
    if (total < 8 || total > (1u<<20)) return real_writev(fd, iov, iovcnt);
    unsigned char *flat = (unsigned char*)malloc(total);
    if (!flat) return real_writev(fd, iov, iovcnt);
    size_t o=0; for (int i=0;i<iovcnt;i++){ memcpy(flat+o,iov[i].iov_base,iov[i].iov_len); o+=iov[i].iov_len; }
    int saw_commit=0;
    if (fd_wl[fd] || valid_stream(flat,total)) process(flat,total,fd,&saw_commit);
    if (saw_commit && fd_wl[fd]) {
        long d = opt_delay_us();
        delay_us(d); n_commit_seen++;
        if (opt_debug()) logline("[pid %d] DELAY %ldus fd=%d len=%zu (writev)\n",
                                 getpid(), d, fd, total);
    }
    ssize_t r = real_writev(fd, iov, iovcnt);   /* original iov, but we modified payload in place */
    free(flat);
    return r;
}

ssize_t sendmsg(int fd, const struct msghdr *msg, int flags)
{
    init_real();
    if (!real_sendmsg || !msg || msg->msg_iovlen <= 0 || (flags & MSG_OOB))
        return real_sendmsg ? real_sendmsg(fd, msg, flags) : (ssize_t)syscall(SYS_sendmsg, fd, msg, flags);
    size_t total = 0; for (size_t i=0;i<msg->msg_iovlen;i++) total += msg->msg_iov[i].iov_len;
    if (total < 8 || total > (1u<<20)) return real_sendmsg(fd, msg, flags);
    /* modify the caller's iov payload in place (length unchanged) */
    int saw_commit=0;
    int force = getenv("WIRE_FORCE_FD")!=NULL;
    if (force || fd_wl[fd] || valid_stream((const unsigned char*)msg->msg_iov[0].iov_base,total)) {
        /* flatten only if multiple iov; else process directly */
        if (msg->msg_iovlen == 1)
            process((unsigned char*)msg->msg_iov[0].iov_base, msg->msg_iov[0].iov_len, fd, &saw_commit);
        else {
            unsigned char *flat=(unsigned char*)malloc(total);
            if (flat) {
                size_t o=0; for(size_t i=0;i<msg->msg_iovlen;i++){memcpy(flat+o,msg->msg_iov[i].iov_base,msg->msg_iov[i].iov_len);o+=msg->msg_iov[i].iov_len;}
                process(flat,total,fd,&saw_commit);
                o=0; for(size_t i=0;i<msg->msg_iovlen;i++){memcpy(msg->msg_iov[i].iov_base,flat+o,msg->msg_iov[i].iov_len);o+=msg->msg_iov[i].iov_len;}
                free(flat);
            }
        }
    }
    if (saw_commit && fd_wl[fd]) {
        long d = opt_delay_us();
        delay_us(d); n_commit_seen++;
        if (opt_debug()) logline("[pid %d] DELAY %ldus fd=%d len=%zu (sendmsg)\n",
                                 getpid(), d, fd, total);
    }
    ssize_t r = real_sendmsg(fd, msg, flags);   /* original msghdr: ancillary preserved */
    if (r >= 0) n_flush_seen++;
    return r;
}

__attribute__((constructor)) static void wire_loaded(void)
{
    logline("=== wire-commit-shim v7 pid=%d damage=%d delay=%ld ===\n",
            getpid(), opt_damage(), opt_delay_us());
}
__attribute__((destructor)) static void wire_done(void)
{
    logline("--- pid=%d flush=%lu commits=%lu rewrote=%lu delay_us=%lu ---\n",
            getpid(), n_flush_seen, n_commit_seen, n_damage_rewritten, n_delay_us);
}
````


---

## 附录 A.4  build.sh（源码编译脚本；注意系统里这版漏编 wire-damage）

源码路径（仅历史记录）：`/opt/eglrun/src/build.sh`

````sh
#!/bin/sh
# Rebuild all native helpers from source into ../lib and ../bin.
# Run on the same UOS 20 / aarch64 environment as the target machine.
# Requires: gcc, wayland-scanner, libwayland-client-dev, libdbus-1-dev,
#           libxkbcommon-dev.
set -e
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"

echo "== gbm-hw-shim.so -> $ROOT/lib =="
gcc -shared -fPIC -O2 -ldl -o "$ROOT/lib/gbm-hw-shim.so" gbm-hw-shim.c

echo "== cursor-wire-shim.so -> $ROOT/lib =="
gcc -shared -fPIC -O2 -ldl -o "$ROOT/lib/cursor-wire-shim.so" cursor-wire-shim.c

echo "== wayland-im-shim -> $ROOT/bin =="
cd ime
gcc -O2 -o "$ROOT/bin/wayland-im-shim" \
  wayland-im-shim.c input-method-unstable-v1-client-protocol.c \
  -I/usr/include/dbus-1.0 -I/usr/lib/aarch64-linux-gnu/dbus-1.0/include \
  -lwayland-client -ldbus-1 -lxkbcommon

echo "done. Verify with: md5sum $ROOT/lib/*.so $ROOT/bin/wayland-im-shim"
````


---

## 附录 B   chrome-nojelly.sh（去果冻垫片 A/B 启动脚本，用默认 profile）

源码路径（仅历史记录）：`/home/UOS/Desktop/chrome-nojelly.sh`

````sh
#!/bin/bash
# chrome-nojelly.sh - Chrome with the SAME config as eglrun, but with ONLY the
# "jelly removal" shim (wire-damage-shim.so) removed. Keeps:
#   - gbm-hw-shim.so       (HUAWEI GBM ABI bridge + VAAPI spoof + stride fix; 1.0.11 has NO glFinish)
#   - cursor-wire-shim.so  (native Wayland cursor size fix)
# All flags, DPI, LIBVA_* and cursor size are inherited verbatim from the real
# eglrun env.sh, so this is byte-for-byte the system config minus one shim.
# Uses your DEFAULT profile (bookmarks/logins intact). To revert, just launch
# Chrome the normal way (google-chrome / desktop icon) — nothing was changed
# on disk in /opt.
#
# Usage:
#   ./chrome-nojelly.sh                       # open default homepage
#   ./chrome-nojelly.sh 'https://threejs.org/examples/#webgl_animation_keyframes'
set -u

# 1) Load the exact system environment (LIBVA_*, LD_PRELOAD with all 3 shims,
#    XCURSOR_SIZE, etc.) so nothing drifts from eglrun.
. /opt/eglrun/bin/env.sh

# 2) Remove ONLY wire-damage-shim.so from LD_PRELOAD, preserving order of the rest.
IFS=':' read -ra _shims <<< "${LD_PRELOAD:-}"
LD_PRELOAD=""
for _p in "${_shims[@]}"; do
  case "$_p" in
    *wire-damage-shim.so*) ;;                    # drop the jelly-timing shim
    *) LD_PRELOAD="${LD_PRELOAD:+$LD_PRELOAD:}$_p" ;;
  esac
done
unset _shims _p
export LD_PRELOAD

# 3) Warn if a Chrome using the default profile is already running — otherwise
#    the new process just forwards the URL to the existing instance and the
#    changed shim set never takes effect.
if pgrep -u "$(id -u)" -f '/opt/google-chrome/chrome' >/dev/null 2>&1; then
  echo "WARNING: Chrome is already running. Quit ALL Chrome windows first," >&2
  echo "         otherwise this launches into the existing instance and the shim change is ignored." >&2
  echo >&2
fi

# 4) Recreate eglrun's exact flag set for APP_TYPE=chrome.
FLAGS=(
  --use-angle=gl
  --enable-gpu-compositing
  --render-node-override=/dev/dri/renderD128
  --disable-features=AcceleratedVideoDecodeLinuxZeroCopyGL
  --enable-features=VaapiVideoDecoder\,VaapiIgnoreDriverChecks
  --enable-wayland-ime
  --wayland-text-input-version=1
  --force-device-scale-factor="${FORCE_SCALE:-2}"
  --class=google-chrome
  --no-first-run --no-default-browser-check
)

echo "=== chrome-nojelly ==="
echo "LD_PRELOAD=$LD_PRELOAD"
echo "flags: ${FLAGS[*]}"
echo

exec env -u EGL_PLATFORM \
  /opt/google-chrome/chrome "${FLAGS[@]}" "$@"
````

