# eglrun 1.0.15 — Release 模式打包交付说明

> 日期：2026-11-21
> 交付物：`eglrun_1.0.15_arm64.deb`（md5 `d1457a61de0f868d1a5267b9b25d0a72`）
> 已安装于 `/opt/eglrun`（覆盖 1.0.14-hw），命令 `eglrun` / `google-chrome` 均指向它。
> 上一版 `eglrun_1.0.14-hw_arm64.deb` 保留在桌面，可回滚。

---

## 一、本版目标：Release 模式打包

功能与 1.0.14-hw 完全一致（硬件合成 + VSYNC 相位锁定 + HYBRID 混合目标），
本版**只做打包卫生与运行卫生**，不改任何已验证的算法参数。

### 依赖 uoshim 输入法

```
Depends: bash, coreutils, uoshim-wayland-im
```

- 依赖已就位（`uoshim-wayland-im 1.2 ii`），`dpkg -i` 时自动校验。
- **eglrun 与输入法彻底解耦**：不写 kwinrc、不碰 CPIS policy、包内无任何输入法二进制或 desktop 文件。
- 安装后实测：kwinrc `InputMethod=` 仍指 `uoshim-wayland-im.desktop`，
  CPIS policy md5 仍为 `752d261e1e5da183f678230e9a6b31e1`，`/opt/eglrun/bin/` 只有
  `eglrun`、`env.sh`、`check-accel` 三个文件。

---

## 二、清理了什么

### 2.1 日志：默认完全静默，且不落任何文件

审计发现**三个垫片都会在默认情况下写文件或打 stderr**，这是 release 的硬伤：

| 垫片 | 原行为 | 修复后 |
|---|---|---|
| `cursor-wire-shim.so` | **完全无条件**写 `/tmp/cursor-wire-shim.log`，每次识别到光标就写一行 | 需 `CURSOR_SHIM_DEBUG=1` 才写 |
| `wire-damage-shim.so` | constructor/destructor **无条件**写 `/tmp/wire-commit-shim.log`（连 `WIRE_DEBUG` 都没设也写） | 需 `WIRE_DEBUG=1` 或 `WIRE_COMMIT_LOG=<非空路径>` 才写 |
| `gbm-hw-shim.so` | `GPU_FINISH_DEBUG` 用 `getenv()` **存在性**判断 —— 设成 `=0` 仍会触发写 `/tmp/gpu-finish-shim.log` | 改为严格 flag 语义：`=1` 开、`=0` 明确关 |

`pointer_guard.so` 与 `gbm-hw-shim.so` 的 stderr 输出原本就受 `debug_enabled()` 门控，保持不变。

### 2.2 环境变量：env.sh 只留运行必需项

删除（对 release 运行无作用、且是研究期遗留）：

```
WIRE_DAMAGE_FULL        WIRE_ADAPTIVE_MODE     WIRE_ADAPTIVE_GAIN
WIRE_ADAPTIVE_ALPHA     WIRE_ABS_ALPHA         WIRE_ABS_HEADROOM_US
WIRE_COMMIT_LOG         （日志路径，默认不写文件）
```

保留（锁相算法实际读取的参数），并统一为「默认值 + 可覆盖」形式，
用户在 shell 里临时导出即可覆盖，不需要改文件：

```
WIRE_ADAPTIVE=1  WIRE_COMMIT_DELAY_US=8000  WIRE_ADAPTIVE_MAX_US=60000
WIRE_LOCK=1  WIRE_LOCK_K=2  WIRE_LOCK_T=16678
WIRE_LOCK_HYBRID=1  WIRE_LOCK_HYBRID_KMAX=4  WIRE_LOCK_HYBRID_HOLD=8
GPU_FINISH_SYNC=1
```

### 2.3 其他

- `check-accel` 里还写着「Compositing: Software only (intentional: software-compositing build)」，
  这是 1.0.13-nocomp 时代的文案，与本版硬件合成矛盾 → 已改正。
- `build.sh` 结尾的 `md5sum` 校验提示 → 删除。
- `src/` 源码保留在包内（可复现编译，已验证重编后符号表与 .text 段完全一致），
  但源码里的版本演进注释保留有价值的技术记录。
- 包内**零** `.log` / `.bak` / `.tmp` / `.orig` 残留文件（已用 `find` 核验）。

---

## 三、验收结果

### 3.1 Release 卫生（真实 Chrome 跑 35 秒）

```
✓ 无 /tmp/wire-commit-shim.log
✓ 无 /tmp/cursor-wire-shim.log
✓ 无 /tmp/gbm-hw-loaded.txt
✓ 无 /tmp/gpu-finish-shim.log
垫片 debug 噪声（stderr）：0 行
GPU 降级（swiftshader/fallback）：0 次
```

### 3.2 开关双向验证（单元级）

| 场景 | 期望 | 结果 |
|---|---|---|
| 无任何开关 | 不产生日志 | ✓ |
| `WIRE_DEBUG=1` | 产生 wire 日志 | ✓ 387 bytes |
| `WIRE_COMMIT_LOG=<path>` | 产生 wire 日志 | ✓ |
| `CURSOR_SHIM_DEBUG=1` | 产生 cursor 日志 | ✓（正确识别 48x48 光标 surface） |
| `CURSOR_SHIM_DEBUG=0` | 不产生 | ✓ |
| `HISI_GBM_SHIM_DEBUG=1` | 产生 gbm 日志 | ✓ 113 bytes |
| `GPU_FINISH_DEBUG=0` | 不产生（修复前会误触发） | ✓ |

### 3.3 功能未被破坏（release 包端到端，three.js）

```
帧数=602  锁相率=100.00%  失锁帧=0 (0.00%)
S p50=33356us -> 30.0fps
k 分布={2: 559, 3: 35, 4: 8}
net p50=12986us p90=21227us max=57990us
```

k=3/k=4 出现证明 HYBRID 升档机制真实生效（不是退化成固定 k）。

---

## 四、排错开关（默认全关，需手动开）

| 变量 | 效果 |
|---|---|
| `WIRE_DEBUG=1` | 逐帧锁相轨迹 → `/tmp/wire-commit-shim.log` |
| `WIRE_COMMIT_LOG=/path/x.log` | 同上，指定路径 |
| `GPU_FINISH_DEBUG=1` | glFinish 命中计数 → `/tmp/gpu-finish-shim.log` |
| `HISI_GBM_SHIM_DEBUG=1` | GBM 桥接调用轨迹 → stderr + `/tmp/gbm-hw-loaded.txt` |
| `CURSOR_SHIM_DEBUG=1` | 光标 surface 改写轨迹 → `/tmp/cursor-wire-shim.log` |
| `POINTER_GUARD_DEBUG=1` | 指针 listener 包装信息 → stderr |

```bash
WIRE_DEBUG=1 eglrun /opt/google-chrome/chrome    # 只影响这一次启动
```

---

## 五、桌面上的历史包

| 文件 | 说明 |
|---|---|
| `eglrun_1.0.15_arm64.deb` | **本版**，已安装 |
| `eglrun_1.0.14-hw_arm64.deb` | 上一版，功能同，保留可回滚 |
| `eglrun_1.0.13-nocomp_arm64.deb` | 软件合成版（旧路线） |
| `eglrun_1.0.12-nocomp_arm64.deb` | 软件合成版（旧路线） |
| `eglrun_1.0.11_arm64.deb` | 更早版本 |

后三个已不再使用，但按「不擅自删除个人文件」原则保留。需要清理请明确告知。

---

## 六、回滚

```bash
sudo dpkg -i ~/Desktop/eglrun_1.0.14-hw_arm64.deb    # 回上一版
sudo dpkg -i ~/Desktop/eglrun_1.0.13-nocomp_arm64.deb  # 回软件合成版
```

输入法包不受影响（独立安装、独立配置）。
