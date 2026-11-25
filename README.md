# dsh-pet-native-cpp

原生 C++ 实现的 Windows 桌面宠物，用于替代内存开销较大的 WebView2 方案。另提供 Linux/X11 版本。

宠物基于透明、可穿透的窗口显示逐帧 webm 动画，支持多只宠物、点击/拖拽交互、右键菜单、加权随机动作链、走路/飞行/碰撞物理和遮挡暂停。

## 低开销
Windows 场景内存较原方案降低约 90%，新增一只宠物约增加 8 MB。

## 素材来源

**本项目使用的动画/音效/配置素材来自 [PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)**，在此向原作者致谢。本项目仅将这些素材用于原生 C++ 重写版的运行与分发，动画版权归原作者所有。

- 114 个 VP9-alpha `.webm` 动画：`assets/clips/`
- 点击音效：`assets/sound/`
- 动作配置：`assets/pet-config.json`
- 图标：`assets/pet.ico`  素材来自[fisharchive](https://fisharchive.cc/)

## 特性

- **透明与穿透**：`WS_EX_NOREDIRECTIONBITMAP` + DirectComposition，逐帧按 alpha 掩码裁剪窗口区域；非可点区域鼠标穿透。
- **共享解码池**：相同动画的多只宠物共享解码线程、纹理与掩码；`move`/`turn` 动作独占实例；完全被遮挡时自动停解码与绘制（以上为 Windows 后端行为；Linux 后端提供共享池适配层，当前帧管线仍按宠物各自解码，详见 `platform/linux/README.md`）。
- **动作状态机**：`idle / turn / move / category / manual / click / drag / sleep`，按权重轮盘抽取下一段；`turn` 会真实翻转全局朝向，文字类动作在镜像态回退以避免反字。
- **物理交互**：走路按 `lead/move/tail` 时间轴随动画平移，撞边/撞宠物回弹；拖拽松手可甩出并受重力与地面摩擦影响。
- **多宠物**：默认 1 只，可通过参数或菜单创建，上限 64 只；宠物之间会碰撞。

## 目录结构

```
core/                  平台无关逻辑（配置、解码、掩码、物理）
platform/win/          Win32 / D3D11 / DirectComposition 后端
platform/linux/        X11 / OpenGL / XShape 后端
third_party/           第三方依赖（nlohmann/json 头文件 + 本地构建的 FFmpeg，见下）
assets/                webm 动画、音效、pet-config.json、图标
tools/                 Windows / Linux 打包脚本
CMakeLists.txt         跨平台构建配置（Windows / Linux）
```

## 构建（Windows）

默认使用 MSYS2 **UCRT64** 工具链，需要 CMake 3.20+、MSYS2 ucrt64，以及本地构建好的裁剪版 FFmpeg。

### FFmpeg 依赖

项目只用到 FFmpeg 的 Matroska/WebM 解封装、VP9 解码和 swscale；`libvpx-vp9` 解码器是必需的（FFmpeg 内置 `vp9` 解码器会丢失 webm 的 alpha 通道）。注意 FFmpeg configure 的选项名是 `libvpx_vp9`（下划线），而运行时 `avcodec_find_decoder_by_name` 用 `libvpx-vp9`（连字符），两者指同一个解码器。`third_party/ffmpeg-msvc/` 和 `third_party/ffmpeg-ucrt64/` 是这两个工具链对应的构建产物，克隆后需自行生成。

以下命令在 **MSYS2 UCRT64 shell** 中执行。先设定两个变量，之后原样复制即可：

```bash
cd <项目根目录>          # 例如 cd dsh-pet-native-cpp
PROJECT=$(pwd)
SRC=$HOME/deps-src       # 源码与临时构建的存放位置，可自行修改
mkdir -p "$SRC"
```

1. **下载源码**（FFmpeg 9.0 与 libvpx 1.15.0）：

   ```bash
   git clone --depth 1 --branch n9.0 https://git.ffmpeg.org/ffmpeg.git "$SRC/FFmpeg-n9.0"
   git clone --depth 1 --branch v1.15.0 https://github.com/webmproject/libvpx.git "$SRC/libvpx-1.15.0"
   ```

2. **构建 libvpx**：

   ```bash
   cd "$SRC/libvpx-1.15.0"
   ./configure --target=generic-gnu --disable-examples --disable-tools --disable-docs \
     --disable-unit-tests --disable-webm-io --disable-libyuv \
     --prefix="$SRC/libvpx-build"
   make -j4 && make install
   ```

   > 注意：libvpx **不认** `--disable-asm`，要用 `--target=generic-gnu`。

3. **生成 FFmpeg 配置所需的 pkg-config 包装脚本**（后续步骤会用到）：

   ```bash
   printf '#!/bin/sh\nexport PKG_CONFIG_PATH=%s/libvpx-build/lib/pkgconfig:$PKG_CONFIG_PATH\nexec pkg-config "$@"\n' "$SRC" > "$SRC/pkg-config"
   chmod +x "$SRC/pkg-config"
   ```

4. **构建 UCRT64 版 FFmpeg**：

   ```bash
   cd "$SRC/FFmpeg-n9.0"
   ./configure --prefix="$PROJECT/third_party/ffmpeg-ucrt64" \
     --arch=x86_64 --target-os=mingw32 --cc=gcc --cxx=g++ \
     --pkg-config="$SRC/pkg-config" \
     --disable-everything --disable-network --disable-avdevice --disable-avfilter \
     --disable-programs --disable-doc --enable-decoder=libvpx_vp9 \
     --enable-demuxer=matroska --enable-parser=vp9 --enable-protocol=file \
     --enable-swscale --enable-libvpx --disable-x86asm \
     --enable-shared --disable-static
   make -j4 && make install
   ```

   产物直接安装到 `third_party/ffmpeg-ucrt64/`（`bin/include/lib/share` 四目录）。

5. **MSVC 版**（`third_party/ffmpeg-msvc/`）使用 MSVC 工具链按同样选项构建一份；若暂不需要 MSVC 构建，可跳过。


### 编译

首次编译前把 ucrt64 的 `bin` 加入 `PATH`（一次性加入系统环境变量即可）：

```powershell
$env:PATH = '<MSYS2 安装目录>\ucrt64\bin;' + $env:PATH
cd dsh-pet-native-cpp
cmake -S . -B build -G "MinGW Makefiles" -DCMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES=OFF
cmake --build build
```

之后增量编译只需：

```powershell
cd dsh-pet-native-cpp
cmake --build build
```

`CMakeLists.txt` 在未指定构建类型时默认 `Release`（`-O3 -DNDEBUG`），单配置生成器可用 `-DCMAKE_BUILD_TYPE=Debug` 覆盖；MSVC 多配置生成器请用 `cmake --build build-msvc --config Debug`。`-DCMAKE_CXX_USE_RESPONSE_FILE_FOR_INCLUDES=OFF` 用于规避部分 MinGW 环境对 include response file 的解析问题。

也可使用 MSVC 工具链（输出到独立的 `build-msvc`）：

```powershell
cmake -S . -B build-msvc -G "Visual Studio 17 2022" -A x64
cmake --build build-msvc --config Release
```

一键打包（生成 `dist/native-win-x64/` 与 zip，素材与运行库一并复制）：

```powershell
powershell -NoProfile -ExecutionPolicy Bypass -File tools\package.ps1
# MSVC 版：追加 -Toolchain MSVC
```

## 运行

直接运行 `build/Release/pet-native-cpp.exe`。资源路径按 exe 目录向上查找 `assets/`，不依赖当前工作目录；打包版把 `pet-config.json` 放在 exe 同目录、webm 放在 `assets/clips/`。

常用参数：

| 参数 | 作用 |
| --- | --- |
| `--pets=N` | 启动时创建 N 只宠物（钳制到 1~64） |
| `--debug` | 打开日志与掩码叠加调试（BMP 导出需编译期开关 `PET_DEBUG_MASK_BMP`） |
| `--clip=名字` | 指定启动动作，名字含空格可用引号包裹 |
| `--probe <path>` | 仅解码指定 webm 的首帧并输出诊断信息后退出 |

交互：

- 左键点击宠物：按权重触发点击回应，带 Q 弹与音效；
- 按住拖动：跟随鼠标，松手速度足够大时甩出；
- 右键菜单：切换动作、睡觉、再来一只、调整大小、退出；
- 托盘图标：显示/隐藏宠物，或退出（仅 Windows；Linux 暂未实现）。

## Linux

Linux 后端位于 `platform/linux/`，使用 X11（ARGB 视觉）+ OpenGL + XShape，GTK3 绘制右键菜单。依赖 Arch 包：`gcc cmake make pkg-config libx11 libxext mesa gtk3 ffmpeg`。

用 CMake 构建（与 Windows 同一份 `CMakeLists.txt`）：

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --parallel            # 产物 ./build/pet-linux
```

打包：`tools/package.sh` 会先用 cmake 编译，再生成 `dist/native-linux-x64/` 与 tar.gz。

透明/穿透、窗口坐标补偿与菜单实现等细节见 `platform/linux/README.md`。

## 配置

动画与行为由 `assets/pet-config.json` 驱动，核心字段：

- `byName`：动作名到 `clip-xxx.webm` 的映射；
- `weights` / `clickWeights`：随机链与点击回应的权重；
- `sizePercent` / `idleAfterAction`：初始大小与动作后是否回待机；
- `pools`：`idle`、`turn`、`drag`、`clicks`、`moves`、`categories`、`events`、`sleep` 动作池；
- `moves[]`：`dirX`（必填）、`dirY`（可选，默认 0）、`minDist`、`maxDist`、`leadSec`、`tailSec`、`flip`；素材里残留的 `margin` 字段当前未被代码读取，可忽略。
`flip` 决定移动动画是否水平镜像；走路速度与移动窗口由动画时长、`leadSec` 和 `tailSec` 计算，走完配置距离即停。


## 致谢

- 动画、音效与配置素材：[PC2005-cloud/dsh-pet](https://github.com/PC2005-cloud/dsh-pet)
- [nlohmann/json](https://github.com/nlohmann/json)
- [FFmpeg](https://ffmpeg.org/) / [libvpx](https://github.com/webmproject/libvpx)
