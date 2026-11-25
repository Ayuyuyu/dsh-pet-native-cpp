# Linux 后端（X11 + OpenGL）

复用 `core/`（config / decode / mask / physics），平台层只负责窗口、渲染、输入与形状裁剪。

## 依赖（Arch）

```bash
sudo pacman -S --needed gcc cmake make pkg-config libx11 libxext mesa ffmpeg gtk3
```

已验证：支持 C++20 的 g++、`X11`、`GLX`（llvmpipe 软件渲染也能跑）、`libvpx_vp9`（FFmpeg configure 选项名，解码器名为 `libvpx-vp9`）、`gtk3`（右键菜单）。

## 构建

```bash
cd dsh-pet-native-cpp
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel       # 产物 ./build/pet-linux
```

与 Windows 共用同一份 `CMakeLists.txt`，通过 `pkg-config` 找 FFmpeg/GTK3，链接 `X11/Xext/GL/pthread`。`tools/package.sh` 内部也走这套 CMake 构建（可用 `--no-build` 跳过构建只打包）。

## 运行

```bash
export DISPLAY=:0
./build/pet-linux                  # 单只，完全静默
./build/pet-linux --pets=3         # 三只
./build/pet-linux --debug          # 红色叠加可点击区域 + 写 exe 目录下的 pet-linux.log
./build/pet-linux --menu-test      # 启动 2 秒后自动弹一次右键菜单（无鼠标环境验证）
```

正常模式不写日志、不往终端输出；`--debug` 才写 exe 目录下的 `pet-linux.log`。

配置与素材按 exe 目录解析（同目录或向上若干级的 `assets/`），不依赖当前工作目录。

## 透明与输入穿透

| 环境 | 处理 |
|---|---|
| **有合成器**（KDE/GNOME + compositing） | 32 位 ARGB visual + 预乘 alpha，`ShapeInput` 只做鼠标穿透，边缘平滑 |
| **无合成器** | 24 位 visual + `ShapeBounding` 按掩码裁剪（硬边），`ShapeInput` 同步 |

合成器用 `_NET_WM_CM_S0` 的 selection owner 判断，窗口管理器用 `_NET_SUPPORTING_WM_CHECK` 判断。

**有 WM 时**必须是**受管理的窗口**（不能 `override_redirect`），否则 KWin 不会合成 ARGB 内容、窗口一片空白。为此设置了：

- `_NET_WM_WINDOW_TYPE_UTILITY`（无任务栏、无装饰）
- `_NET_WM_STATE_ABOVE` + `SKIP_TASKBAR` + `SKIP_PAGER`
- `_MOTIF_WM_HINTS` decorations=0

## 窗口坐标与屏幕边界

X11 的窗口坐标有两套：**相对父窗口**（`XGetWindowAttributes` 的 `x/y`）和 **root 绝对坐标**。
有 WM 时窗口会被 reparent 到 frame、被 placement 策略重新摆放，WM 还会异步处理
`ConfigureRequest`，所以每帧读回的 `x/y` 既不是屏幕坐标、也可能滞后一帧。
拿它和屏幕边界做物理运算会导致：甩出后速度不衰减、`vy` 一路累加、原地无限弹跳，
宠物也永远到不了屏幕边缘。

因此窗口位置由 `Pet::posX/posY`（root 绝对坐标）自己维护：

- 创建时 `XSync` + `XTranslateCoordinates` 读取真实位置；WM 可能还在摆放，
  启动后前 1.5 秒在程序尚未主动移动窗口前再校准几次。
- 拖动 / 飞行 / 走路都基于 `posX/posY` 计算，只用 `XMoveWindow` 写，不再读回。
- 边界取 `_NET_WORKAREA`（去掉 KDE/GNOME 面板），和 Win32 的 `rcWork` 对齐。
- 撞宠物碰撞框也用 `posX/posY + body`，不再逐只查窗口属性。

**KWin 会把受管理窗口完整约束在工作区内**，窗口不能像 Win32 那样部分移出屏幕，
而宠物的身体框四周本来就有透明边距，直接贴边会差一个身位。
`MovePetTo` 把「期望位置」和「WM 允许的位置」之差记成 `drawOffX/drawOffY`：

- `glViewport(offX, -offY, w, h)` 让画面在窗口内补偿平移（GL 视口允许超出 drawable）；
- `ShapeInput` 矩形同步平移并按窗口裁剪；无合成器时 `ShapeBounding` 也同步；
- 补偿量被限制在身体框外的透明边距内（等价于物理边界），拖出屏幕时宠物不会消失。

`--debug` 日志里 `pos=` 是逻辑位置、`off=` 是补偿量、`real=` 是
`XTranslateCoordinates` 回读的真实窗口位置（WM 异步，可能滞后一帧）。

## 交互

- 左键拖动，松手速度 > 80 px/s 甩出（重力 1400、反弹 0.78、地面摩擦 0.85）；
  贴地且速度都 < 80 px/s 时提前收尾，另有 12 秒兜底，不会无限弹跳
- 点击：随机点击回应 + Q 弹（220 ms）
- **右键菜单**（GTK3 自绘窗口）：再来一只 / 关闭这一只 / 放大·缩小 5% / 动作列表（滚动，按分类）/ 退出
- 区域外鼠标穿透到桌面

菜单**没有用 `GtkMenu`**：我们的窗口是原生 Xlib 创建的，GTK 不知道它的窗口树，
`GtkMenu` 能显示但 grab 建立不起来（点击菜单项无效）。改成 `GtkWindow(POPUP)` + `GtkButton`
列表后，按钮点击是标准 GTK 事件，正常工作；点击菜单外部由 50ms 轮询检测指针位置关闭。
GTK/GLib 的提示走 `g_log_set_writer_func` 收进调试日志，不污染 stderr。

**菜单跟随鼠标**：启动时 `setenv("GDK_BACKEND", "x11", 1)` 强制 GTK 走 X11 后端。
在 Wayland 会话里 GTK 默认选 wayland，`GtkWindow(POPUP)` 没有父 surface，KWin 会把它当
普通窗口加标题栏并居中摆放，`gtk_window_move` 也直接被 GDK 拒绝（日志里的
`temporary window without parent`）；`gdk_x11_window_get_xid` 同样会断言失败。
`ShowMenu` 先 `gtk_widget_realize` 取到 XID、把窗口设成 `override_redirect`，
位置统一交给 `PlaceMenu()`：菜单放在**鼠标右上方**（左下角贴 `x_root/y_root`），
在屏幕底部右键时向上展开；上方放不下就退回鼠标下方，超出右/下边界按
`_NET_WORKAREA` 往内钳制，对齐 Win32 `TrackPopupMenu` 的自动翻转。
分页后内容会变高（动作列表带 360px 滚动区），三个 `BuildXxxPage()` 末尾都会
再调一次 `PlaceMenu()` 重新定位，切到动作列表也不会从底部溢出。
菜单是 OR 窗口，KWin 不再接管，也不会再出现标题栏。

**菜单收起**：OR 窗口拿不到 WM 的焦点通知，GTK 事件只能覆盖部分场景，
不能单靠 `gtk_grab_add` + `button-press-event` 收起菜单。
`ShowMenu` 记下打开时的 `XGetInputFocus` 快照和菜单矩形，主循环每 50ms 跑一次
`CheckMenuDismiss()`：焦点转到别的窗口，或在菜单外检测到按键，就销毁菜单
（对齐 Win32 `TrackPopupMenu` 的行为）。按键检测带 250ms 宽限和 4px 边界容差，
避免右键 `press→release` 期间把刚弹出的菜单误关。

**多只与关闭**：`关闭这一只` 只置 `closing` 标记，主循环统一 `Stop`/`Close`/销毁/`erase`（`unique_ptr` 自动析构），
避免在 GTK 回调里 join 解码线程。退出前先 `linuxui::CloseMenu()`，防止菜单里的借用 `Pet*` 悬空。

## 视频时钟与移动动作（对齐 Win32）

`core/decode.cpp` 的解码线程按 `sleep_until` 绝对时间调度，帧间隔稳定在
1000/fps（约 30fps），而主循环 Tick 固定 16ms（约 62.5Hz）。两者不同步，
所以状态机**不能**拿 Tick 计数当视频时钟：那样 30fps 的动作会被当成
62.5fps，片段只播一半就切换、转身段缩短一半、移动速度翻倍。与 Win32 端一致：

- 只有 `TakeFrame` 成功才 `++playedFrames`；`clipFrames = dec.clipFrames - 1`，
  `playDurationMs = clipFrames * 1000 / fps`；
- `frames >= clipFrames` 才切下一段，另有
  `playDurationMs * 2 + 2000` 墙钟兜底，防解码卡死；
- 移动：`leadFrames = leadSec * fps`、`moveFrames = clipFrames - lead - tail`、
  `moveSpeedPxPerFrame = dist * 1.2 / moveFrames`；按 `dFrames`（呈现帧增量）
  推进，转身段原地、走完 `dist` 立刻停、tail 段静止；
- 撞边/撞宠物：位移方向反向一次（视频镜像/朝向不变），剩余距离 `×0.6`，太小就停；
  `moveDirY` 也会反向，拖动时立刻 `moveActive = false`。

`frameCount` 是 16ms Tick 计数，仅在启动后前 1.5 秒用于一次性的窗口位置校准；
切段时归零不会让校准反复触发。

## 已知限制

- 共享解码池：`core/clip_pool.cpp` 与 `linux_clip_pool.cpp` 已提供完整的实例管理/GL 纹理适配层，但当前帧管线仍按每只宠物各自的 `DecodeState` 解码；同动作多只宠物不会共享解码与相位。因该后端按约定不上机验证，未做高风险切换。
- 遮挡暂停：Windows 后端实现，Linux 后端暂未实现。
- 托盘未做（Linux 需要 libappindicator/GTK，暂缺）
- 无合成器时边缘是硬边；Windows 端同时用 `WM_NCHITTEST`/`HTTRANSPARENT` 做同链穿透、用 `SetWindowRgn` 做跨进程穿透
- Wayland 未支持，仅 X11
- `x11grab` 抓不到合成后的画面，验证要用 `spectacle` 之类走 Composite 的工具
