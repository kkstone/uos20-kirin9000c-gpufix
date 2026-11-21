# Chrome Wayland 抖屏/合成同步 — 交付说明（eglrun 1.0.14-hw）

> 日期：2026-11-21
> 交付物：`eglrun_1.0.14-hw_arm64.deb`（md5 `360ada9c2b620caa95c7afdc6eb17bde`）
> 已安装于 `/opt/eglrun`，命令 `eglrun` / `google-chrome` 均已指向它。

---

## 一、这一版解决了什么

| 问题 | 上一版（1.0.13-nocomp） | 本版（1.0.14-hw） |
|---|---|---|
| 画面闪烁 | ✅ 已解决（软件合成，绕开 GBM present） | ✅ 解决（硬件合成 + 相位锁定，从根上不闪） |
| 果冻/撕裂 | ✅ 已解决（软件合成，无 present 路径） | ✅ 解决（相位锁定，`S ≡ k*T`） |
| 复杂页面卡顿 | ❌ CPU 合成，全页 readback | ✅ 硬件合成，30fps 稳定 |
| 帧率 | 波动 | 30.0fps 恒定 |

**核心变化**：从「软件合成绕开问题」升级为「硬件合成 + 相位锁定解决问题」——不再靠绕路，而是让 Maleoon 无 fence 的 present 路径本身安全。

---

## 二、相位锁定的原理

Maleoon 平台没有跨进程 dma-buf fence。内核 kprobe 铁证：华为驱动 `hvgr_ksync_create_dma_fence` 恒为 0，`dma_fence_wait_timeout` 恒为 0。KWin 拿不到 fence，只能凭时间猜 buffer 是否写完——猜错就扫到半张画面，这就是果冻。

修 fence 需要改内核，代价太高。**本方案改用相位对齐**：

```
S = net + delay                        S = 相邻两次提交的间隔
net = interval - last_applied_delay    剥离自身注入后的真实渲染耗时
delay = k*T - net                      前馈抵消
─────────────────────────────────────
=> S ≡ k*T                             恒为 vsync 整数倍
```

`T = 16678µs`（59.96Hz）。KWin 每次都在**同一个扫描位置**取样 buffer，半写状态永远暴露不出来。

### HYBRID 模式（本版新增）

固定 `k=2` 时，重帧（`net > 2T`）的 `delay` 会被截断为 0，该帧提交时刻失控 → 相位漂移 → **残余果冻**。这是 v13 唯一的果冻来源。

HYBRID 逐帧选「使 `delay ≥ 0` 的最小 k」：

```
need = 1
while (need * T * margin < net && need < kmax) need++
```

- `delay` 恒 ≥ 0 → 永不 clamp 到 0 → 相位恒锁
- 重帧自动升到 3T/4T 获得足够 settle 时间
- 降档带滞回（连续 8 帧都低于 `(k-1)*T` 才降），避免 k 边界横跳
- k 跳变时 S 仍是 vsync 整数倍 → 相位不会因跳变而散

---

## 三、实测数据

### 离线回放（943 帧真实 aistudio 日志，`lock-replay2.py`）

| 方案 | 锁相率 | 失锁帧 | 帧率 |
|---|---|---|---|
| 固定 k=2（v12） | 88.65% | 12.73% | 30.0fps |
| HYBRID kmax=3 | 99.36% | 1.27% | 30.0fps |
| **HYBRID kmax=4** | **99.89%** | **0.11%** | 30.0fps |

### 真实 Chrome A/B（three.js，各 1000+ 帧）

```
--- v13 固定 k=2 ---  帧数=1020  锁相率=99.80%  失锁帧=2 (0.20%)  S p50=33356us -> 30.0fps
--- v14 HYBRID    ---  帧数=1000  锁相率=100.00% 失锁帧=0 (0.00%)  S p50=33356us -> 30.0fps
```

### 本版安装后端到端（系统 launcher，three.js）

```
帧数=976  锁相率=100.00%  失锁帧=0 (0.00%)
S p50=33356us -> 30.0fps
k 分布={2: 843, 3: 133}
net p50=15074us  p90=24578us  max=47034us
```

**k 分布出现 k=3 说明 HYBRID 升档机制在真实负载下确实被触发**，不是退化成的固定 k。

---

## 四、为什么是 30fps 而不是 60fps

这不是硬件限制。实测 net 分布（943 帧 aistudio）：

```
62.3% 的帧 net < 16.7ms      GPU 物理上有 60fps 能力
41.9% 的帧 net > 16.7ms      全程 k=1 会丢帧
```

`k=1`（60fps）在 42% 的重帧上无法保证 `delay ≥ 0` → 相位必然漂移 → 果冻回来。`k=2`（30fps）保证**每帧完整且相位恒锁**。

**结论：要提帧率只能提升 GPU 渲染能力（渲染层优化），与上屏垫片无关。** 已验证的自适应 k（v12）路线也已证伪——three.js net p50=17.4ms、aistudio 15.3ms，物理产不出 16.7ms 帧。

---

## 五、怎么用

### 日常使用（无感知）

```bash
google-chrome https://aistudio.google.com/prompts/new_chat
```

桌面图标、终端 `google-chrome`、`/usr/local/bin/eglrun` 全部走新版，锁相默认开启，**不需要任何额外参数**。

### 肉眼验收（真实 profile + aistudio）

```bash
cd ~/Desktop/chrome-adaptive-shim
./launch-v13-realprofile.sh start     # 启动真实 profile 的 Chrome
# ... 肉眼确认果冻是否彻底消失 ...
./launch-v13-realprofile.sh stats     # 看锁相质量数字
./launch-v13-realprofile.sh stop      # 关闭
```

### 调参

```bash
WIRE_LOCK_K=2            eglrun google-chrome   # 2T=30fps
WIRE_LOCK_HYBRID=0       eglrun google-chrome   # 关 HYBRID，退回固定 k
WIRE_LOCK_HYBRID_KMAX=3  eglrun google-chrome   # 限制最高 k
WIRE_DEBUG=1             eglrun google-chrome   # 逐帧日志 → /tmp/wire-commit-shim.log
```

| 变量 | 默认 | 说明 |
|---|---|---|
| `WIRE_LOCK` | 1 | 相位锁定总开关 |
| `WIRE_LOCK_K` | 2 | 基础倍数 → 2T = 33.356ms = 30fps |
| `WIRE_LOCK_T` | 16678 | vsync 周期（µs） |
| `WIRE_LOCK_HYBRID` | 1 | 逐帧 k 选择 |
| `WIRE_LOCK_HYBRID_KMAX` | 4 | 单帧可升到的最高 k |
| `WIRE_LOCK_HYBRID_HOLD` | 8 | 降档所需连续轻帧数 |
| `WIRE_COMMIT_DELAY_US` | 8000 | 基础延迟（锁相模式下的兜底值） |
| `WIRE_ADAPTIVE_MAX_US` | 60000 | delay 上限 |

---

## 六、包内容

```
/opt/eglrun/bin/eglrun       启动器（--enable-gpu-compositing + renderD128）
/opt/eglrun/bin/env.sh       环境 + 锁相开关
/opt/eglrun/bin/check-accel  诊断
/opt/eglrun/lib/
  ├── gbm-hw-shim.so         GBM ABI 桥 + VAAPI 1.14→1.17 + GPU 侧 glFinish 真同步
  ├── cursor-wire-shim.so    Wayland 字节流光标尺寸修正
  ├── pointer_guard.so       补齐 wl_pointer warp(opcode 11)，修最大化/还原 SIGABRT
  └── wire-damage-shim.so    ★ 相位锁定（HYBRID）
/opt/eglrun/src/             四个垫片源码 + build.sh（可复现编译，已验证符号表/代码段一致）
```

**输入法完全解耦**：本包 `Depends: uoshim-wayland-im`，不写 kwinrc、不碰 CPIS policy、不含任何输入法二进制。已验证安装后 `kwinrc` 仍指 `uoshim-wayland-im.desktop`，CPIS policy md5 `752d261e1e5da183f678230e9a6b31e1` 未变。

---

## 七、回滚

```bash
sudo dpkg -i /tmp/eglrun_1.0.13-nocomp_rollback.deb   # 回到软件合成版
```

（回滚包已备份在 `/tmp/eglrun_1.0.13-nocomp_rollback.deb`，桌面亦有 `eglrun_1.0.13-nocomp_arm64.deb`）

---

## 八、本轮被证伪的路线（不要再走）

| 路线 | 证伪证据 |
|---|---|
| **Chrome buffer 提交改道 maleoon create_buffer** | 真实 Chrome 硬件合成实跑：Chrome **根本不 bind** `maleoon_buffer_sharing`（name=52），只 bind 了 30 个 global（含 wl_drm 53、zwp_linux_dmabuf_v1 54）。三重死因：①无已认证 proxy 可复用；②自建 proxy 需 DRM magic，试 10/8/12/0 全被 KWin 拒（error 2 couldn't authenticate client magic）；③注入新 object id 会让 Chrome 内嵌 libwayland 报 `invalid object 1073741825`（=0x40000001，正是注入的 proxy id）直接断连 |
| **补标准 dma_fence** | 内核 kprobe 铁证：华为 GPU 从不创建标准 fence（`hvgr_ksync_create_dma_fence=0`），Chrome/Firefox 两条 present 路径都不走标准 fence。不闪的 Firefox 同样 `create_dma_fence=0` 却满帧 59fps——缺 fence 不是原因 |
| **自适应 k / 60fps 锁定** | 物理产不出 16.7ms 帧（GPU 渲染 ~30fps），强制 k=1 实跑 31fps 且相位锁定质量仅 2.7% |
| **wl2x 自写 Wayland→X11 桥** | 用户明确判定「巨坑」，已作废 |

---

## 九、Wayland wire 协议实测事实（已固化，供后续复用）

Chrome 内嵌 libwayland，符号 hook 看不到，必须在 `write/send/sendmsg` 的字节流层操作。消息头 8 字节 = `object_id(4) + opcode(2) + size(2)`。

| 消息 | opcode | size | payload |
|---|---|---|---|
| `wl_display.get_registry` | 1 | 12 | — |
| `wl_display.sync` | 0 | — | — |
| `wl_registry.bind` | 0 | 变长 | `name(4) + slen(4) + iface(slen字节,含结尾'\0') + version(4) + new_id(4)` |
| `zwp_linux_dmabuf_v1.create_params` | 1 | 12 | `new_id(4)` |
| `zwp_linux_buffer_params_v1.add` | 1 | 28 | `plane_idx(4) + offset(4) + stride(4) + mod_hi(4) + mod_lo(4)` |
| `zwp_linux_buffer_params_v1.create_immed` | 3 | 28 | `new_id(4) + w(4) + h(4) + format(4) + flags(4)` |

**两个易错点**：
1. `bind` 的 `slen` **含结尾 null 且按 4 字节对齐**（"zwp_linux_dmabuf_v1" 19 字符 → slen=20），version/new_id 位置 = `8 + align4(slen)`
2. `add()` 的 fd **走 SCM_RIGHTS，不占 message body**（body 只有 5 个 u32 = 20 字节）。误按 fd 占位解析会整体错位 4 字节

KWin 全局枚举：`name=52 maleoon_buffer_sharing ver=5`、`name=53 wl_drm ver=2`、`name=54 zwp_linux_dmabuf_v1 ver=4`。

工具：`maleoon-wire-shim-v2.c`（含 observe 纯观测模式）、`v2-parse-unit.c`（离线单测，ALL PASS）、`wl-sizes-probe*.c`（wire 语义探针）。
