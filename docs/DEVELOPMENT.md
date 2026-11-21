# 开发主线（2026-11-16 → 11-21）

本文串起三天内的关键转折。原始逐日日志见 [`devlog/`](devlog/)，更完整的技术论证见
[`CHROME果冻问题-完整交接文档.md`](CHROME果冻问题-完整交接文档.md) 与
Firefox 的 `RESEARCH.md`。日期与版本号均按当时系统实测记录。

## 平台背景

UOS 20（DDE 桌面）/ Kirin 9000c / Maleoon 910（aarch64）。华为深度定制了图形栈：
`libEGL.so.1` / `libGLESv2.so.2` / `libgbm.so.1` 被替换为 `libGFX_hisi.so.0.8.0`，
缺少 GLVND 标准符号；Maleoon 的 Vulkan 走私有 `libhvgr.so`，缺 `VK_EXT_provoking_vertex` 等扩展；
VAAPI 由 `vacore` 驱动提供，但只报告 VA-API 1.14，低于 Chrome 157 的 1.17 门槛。
这三点决定了三条适配路线：Wayland 同步垫片、Vulkan ICD wrapper、VAAPI 版本垫片。

## 阶段一：Firefox 先行（11-16 ~ 11-19）

Firefox 目标形态从一开始就被约束为：**原厂 AppImage 原样运行，不解包、不改任何二进制字节**，
全部能力靠启动器 + `LD_PRELOAD` / `LD_LIBRARY_PATH` / 子进程环境注入实现。

关键结论（详见 Firefox `RESEARCH.md`）：

- **翻案 1**：系统默认浏览器的"硬件 WebGL"是假象，多为软渲染，不能作为依据。
- **翻案 2**：Mesa EGL 的 `dma_buf_import` 零拷贝是假象。
- **翻案 3（最关键）**：给 libxul 打 3 字节补丁并非 AppImage 必需 —— 改为在
  `execve` / `posix_spawn` 边界用 `gfxtest-env-shim` 给 GL/编解码探测子进程注入等价环境，
  使 VAAPI/H264 探测通过，挂载点原版二进制照跑。
- **翻案 4**：无需解包 AppImage 替换 gfxtest。
- 三类崩溃被分开定位：华为 `libGFX` 析构 UAF（用打掉 UAF 的库副本解决）、Wayland 队列销毁警告洪泛
  （`wllog-shim` 按模块忽略）、CSD 三按钮缺失（`csd-shim` 强制 EGL client-side decoration）。
- 滚轮修复 `wheel-shim`：拦 `wl_proxy_add_listener`，WHEEL → CONTINUOUS，倍率 0.30。
- 输入法最初随 eglrun 一起部署；后续（11-20）独立拆分为 `uoshim-wayland-im`。

## 阶段二：Chrome 果冻问题与 Wayland 路线（11-16 ~ 11-20）

Chrome 157 走 ANGLE surfaceless → GBM/dma-buf → Wayland dmabuf，跨进程缺 fence，滑动时出现果冻 / 残留 / 闪烁。

- 把 `eglrun` 从 1.0.11 迭代到 **1.0.15 release**：GL 硬件合成 + `wire-damage-shim` 逐帧相位锁定
  （HYBRID 策略 `k = ceil(net/T)`，T≈16678us，kmax=4，hold=8），把抖动大幅减轻。
- 所有显式 / 隐式 fence、KWin 同步、flag toggle、DPI 路线均被数据证伪或引发崩溃。
- kprobe 证伪：Chrome / Firefox 的 `create_dma_fence` / `wait_dma_fence` 全为 0 —— 华为走私有 hvgr token，
  fence 路线不可行。`maleoon_buffer_sharing` 改道也被证伪：Chrome 根本不 bind name=52，自建 proxy 需 DRM magic
  被 KWin 拒，注入 object id 触发 `0x40000001` 断连。
- Wayland wire 协议细节被固化（消息头 8B = obj+opcode+size；bind 的 slen 含 null 按 4B 对齐；
  add=op1/sz28 且 fd 走 SCM_RIGHTS 不占 body；create_immed=op3/sz28）。
- 但用户明确要求：**不锁相**。该路线虽能消闪，终因锁相约束被放弃。

## 阶段三：转向 X11 + ANGLE/Vulkan（11-20 ~ 11-21）

用户纠正方向：停止 zink 实验，改走 XWayland + Vulkan。决定性突破：

- 在 `--ozone-platform=x11` 下加入 `--enable-features=Vulkan`，A/B 证实 display compositor 从 SwiftShader
  软件回退切到 Maleoon 硬件：`gpu_compositing=enabled`、`webgl=enabled`、dma-buf>0、swiftshader=0、ReadPixels=0。
- **Vulkan → ES3/WebGL2 的根因（ANGLE 源码铁证）**：`RendererVk::getMaxSupportedESVersion()` 中若
  `provokingVertex` feature 未启用就把 maxVersion 砍到 ES2；而该 feature 只在设备扩展含
  `VK_EXT_provoking_vertex` 时启用，Maleoon 无此扩展。之前所有 forge（feature/vendor/api/name/devtype/BC）
  无效，因为都不碰这一点。
- 修法：写 **ICD wrapper**（`chrome-adaptive-shim/vk-angle-shim/icd-wrap.c`）——注意是 ICD 层 wrapper
  而非 dlsym hook（Chrome 157 对 gipa 指针身份校验，wrap 会静默退出）。它在枚举设备扩展时注入
  `VK_EXT_provoking_vertex`、在 `vkCreateDevice` 的 feature pNext 链里填 `provokingVertexLast=1`，
  真调用 ICD 前再移除该扩展并 scrub pNext 节点（替换成 `VK_KHR_surface` 会返回 -7）。同时修正了
  硬编码的 pNext struct size（Vk11=64/Vk12=204/Vk13=76）与扩展注入越界覆盖真实扩展的 bug。
- 第二个关键点：flag 必须是 `--enable-features=VulkanFromANGLE,DefaultANGLEVulkan`（单用 `Vulkan`
  与 ozone=wayland 冲突导致 Skia 失败；但在 ozone=x11 下 `Vulkan` 是让合成器切硬件合成的关键，二者都保留）。
- 硬件合成判据确认为：GL 路径（eglrun 1.0.15）与 X11+Vulkan 路径都曾有真实 dma-buf；Vulkan WSI 在
  Maleoon 上无法做到硬件合成的说法经复验被推翻 —— x11 下 ANGLE-Vulkan 的 `VK_KHR_xcb_surface`
  经 XWayland DRI3/Present 上屏即为硬件链，实测 dmabuf=3、无 SwiftShader。

## 阶段四：eglrun 2.x 收口（11-21）

- **2.0.0**：以 X11 + ANGLE/Vulkan 为唯一路线重写启动器，去掉所有 Wayland 参数与四个旧垫片、
  去 `no-sandbox`、去输入法依赖、去 check-accel，保留 ICD 源码。端到端验证全绿。
- **2.0.1**：把 `gbm-hw-shim.c` 里的 VAAPI 逻辑单独提取为 `vaapi-shim.so` —— `dlsym` 钩子只拦
  `vaInitialize`，把 `vacore` 的 VA-API 1.14 改报 1.17，constructor 补回被 zygote 清掉的 `LIBVA_*` 变量。
  A/B 铁证：`HISI_VAAPI_SPOOF=0` 精确复现 `Installed VAAPI version is too old, min 1.17 installed 1.14`，
  开启后该错误消失且 `video_decode=enabled`。
- **2.0.2**：启动器通用化为 `eglrun /opt/xxx/yyy [args...]`，删除窗口尺寸 pin、user-data / profile、
  DPI 设置、显式 DISPLAY 及 8 个冗余 flag，只注入 6 个必要 flag。所有安装文件属主统一为 `root:root`
  （打包用 `dpkg-deb --build --root-owner-group`）。

输入法 `uoshim-wayland-im` 在 1.2 版从 eglrun 中彻底独立：KWin 通过 `.desktop` 描述符拉起
`wayland-im-shim`；CPIS uinput DBus 策略由 postinst 安装到 `/etc`（不进 payload，规避 dpkg 陈旧属主记录），
保持与 eglrun 解耦。

## 结局

三件套形成稳定生产态：eglrun 2.0.2（Chrome/Electron 的 X11+Vulkan 硬件合成 + VAAPI 硬解）、
uoshim-wayland-im 1.2（纯 Wayland 中文输入）、firefox.appimage 160.0（原厂 AppImage 硬件加速）。
端到端判据：WebGL2 renderer = ANGLE Maleoon 910；`gpu_compositing` / `webgl` / `rasterization` /
`video_decode` 全 enabled；GPU 进程映射 `libhvgr` + `icd-wrap` + `vaapi-shim`，swiftshader=0，dmabuf>0；
窗口缩放多次无崩溃。
