# Firefox（原厂 AppImage）— UOS20 / Kirin 9000c / Maleoon 910 硬件加速版

这是一个 **deb 安装包**，把原厂 Firefox AppImage 和一套本地硬件加速垫片打包在一起，
让 Firefox 在这台机器（UOS20 / Kirin 9000c / Maleoon 910）上获得：

- 原生 Wayland + Firefox 自绘 CSD 三按钮（最小化 / 最大化 / 关闭）
- WebRender / WebGL 硬件渲染（Maleoon 910）
- H.264 VAAPI 真硬解（vacore / vadec）

**核心原则：AppImage 原样运行，不解包、不改任何二进制字节。** 所有加速都靠
`LD_PRELOAD` / `LD_LIBRARY_PATH` 层的垫片和子进程环境注入实现，零字节补丁。
详见同目录的 `RESEARCH.md`（研究与验证记录）。

---

## 1. 安装 / 卸载

```bash
# 安装（需要 root）
sudo dpkg -i firefox.appimage_160.0_arm64.deb

# 卸载
sudo dpkg -r firefox.appimage
```

安装后所有文件和目录的属主都是 `root:root`。

**依赖**：`libc6 (>= 2.28)` 与 `eglrun`。`eglrun` 提供原生 Wayland 输入法链路
（其 postinst 把 KWin `[Wayland] InputMethod=wayland-im-shim` 写入系统配置，并部署
CPIS/fcitx 自愈服务）——搜狗/fcitx 在纯 Wayland 下能否输入依赖这套系统级配置，
Firefox 启动器本身不挂输入法垫片。未安装 eglrun 时 dpkg 会报依赖缺失。

## 2. 运行

```bash
firefox                 # 打开新标签页
firefox https://example.com
```

也可以直接点应用菜单里的 Firefox 图标——包内原装 `.desktop` 的 `Exec=firefox %u`
正好命中 `/usr/local/bin/firefox` 启动器。

profile 位于 `$HOME/.mozilla/firefox/mozilla-firefox-appimage/`，与系统其他 Firefox profile 互不干扰：

- **每次启动**都会创建 profile 目录（Firefox 的 `-profile` 要求目录已存在，否则报
  "profile cannot be loaded"），并注入硬件加速与 UI 所需的 `user.js`（不含个人数据）。
  DPI、沙箱横幅隐藏等偏好**首次即生效**——语言引导由 OS locale + langpack 自动触发，
  与 `user.js` 无关，两者不再耦合。

**DPI 缩放不写死**：启动器每次按优先级动态取值并注入 `layout.css.devPixelsPerPx`：
`FF_DPI_OVERRIDE` 手动覆盖 → DDE `com.deepin.xsettings scale-factor` → `Xft.dpi/96`
→ GNOME `scaling-factor` → 兜底 `1.0`。分发到不同缩放比例的机器会自动适配；
特殊机器可用 `FF_DPI_OVERRIDE=1.25 firefox` 强制指定。
- **首次启动**（profile 内无 `prefs.js`）：不传 URL，走原厂首次运行流程——弹出
  "使用简体中文继续浏览"引导，点选后自动下载中文语言包（需联网）。
- **引导完成后**：后续启动传 `about:newtab`，且 `user.js` 不含任何 locale 项，
  不覆盖你在引导里选的中文，界面语言保留。

**在已有 profile 上重放首次运行向导**（用于复测首次启动的 DPI / 横幅 / 语言引导）：

```bash
FF_REPLAY_FIRST_RUN=1 firefox
```

该开关清掉 profile 里的首启标记（`trailhead.firstrun.didSeeAboutWelcome`、
`browser.aboutwelcome.didSeeFinalScreen`、`homepage_override.mstone/buildID`），
不动语言包和硬件加速偏好，因此下次启动会重新弹出 `about:welcome` 向导而中文界面仍在。
另一种等价做法是在 Firefox 地址栏直接输入 `about:welcome`（不重置任何标记，仅查看向导页）。

夜间模式跟随系统：启动器读取 `~/.config/gtk-3.0/settings.ini` 的 `gtk-theme-name` 注入 `GTK_THEME`
（Wayland 下 Firefox 默认读不到 Deepin 主题）。注意只在启动时读取，运行中切换系统主题需重启 Firefox。

## 3. 安装后布局

```
/opt/mozilla-firefox/
├── README.md                     本文档
├── RESEARCH.md                   研究与验证记录（原理 / 根因 / A/B / 结论）
├── bin/
│   └── firefox-bin               原厂 Firefox 160.0 AppImage（零字节补丁）
├── lib/                          硬件加速垫片
│   ├── wllog_shim.so             过滤 Wayland 队列销毁警告，防日志洪泛
│   ├── csd_shim.so               强制 EGL client-side decoration → 自绘三按钮
│   ├── wheel_shim.so             修复鼠标滚轮（WHEEL 轴事件重建 + 倍率 0.3）
│   ├── gbm-hw-shim.so            华为 GBM ABI 桥 + VAAPI 硬解修复
│   ├── gfxtest_env_shim.so       在 exec/spawn 边界给 gfxtest 探测子进程换环境
│   ├── ff-drm-device-shim.so     DRM 设备节点伪装
│   ├── libGFX_hisi.so.0.8.0      打掉析构 UAF 的华为 GPU 库副本（about:support 不崩的关键）
│   ├── egl-huawei-v12/           主进程 EGL 分流垫片（libEGL.so.1 + libEGL.so 软链，两者缺一不可）
│   ├── egl-fbspoof/              gfxtest 用 GL_VERSION / device string 伪装
│   └── mesa-egl-glvnd/           真实 Mesa GBM / GLESv2（系统版被华为库替换）
└── src/                          全部垫片源码 + 启动器源码 + README
/usr/local/bin/firefox            启动器（写 user.js / 设环境变量 / exec AppImage）
/usr/share/applications/firefox.desktop   原厂 desktop（AppImage 内原样）
/usr/share/pixmaps/firefox.png            原厂图标
```

## 4. 验证硬件加速是否生效

1. `about:support` → Graphics 段：`WebGL Renderer` 应显示 Maleoon 910 / HUAWEI；
   进入该页**不应崩溃**（历史上这里必崩，是本方案修掉的核心问题之一）。
2. WebGL 固定测试页（本项目统一用它验证）：

   ```
   https://threejs.org/examples/#webgl_animation_keyframes
   ```

3. 视频硬解：终端启动时 vadec / vcodec 驱动应有输出；播放 H.264 不应退回软解。
4. 退出 Firefox 时终端**不应出现** `段错误 (核心已转储)`。

## 5. 重编译垫片

源码在 `src/`，例如重编译 gfxtest 环境垫片：

```bash
sudo gcc -shared -fPIC -O2 -o /opt/mozilla-firefox/lib/gfxtest_env_shim.so \
    /opt/mozilla-firefox/src/gfxtest-env-shim/gfxtest_env_shim.c -ldl
```

各垫片源码目录自带有编译方式；`egl-huawei-v12` 需要 EGL 头文件与 `-DEGL_NO_X11`。
具体见 `src/README`。

## 6. 已知限制 / 设计取舍

- **沙箱保持关闭**：私有华为 shim 和 GPU 设备节点与 Firefox 沙箱不兼容，启动器设置了
  `MOZ_DISABLE_*_SANDBOX=1`；"沙箱已关闭"的横幅已用官方 pref + 用户样式表隐藏。
- **只针对本机**：本包是为 Kirin 9000c / Maleoon 910 的特定软件栈定制的，换到别的机器
  （尤其非华为 GPU / 非 UOS20）垫片可能不适用甚至起反作用。
- **滚轮倍率**：当前 `0.3`（见 `wheel_shim`），触摸板双指滚动不受影响。
