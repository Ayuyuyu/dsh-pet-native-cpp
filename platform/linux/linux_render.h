#pragma once

// Linux 渲染层：GLX + OpenGL（预乘 alpha）。
// 只负责 GPU 资源与绘制原语；绘制所需的几何补偿参数由 Pet 传入，
// 不参与动作状态机（对应 render_d3d11.*）。
#include <string_view>

#include "platform/linux/linux_state.h"

namespace linuxrender {

GLuint CompileShader(GLenum type, std::string_view src);
GLuint BuildProgram();

// 选 visual：有合成器用 32 位 ARGB，否则退回 GLX + ShapeBounding 裁剪。
bool InitVisual();

// 按视频尺寸（重新）创建纹理。
void InitTexture(Pet& pet);

// 上传一帧 BGRA 到纹理。
void UploadVideoFrame(Pet& pet);

// 绘制 + glXSwapBuffers。
void Render(Pet& pet);

}  // namespace linuxrender
