# core / platform 架构说明

> 当前架构：「平台无关 core + 平台后端」。Windows 后端为 Win32 + D3D11/DComp，
> Linux 后端为 X11/GLX + OpenGL。macOS（Cocoa + Metal）尚未实现。

## 1. 目录

```
dsh-pet-native-cpp/
  core/                      平台无关（不 include 任何系统头）
    action_machine.{h,cpp}  动作状态机：朝向/睡觉/切段/失败回滚/移动时间轴
    clip_pool.{h,cpp}       共享动画实例：解码、引用计数、遮挡暂停、事务化缩放
    config.{h,cpp}          pet-config.json 解析、权重轮盘与随机池
    decode.{h,cpp}          DecodeState：FFmpeg 解码线程 + 双缓冲 BGRA
    log.{h,cpp}             日志格式化 + 平台 sink 注入
    mask.{h,cpp}            掩码 / body / 区域矩形（纯 CPU）
    paths.{h,cpp}           资源路径向上查找（窄/宽两个重载）
    physics.{h,cpp}         走路 / 飞行 / 碰撞（纯数学）
    util.{h,cpp}            公共随机、Q 弹曲线与掩码档位
  platform/
    win/                    Win32 + D3D11/DComp 后端（6 个 .cpp + win_pet.h）
    linux/                  X11 + GLX/OpenGL + XShape + GTK3 后端（7 个 .cpp）
  third_party/nlohmann/json.hpp
  CMakeLists.txt            跨平台（Windows / Linux）构建配置
  tools/                    Windows / Linux 打包脚本
```

## 2. 当前平台无关逻辑

| 模块 | 主要接口 | 说明 |
|---|---|---|
| `config` | `LoadConfig` / `PickNextAction` / `IdleSpec` / `SpecForName` | 配置池与加权随机 |
| `action_machine` | `ApplyActionSpec` / `SnapshotAction` / `RestoreAction` / `NextActionAfterEnd` / `ComputeMoveTiming` | 两端共用的动作决策；失败时整体回滚状态 |
| `clip_pool` | `AcquireClip` / `ReleaseClip` / `SetClipVisible` / `UpdateClips` / `ResizeClips` | 共享解码与实例生命周期；平台通过回调注入纹理 |
| `decode` | `Open` / `Close` / `Resize` / `Start` / `Stop` / `TakeFrame` | 双缓冲 + `sleep_until` 绝对时间调度；Resize 事务化 |
| `mask` | `BuildMask` | 逐像素阈值、水平镜像、body 包围盒与区域矩形 |
| `physics` | `StepWalk` / `RectsOverlap` / `StepFly` | 撞边/撞宠物回弹、重力与地面摩擦 |
| `paths` | `ResolveUpwards` | 同目录与向上若干级查找；宽字符版本供 Windows 中文路径使用 |
| `util` | `RandomInt` / `PickRandom` / `SquashCurve` / `MaskSizeFor` | 消除两平台重复实现 |
| `log` | `Init` / `Enabled` / `Log` / `Write` | `std::format` 编译期校验，sink 由平台注入 |

## 3. 平台相关（各自实现）

| 能力 | Windows | Linux |
|---|---|---|
| 窗口 / 消息循环 | `CreateWindowExW` / `GetMessageW` | `XCreateWindow` / `XNextEvent` |
| 透明合成 | DComp visual | ARGB visual + 预乘 alpha（无合成器时退回 XShape 裁剪） |
| 命中 / 穿透 | `WM_NCHITTEST` + `SetWindowRgn` | `XShapeCombineRectangles` |
| 渲染 | D3D11 | GLX / OpenGL |
| 置顶 | `WS_EX_TOPMOST` | `_NET_WM_STATE_ABOVE` |
| 托盘 | `Shell_NotifyIconW` | 未实现 |
| 菜单 | `TrackPopupMenu` | GTK3 POPUP 窗口 |
| 资源路径 | 宽字符全程，UTF-8 仅在 FFmpeg 入口 | 窄字符（UTF-8 环境） |

## 4. 边界与迁移状态

| # | 项 | 状态 |
|---|---|---|
| 1 | `core/config.cpp` | ✅ |
| 2 | `core/mask.cpp` | ✅ |
| 3 | `core/physics.cpp` | ✅ |
| 4 | `core/decode.cpp` | ✅ |
| 5 | `core/util.cpp` | ✅ |
| 6 | `core/action_machine.cpp` | ✅ |
| 7 | `core/clip_pool.cpp` + Win 适配 | ✅ |
| 8 | Linux 共享池帧管线切换 | ⏳ 适配层已就位，当前仍按 Pet 各自解码 |
| 9 | macOS（Cocoa + Metal） | ⏳ 需真机 |

**原则**：core 不 include 任何平台头；平台层只负责窗口、渲染、输入与资源创建。
新增 `core/*.cpp` 必须同步加入 `CMakeLists.txt` 的 `PET_CORE_SOURCES`。

## 5. 风险

| 风险 | 说明 |
|---|---|
| 拆文件期间功能退化 | 每阶段保持 Windows 可编译，按 `tools/package.ps1` 验证 |
| X11 透明在虚拟机 | 需要 `+extension Composite`；软件 GL 可能不完整 |
| Wayland | 不支持，仅 X11 / Xwayland |
| 托盘 | Linux 未实现 |
| 多显示器 | Linux 工作区目前取 `_NET_WORKAREA` 第一组，极端布局需真机校准 |
