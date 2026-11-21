# 研究与技术验证记录

> 对象：在 UOS20 / Kirin 9000c / Maleoon 910 上运行原厂 Firefox AppImage，
> 实现原生 Wayland、WebRender/WebGL 硬件渲染、H.264 VAAPI 硬解。
> 本文记录探索过程中的关键判断、几次翻案、决定性实验与最终结论，供后续维护者参考。

---

## 一、机器与系统栈的特殊性（一切问题的根源）

这台机器的图形栈出厂就被华为深度定制，几个关键系统库被替换：

```
/usr/lib/aarch64-linux-gnu/libEGL.so.1      ─┐
/usr/lib/aarch64-linux-gnu/libGLESv2.so.2   ─┼─→ 软链到 /usr/lib/aarch64-linux-gnu/libGFX_hisi.so.0.8.0
/usr/lib/aarch64-linux-gnu/libgbm.so.1      ─┘    （华为 GPU 库，无 GLVND 的 egl* 符号）
```

- 真 Mesa GLVND 只在 `/usr/lib/deepin-shared-libs/libegl1/libEGL.so.1.1.0`（导出全套 `egl*`），
  它本身是 dispatcher，`libEGL_mesa.so.0` 只是 vendor stub（只导出 `__egl_Main`）。
- H.264 硬解驱动 `vacore_drv_video.so` / `vadec` 走 VAAPI，但只在 `/usr/lib/`，
  且 Firefox 侧的硬解能力查询依赖 `libavcodec` 建立的 EGL device 函数表。
- 系统自带浏览器（Chrome 126，`/usr/bin/browser`）之所以"能硬解"，靠的是
  `zink → Vulkan → Maleoon` 绕开华为 GBM 路径 + `/usr/share/browser` 自带的 ANGLE 库。
  **注意**：它所谓的"硬件加速"主要指视频硬解，3D/WebGL 在早期实测中多为软渲染。

因此任何第三方浏览器想拿到同样的能力，都不能直接依赖系统库，必须自己做隔离与转发。

## 二、目标形态的确立

用户从一开始就明确了最终形态（这是贯穿全程的约束）：

> 一个启动器 + 一些垫片，能正常启动**原厂 Firefox AppImage**，原样运行、不解包、不改二进制字节。

这意味着：不 patch 系统文件、不 patch 解包目录、不 patch libxul / libGFX_hisi 的字节。
所有东西都必须在 `LD_PRELOAD` / `LD_LIBRARY_PATH` / 子进程环境层解决。

## 三、几次关键翻案（避免后人重走弯路）

### 翻案 1："默认浏览器 WebGL 是硬件加速" —— 错

早期以为系统浏览器什么都是硬件的。CDP 实测后修正：它强在**视频硬解（VAAPI）**，
WebGL 多为 SwiftShader 软渲染。工作重心因此从"追 WebGL"调整为"硬解 + 正确的 GPU 合成参数"。

### 翻案 2："Mesa EGL 提供 dma_buf_import 零拷贝" —— 假象

`libEGL_mesa.so.0` 探针一度报 `dma_buf_import=YES / Maleoon 910`，符号级复核发现：
`dlopen(libEGL_mesa.so.0)` 因缺 `gbm_bo_get_bpp` 失败，探针的 `dlsym` 回退到
`RTLD_DEFAULT`，实际测到的是**华为库**。真 Mesa dispatcher 的 `dma_buf_import=NO`。
→ 真正有零拷贝能力只有华为 EGL。凡是依赖"让 Mesa 提供该扩展"的方案全部作废。

### 翻案 3："libxul 3 字节补丁是 AppImage 必需" —— 错（最关键的一次）

这是全程最大的弯路：

- `about:support` 崩溃的确定性复现（3/3 exit=139）一度被归因为 libxul 中
  `ldr x8,[x21,#592]; blr x8` 的空指针调用，于是做了 libxul 字节热补丁 `xul_hotfix.so`。
- 但该热补丁**本身导致 AppImage 启动失败**（core 栈顶 `raise()`），且用户明确拒绝字节补丁。
- 用户一句"我记着你也 patch 了 gfxtest"点醒：回头做决定性 A/B：

  | 组合 | about:support 结果 |
  |------|--------------------|
  | 原厂 AppImage + 原厂 libxul（原版 gfxtest） | 3/3 崩（139） |
  | unpatched libxul + **有 gfxtest 包装脚本** | 3/3 存活（124，0 错误） |
  | unpatched libxul + **最简 gfxtest 脚本**（无 fbspoof/drmshim、未剥 gbm） | 3/3 崩（139） |

  **结论：about:support 崩溃的开关是 gfxtest 探测子进程的环境，与 libxul 字节补丁无关。**
  之前用 H.264 播放这种概率性场景做 A/B，把结论带偏了。
- 字节补丁路线彻底放弃，`xul_hotfix.so` 未接入 deb。

### 翻案 4："解包 AppImage 替换 gfxtest 才行" —— 不必

gfxtest 包装脚本的本质**不是改二进制**，而是给这个探测子进程换一套环境变量：

```
LD_LIBRARY_PATH 前置 egl-fbspoof + mesa
LD_PRELOAD      只留 ff-drm-device-shim.so（剥离继承来的 gbm-hw-shim.so —— 崩溃源）
GALLIUM_DRIVER=zink / MESA_LOADER_DRIVER_OVERRIDE=zink / LIBGL_KOPPER_DRI2=1
unset EGL_PLATFORM
```

AppImage 的 `/tmp/.mount_*` 是 fuse.dwarfs 只读挂载点，无法原地替换 gfxtest。
但 libxul 用 `execve` / `posix_spawn` 启动子进程 → 可以在 exec/spawn 边界注入环境。
于是写了 `gfxtest_env_shim.so`：hook `execve` + `posix_spawn`，仅当目标 basename 为
`gfxtest` 时用构造好的 `envp` 替换，其余进程原样透传。
→ **AppImage 原样运行、不解包**的目标达成。实测命中挂载点内原版 gfxtest 约 4 次/会话，
about:support 3/3 存活。

## 四、崩溃根因链（三类不同的崩溃，别混为一谈）

调试过程中出现过多处崩溃，根因并不相同：

1. **GPU 进程反复崩（早期 Chrome 路线）**：wayland ozone + Vulkan 不兼容，
   `DRM_IOCTL_HISI_GEM_CREATE error=-13`，被系统永久禁用 GPU。
2. **退出时段错误 / crash report**：华为库 `wl_event_queue_destroy` 回收循环触发 UAF，
   Firefox 主动 `MOZ_CRASH`。对策：`wllog_shim.so` 过滤队列警告 + `libGFX_hisi.so` 副本打掉析构 UAF。
3. **about:support 崩溃**：core 显示 `blr x8` 空指针，x21 指向的是
   **libavcodec 建立的 EGL device 函数表**，空槽由 vendor VAAPI 路径留下。
   这才是 gfxtest 环境注入要修的点——让探测子进程在干净的 Mesa/zink 环境里完成枚举。

其中 (3) 曾误导为 libxul 问题（见翻案 3），是本次研究最重要的教训：
**要区分"崩溃发生在哪个进程、哪张函数表上"**，不能只看栈顶指令就 patch 调用点。

## 五、UI 三个问题的定点修复

- **三按钮**：根因不是 CSD 开关，而是 GTK 的 `button-layout` 设置。
  Wayland 下 Firefox 的 `ToplevelUsesCSD()` 会被 `gdk_wayland_display_prefers_ssd` 否决。
  对策：`csd_shim.so` 在进程内拦截 GTK 设置（不影响其它 GTK 程序），
  配合 `MOZ_GTK_TITLEBAR_DECORATION=client` 与 `browser.tabs.inTitlebar=1`，
  拿到标签栏即标题栏 + Firefox 自绘三按钮。曾试过全局 `gsettings`，因影响面太大放弃。
- **滚轮**：不是完全失效，而是灵敏度和 Wayland 轴事件源问题。
  KWin 对 WHEEL 源发的 axis 事件与触摸板不同，Firefox 160 原生处理有问题。
  `wheel_shim.so` 重建 axis 事件并对滚轮源乘倍率 `0.3`；触摸板双指路径不动。
- **日志洪泛**：华为库的 Wayland 队列警告刷屏，`wllog_shim.so` 过滤。

## 六、最终架构

```
/usr/local/bin/firefox（启动器）
   │  profile 状态机：prefs.js 不存在=首次 → 只 mkdir 空目录（不写 user.js / 不传 URL，放行原厂语言引导）；
   │                  prefs.js 存在=已初始化 → 写 user.js / userChrome.css（不覆盖 locale）
   │  注：-profile 目标目录必须已存在，否则 Firefox 报 "profile cannot be loaded"
   │  设 GTK_THEME = 当前 gtk-3.0/settings.ini 的 gtk-theme-name（修 Wayland 下夜间模式失效）
   │  设 LD_LIBRARY_PATH = egl-huawei-v12 : mesa-egl-glvnd
   │  设 LD_PRELOAD = wllog + csd + wheel + gbm-hw + gfxtest_env
   │  设 HISI_FF_LIBGFX = 打掉 UAF 的 libGFX_hisi 副本
   ▼
原厂 Firefox AppImage（/opt/mozilla-firefox/bin/firefox-bin，零字节补丁）
   ├─ 主进程 / GPU 进程：egl-huawei-v12 分流 → 华为 EGL → Maleoon 910（WebGL/WR）
   ├─ gbm-hw-shim → VAAPI vacore/vadec（H.264 硬解）
   └─ gfxtest 探测子进程（fork/execve 时被 gfxtest_env_shim 截获）
         → LD_LIBRARY_PATH = egl-fbspoof : mesa-egl-glvnd
         → LD_PRELOAD 只剩 ff-drm-device-shim.so
         → zink 环境；about:support 在此干净环境完成枚举，不崩
```

关于 `lib/` 下各库是否必需、以及为什么不能删，见 `README.md` 第 3 节与各垫片源码头部注释。
一句话：每一个都对应链路上的一个环节，垫片总计约 4 MB（大头是 3.9 MB 的
`libGFX_hisi` 副本，恰是最不能删的），相对 91 MB 的 AppImage 可忽略。

## 七、验证方法（固定，勿改）

- **WebGL 固定测试页**（本项目统一标准）：
  `https://threejs.org/examples/#webgl_animation_keyframes`
- **崩溃确定性复现**：进 `about:support` 页面（枚举所有 GL/device 信息，必走崩溃路径）。
  退出码 139 = 崩，124（超时存活）= 过。这是比 H.264 播放更可靠的 A/B 手段。
- **最终用户实测**（`FF_MODE=appimage` 原厂 AppImage）：退出无段错误、vadec 有输出、
  WebGL 正常、about:support 不崩——四项全部通过，方案定稿。

## 八、经验教训

1. **先做符号级证据再下结论**：`dlopen`/`dlsym` 失败回退 `RTLD_DEFAULT` 会制造假象；
   用 `dladdr`、`nm`、`coredumpctl` 确认符号真实来源。
2. **用确定性场景做 A/B**：概率性场景（如 H.264 播放）会把因果带偏；
   `about:support` 是这里唯一可靠的开关。
3. **区分崩溃所在的进程与函数表**：同一个 `blr x8` 空指针，可能是 libxul 的，
   也可能是 libavcodec 建的 EGL device 表——patch 错对象必然失败。
4. **尊重"原厂 + 纯垫片"的约束**：字节补丁不仅违背可移植性，本身还会引入新故障
   （xul_hotfix 导致启动失败）。环境层能解决的，就不要动二进制。
5. **打包必须逐个文件核对软链**：deb 首版 `egl-huawei-v12/` 只拷了 `libEGL.so.1`，
   漏掉 `libEGL.so -> libEGL.so.1` 软链。运行时某进程按 `libEGL.so`（无版本 SONAME）
   查找，在工作区目录能命中软链、在 /opt 前缀却落空 → 回退到系统里被华为劫持的
   libEGL → `MESA-LOADER: failed to retrieve device information` → GPU 进程 channel error
   → 主进程段错误。二进制 md5 全一致、环境变量全一致，唯独缺这条软链。
   排错方法：把 deb 解包到可写前缀，逐项 A/B 替换 `LD_LIBRARY_PATH` 的 SHIM/MESA 路径，
   锁定到目录内容差异。教训：打包脚本要用 `cp -a` 连同软链一起拷，且解包后端到端实测。
6. **别把不相干的两件事耦合进同一个状态机**：为放行首次语言引导引入的 `FIRST_RUN`
   状态机，把整段 `user.js` 注入包进了 `if FIRST_RUN==0`，结果首次启动不写 `user.js`，
   导致 DPI(`layout.css.devPixelsPerPx=2.0`) 与沙箱横幅隐藏首次全部失效。实际上语言引导由
   OS locale + langpack 自动触发，与 `user.js` 无关。修法：`user.js` 每次都写，
   状态机只保留"是否传 URL"这一个职责（带 URL 会跳过 about:welcome）。

## 九、在已有 profile 上重放首次运行向导

首次运行向导 `about:welcome`（含"使用简体中文继续浏览"语言引导）的出现由三个首启标记共同决定
（Firefox 160 源码 `AboutWelcomeParent.sys.mjs` / `AboutWelcomeDefaults.sys.mjs`）：

| pref | 作用 |
|---|---|
| `trailhead.firstrun.didSeeAboutWelcome` | 看过向导 → true，首启向导不再弹 |
| `browser.aboutwelcome.didSeeFinalScreen` | 看完最后一屏 → true |
| `browser.startup.homepage_override.mstone` | 版本里程碑；缺失或与当前版本不符 → 判定首次/升级 |

另外**带 URL 启动会跳过向导**（`about:welcome` 属于首页覆盖，只有无参数启动才走）。

两条重放途径：

1. **仅查看向导页**：地址栏直接输入 `about:welcome`（源码中它是可直接导航的内部页面，
   `document.location.href === "about:welcome"` 即渲染向导）。不重置任何标记、不改 profile。
2. **完整重放首启体验**：`FF_REPLAY_FIRST_RUN=1 firefox`。启动器在 exec 前 sed 删除上述
   三个标记（不动 `intl.locale.requested` 语言偏好、不动任何硬件加速 pref），且不传 URL，
   于是重新弹出向导且中文界面保留。验证（mock + 真实 pref 结构）：首启标记全部清除、
   语言偏好与 DPI/横幅 pref 均保留、exec 参数不含 URL。

## 十、分发适配（依赖 / DPI / 版本）

1. **依赖 eglrun**：纯 Wayland 下输入法（搜狗/fcitx）能否工作，依赖 eglrun 包 postinst
   部署的系统级配置——KWin `[Wayland] InputMethod=wayland-im-shim` 与 CPIS/fcitx 自愈
   systemd 服务。这不是 Firefox 启动器能自己挂垫片解决的（wayland-im-shim 是 KWin 合成器
   侧的客户端，作用于全局而非单个应用）。因此把 `eglrun` 写入 deb 的 `Depends`，
   与 eglrun 自身的 chrome 包口径一致；`libc6` 是几个 shim 的唯一系统库依赖。
2. **DPI 动态化**：早期把 `layout.css.devPixelsPerPx=2.0` 写死，仅适用本机（DDE 缩放 2.0、
   Xft.dpi 192）。分发到其他缩放比例的机器会失配。改为启动器每次按优先级动态解析：
   `FF_DPI_OVERRIDE` → DDE `com.deepin.xsettings scale-factor`（与 eglrun 同口径）
   → `Xft.dpi/96` → GNOME `scaling-factor` → 兜底 `1.0`，用占位符代入 `user.js`。
   验证四态：默认=2.0、override=1.25、屏蔽 gsettings 回退 Xft=2.00、再屏蔽 xrdb 兜底 1.0，
   占位符无残留。注意 Firefox 在 Wayland 下读不到 KWin 的 wl_output 缩放（常误报 1x），
   所以必须显式注入而不能依赖自动探测。
3. **版本对齐**：deb `Version` 从占位 `1.0` 改为 Firefox 实际版本 `160.0`，
   包名文件名同步为 `firefox.appimage_160.0_arm64.deb`，便于与目标 Firefox 版本对应、
   升级时判断新旧。
