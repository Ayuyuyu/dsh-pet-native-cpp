cmake_minimum_required(VERSION 3.20)

# Windows 需要 RC 语言编译图标/版本资源；Linux 不需要。
if(WIN32)
  project(pet-native-cpp LANGUAGES CXX RC)
else()
  project(pet-native-cpp LANGUAGES CXX)
endif()

# 单配置生成器（MinGW Makefiles / Unix Makefiles）默认 Release（-O3 -DNDEBUG）；
# 命令行显式传 -DCMAKE_BUILD_TYPE=... 时仍可覆盖。
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PET_CORE_SOURCES
    core/action_machine.cpp core/clip_pool.cpp core/config.cpp core/decode.cpp
    core/log.cpp core/mask.cpp core/paths.cpp core/physics.cpp core/util.cpp)

# 调试开关：采样导出掩码 BMP（默认关闭；-DPET_DEBUG_MASK_BMP=ON 打开）。
option(PET_DEBUG_MASK_BMP "Export sampled mask BMPs for debugging" OFF)

if(WIN32)
  # ---------- Windows：Win32 + D3D11 + DirectComposition + 本地裁剪版 FFmpeg ----------
  if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-msvc")
    set(FFMPEG_LINK_DIR "${FFMPEG_ROOT}/bin")
    set(FFMPEG_LIBS avcodec.lib avformat.lib avutil.lib swscale.lib)
  else()
    set(FFMPEG_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-ucrt64")
    set(FFMPEG_LINK_DIR "${FFMPEG_ROOT}/lib")
    set(FFMPEG_LIBS avcodec avformat avutil swscale)
  endif()

  # exe「属性 → 详细信息」里的版本元数据，由 app.rc.in 用 @VAR@ 替换。
  set(PET_VERSION "1.0.0.0")
  set(PET_VERSION_COMMA "1,0,0,0")
  set(PET_COMPANY "pet-native-cpp")
  set(PET_FILE_DESC "pet-native-cpp")
  set(PET_PRODUCT "pet-native-cpp")
  set(PET_COPYRIGHT "Copyright (c) 2026 pet-native-cpp")
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/app.rc.in"
    "${CMAKE_CURRENT_BINARY_DIR}/app.rc"
    @ONLY)
  # RC 里用相对路径引用图标，避免生成的 app.rc 中出现机器相关的绝对路径。
  configure_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/assets/pet.ico"
    "${CMAKE_CURRENT_BINARY_DIR}/pet.ico"
    COPYONLY)

  add_executable(pet-native-cpp WIN32
      platform/win/win_main.cpp platform/win/render_d3d11.cpp
      platform/win/win_util.cpp platform/win/win_pet_ops.cpp
      platform/win/win_clip_pool.cpp platform/win/win_ui.cpp
      ${PET_CORE_SOURCES}
      "${CMAKE_CURRENT_BINARY_DIR}/app.rc")

  # 头文件（nlohmann/json 等）在 MSVC 下会触发 C4996，统一关闭该提示。
  target_compile_definitions(pet-native-cpp PRIVATE _CRT_SECURE_NO_WARNINGS
                             UNICODE _UNICODE)

  target_include_directories(pet-native-cpp PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party"
      "${FFMPEG_ROOT}/include")

  if(MSVC)
    target_compile_options(pet-native-cpp PRIVATE /utf-8)
    target_link_libraries(pet-native-cpp PRIVATE
        d3d11.lib dxgi.lib dcomp.lib d3dcompiler.lib dwmapi.lib shell32.lib
        winmm.lib ${FFMPEG_LIBS})
  else()
    target_compile_options(pet-native-cpp PRIVATE
        -finput-charset=UTF-8 -fexec-charset=UTF-8)
    # 剥离符号表，减小分发体积。
    target_link_options(pet-native-cpp PRIVATE -s)
    target_link_libraries(pet-native-cpp PRIVATE
        d3d11 dxgi dcomp d3dcompiler dwmapi shell32 winmm ${FFMPEG_LIBS})
  endif()

  target_link_directories(pet-native-cpp PRIVATE "${FFMPEG_LINK_DIR}")

  # 所有配置（含 Debug）统一输出到 Release/，便于打包脚本按固定路径取产物。
  set_target_properties(pet-native-cpp PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/Release")
  foreach(cfg DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
    set_target_properties(pet-native-cpp PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY_${cfg} "${CMAKE_BINARY_DIR}/Release")
  endforeach()

  if(NOT MSVC)
    # MinGW 还需要把 libstdc++ 放到 exe 目录。
    get_filename_component(UCRT64_BIN_DIR "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(EXISTS "${UCRT64_BIN_DIR}/libstdc++-6.dll")
      add_custom_command(TARGET pet-native-cpp POST_BUILD
          COMMAND ${CMAKE_COMMAND} -E copy_if_different
                  "${UCRT64_BIN_DIR}/libstdc++-6.dll"
                  "$<TARGET_FILE_DIR:pet-native-cpp>/libstdc++-6.dll")
    else()
      message(WARNING "libstdc++-6.dll not found next to ${CMAKE_CXX_COMPILER}")
    endif()
  endif()

  # FFmpeg 运行库两种工具链都需要：MSVC 链接的导入库在 bin/，
  # 对应 DLL 也在同目录，过去把它包在 NOT MSVC 里导致 MSVC 包无法运行。
  file(GLOB PET_FFMPEG_DLLS "${FFMPEG_ROOT}/bin/*.dll")
  list(FILTER PET_FFMPEG_DLLS EXCLUDE REGEX "swresample")
  foreach(dll IN LISTS PET_FFMPEG_DLLS)
    add_custom_command(TARGET pet-native-cpp POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${dll}" "$<TARGET_FILE_DIR:pet-native-cpp>/")
  endforeach()

else()
  # ---------- Linux：X11 + OpenGL + GTK3（右键菜单）+ 发行版 FFmpeg ----------
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(FFMPEG REQUIRED IMPORTED_TARGET
      libavcodec libavformat libavutil libswscale)
  pkg_check_modules(GTK3 REQUIRED IMPORTED_TARGET gtk+-3.0)

  find_package(X11 REQUIRED COMPONENTS Xext)
  find_package(OpenGL REQUIRED)
  find_package(Threads REQUIRED)

  add_executable(pet-native-cpp
      platform/linux/linux_clip_pool.cpp platform/linux/linux_main.cpp
      platform/linux/linux_pet_ops.cpp platform/linux/linux_render.cpp
      platform/linux/linux_state.cpp platform/linux/linux_ui.cpp
      platform/linux/linux_util.cpp ${PET_CORE_SOURCES})

  target_include_directories(pet-native-cpp PRIVATE
      "${CMAKE_CURRENT_SOURCE_DIR}" "${CMAKE_CURRENT_SOURCE_DIR}/third_party")

  # Linux 后端依赖 X11/Xext/GL/pthread/m，全部由 find_package 解析。
  target_link_libraries(pet-native-cpp PRIVATE
      PkgConfig::FFMPEG PkgConfig::GTK3
      X11::X11 X11::Xext OpenGL::GL Threads::Threads m)

  # 产物名保持 pet-linux，打包脚本与文档不用改。
  set_target_properties(pet-native-cpp PROPERTIES OUTPUT_NAME "pet-linux")

  # Release 默认 -O3，额外补上 -Wall 以便静态发现问题。
  target_compile_options(pet-native-cpp PRIVATE -Wall)
endif()

if(PET_DEBUG_MASK_BMP)
  target_compile_definitions(pet-native-cpp PRIVATE PET_DEBUG_MASK_BMP=1)
endif()
