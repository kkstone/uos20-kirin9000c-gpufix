# eglrun 2.0.2

HUAWEI Kirin 9000c / Maleoon 910（UOS 20）上的 Chrome / Electron 应用 GPU 启动器：
WebGL 渲染与画面合成走真 GPU，视频解码走硬件 VAAPI。

## 它做什么

`eglrun` 接收一个 Chrome 或 Electron 可执行文件路径，为它应用经过验证的
**X11 + ANGLE/Vulkan** GPU 栈与 VAAPI 视频解码修复，然后把应用原样 exec 起来。
显示、DPI、用户数据目录等都来自调用方环境或应用自身默认值。

拦截只发生在两处，各司其职：

- **Vulkan 层**：`lib/icd-wrap.so` 经 `VK_ICD_FILENAMES` 加载，在 Vulkan 驱动
  接口层补齐 Maleoon 缺失的能力（如 `provoking_vertex`，否则 ANGLE 会把 WebGL2
  降为 WebGL1），并用真驱动导出 dma-buf。
- **VAAPI 层**：`lib/vaapi-shim.so` 经 `LD_PRELOAD` 加载，把 `vacore` 驱动报告的
  VA-API 1.14 提升为 1.17 以通过 Chrome 的 VAAPI 版本门槛；它的 `dlsym` 钩子只
  包裹 `vaInitialize`，其余符号原样透传。

## 工作原理

```
应用 (Chrome / Electron)
  ├─ ANGLE (Vulkan 后端)
  │    └─ libvulkan → ICD wrapper (lib/icd-wrap.so)
  │                     └─ libhvgr.so  (Maleoon 910, Vulkan 1.2)
  │                          └─ VK_KHR_xcb_surface
  │                               └─ XWayland → DRI3/Present → KWin 上屏
  └─ VAAPI 视频解码
       └─ libva → vaapi-shim.so → vacore_drv_video.so (1.14 → 1.17)
```

启动器为应用设置的 flag：

- `--ozone-platform=x11`：使用 X11 的 Vulkan WSI 上屏链路。
- `--use-gl=angle --use-angle=vulkan`：WebGL 走 ANGLE 的 Vulkan 后端。
- `--enable-features=Vulkan,VulkanFromANGLE,DefaultANGLEVulkan,...`：
  其中 `Vulkan` 使 Chrome 的显示合成器切到硬件合成（关键）；
  `VulkanFromANGLE`/`DefaultANGLEVulkan` 让 ANGLE 默认选 Vulkan。
- `--enable-gpu-compositing --ignore-gpu-blocklist`：启用 GPU 合成。

环境变量：

- `VK_ICD_FILENAMES` 指向包内 manifest（`lib/maleoon-wrap.json`）；
  manifest 的 `library_path` 指向 `lib/icd-wrap.so`。
- `LIBVA_DRIVER_NAME=vacore`、`LIBVA_DRIVERS_PATH=/usr/lib` 让 libva 直接找到
  HUAWEI 视频驱动；VAAPI 垫片在每个子进程补回这些变量。

## 用法

```sh
eglrun /opt/google-chrome/chrome                     # 启动 Chrome
eglrun /opt/google-chrome/chrome https://example.com # 附带 URL
eglrun /opt/some-electron-app/app --flag value       # 启动 Electron 应用
```

应用参数原样透传。`--chrome <path>` 与 `--electron <path>` 为兼容写法，
`<path>` 即应用可执行文件路径。

排查时保留本次会话的输出：

```sh
EGLLRUN_DEBUG=1 eglrun /opt/google-chrome/chrome
```

各层自身的调用跟踪默认关闭，需要时单次运行加开关：
`ICD_WRAP_DEBUG=1`（ICD wrapper）、`VAAPI_SHIM_DEBUG=1`（VAAPI 垫片）。
`HISI_VAAPI_SPOOF=0` 可临时关闭 VAAPI 版本伪装用于对照。

## 验证硬件合成是否生效

启动后用 CDP 或 `chrome://gpu` 检查：

- `gpu_compositing = enabled`，`webgl = enabled`，`rasterization = enabled`
- WebGL renderer 为 `ANGLE (ARM, Vulkan ... Maleoon 910)`
- GPU 进程映射了 `libhvgr.so` 与 `icd-wrap.so`
- GPU 进程持有 dma-buf 文件描述符（`/proc/<pid>/fd` 中的 `dmabuf:*`）

## 验证视频解码是否生效

- `chrome://gpu` 的 Video Decode 一项显示为硬件加速（VAAPI）。
- 播放 H.264 视频后，GPU/视频进程映射了 `vaapi-shim.so` 与 `vacore_drv_video.so`。
- 单次调试运行 `VAAPI_SHIM_DEBUG=1 eglrun ...` 可看到
  `[vaapi-shim] vaInitialize 1.14 -> 1.17`。

## 目录

```
/opt/eglrun/
├── bin/eglrun                      启动器
├── lib/
│   ├── icd-wrap.so                 ICD wrapper
│   ├── maleoon-wrap.json           ICD manifest
│   └── vaapi-shim.so               VAAPI 垫片
├── src/
│   ├── icd-wrap.c                  ICD wrapper 源码
│   ├── build.sh                    重建 icd-wrap.so
│   ├── vaapi-shim.c                VAAPI 垫片源码
│   └── build-vaapi-shim.sh         重建 vaapi-shim.so
├── VERSION                         2.0.2
└── README.md
```

两份 `.so` 的源码均随包提供，可用对应的 `src/build*.sh` 重新编译。
