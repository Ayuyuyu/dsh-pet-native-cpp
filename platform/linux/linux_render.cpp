#include "platform/linux/linux_render.h"

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>

#include <cstdio>
#include <string_view>

#include "core/log.h"
#include "platform/linux/linux_util.h"

#ifndef GL_BGRA
#define GL_BGRA 0x80E1
#endif

using namespace core::log;  // Log(...) 与 Win 端保持同形

namespace linuxrender {

namespace {

// GL 的 NDC 里 y=1 是顶部；压扁时底部固定（y=-1 不动），顶部从 1 下压到 2*scaleY-1。
constexpr std::string_view kVS = R"(#version 120
attribute vec2 aPos;
uniform float uScaleY;
uniform float uFlip;
varying vec2 vUv;
void main() {
  vUv = vec2(aPos.x * 0.5 + 0.5, 0.5 - aPos.y * 0.5);
  // 镜像动作只翻纹理坐标，几何保持不动。
  if (uFlip > 0.5) vUv.x = 1.0 - vUv.x;
  vec2 p = aPos;
  // 底部固定、顶部往下压（和 Windows 端一致）。
  p.y = p.y * uScaleY - (1.0 - uScaleY);
  gl_Position = vec4(p, 0.0, 1.0);
}
)";

constexpr std::string_view kFS = R"(#version 120
uniform sampler2D uTex;
uniform float uDebugMask;
varying vec2 vUv;
void main() {
  vec4 c = texture2D(uTex, vUv);
  vec4 outColor = vec4(c.rgb * c.a, c.a);
  if (uDebugMask > 0.5) {
    outColor = mix(outColor, vec4(1.0, 0.0, 0.0, 1.0), 0.35 * c.a);
  }
  gl_FragColor = outColor;
}
)";

}  // namespace

GLuint CompileShader(GLenum type, std::string_view src) {
  const GLuint sh = glCreateShader(type);
  const GLchar* src_ptr = src.data();
  const GLint src_len = static_cast<GLint>(src.size());
  glShaderSource(sh, 1, &src_ptr, &src_len);
  glCompileShader(sh);
  GLint ok = 0;
  glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
  if (ok == 0) {
    char buf[1024]{};
    glGetShaderInfoLog(sh, sizeof(buf), nullptr, buf);
    Log("shader compile failed: {}", buf);
    glDeleteShader(sh);
    return 0;
  }
  return sh;
}

GLuint BuildProgram() {
  const GLuint vs = CompileShader(GL_VERTEX_SHADER, kVS);
  const GLuint fs = CompileShader(GL_FRAGMENT_SHADER, kFS);
  if (vs == 0 || fs == 0) {
    if (vs != 0) glDeleteShader(vs);
    if (fs != 0) glDeleteShader(fs);
    return 0;
  }
  const GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  // attach 后 program 持有引用，可立即释放本侧的 shader 对象。
  glDeleteShader(vs);
  glDeleteShader(fs);
  glBindAttribLocation(prog, 0, "aPos");
  glLinkProgram(prog);
  GLint ok = 0;
  glGetProgramiv(prog, GL_LINK_STATUS, &ok);
  if (ok == 0) {
    char buf[1024]{};
    glGetProgramInfoLog(prog, sizeof(buf), nullptr, buf);
    Log("program link failed: {}", buf);
    glDeleteProgram(prog);
    return 0;
  }
  return prog;
}

// 有合成器：32 位 ARGB 窗口，靠 alpha 混合透明。
// 无合成器：24 位窗口 + ShapeBounding 裁剪（硬边，但能显示）。
bool InitVisual() {
  static XVisualInfo info{};
  // 合成器 selection 名随 screen 变化：多显示器环境下不能写死 S0。
  char cm_name[32] = {};
  std::snprintf(cm_name, sizeof(cm_name), "_NET_WM_CM_S%d", g_screen);
  const Atom cm = XInternAtom(g_dpy, cm_name, False);
  g_hasCompositor = (cm != None) && (XGetSelectionOwner(g_dpy, cm) != None);
  // _NET_SUPPORTING_WM_CHECK 是 root 上的 window 属性，不是 selection。
  const Atom wm = XInternAtom(g_dpy, "_NET_SUPPORTING_WM_CHECK", False);
  Atom type = None;
  int format = 0;
  unsigned long n = 0;
  unsigned long after = 0;
  unsigned char* raw = nullptr;
  const Status st = XGetWindowProperty(g_dpy, g_root, wm, 0, 1, False,
                                       XA_WINDOW, &type, &format, &n, &after,
                                       &raw);
  linuxutil::XPropertyPtr data(raw);
  if (st == Success && data != nullptr) g_hasWm = true;
  Log("compositor: {} wm: {}", g_hasCompositor ? 1 : 0, g_hasWm ? 1 : 0);

  if (g_hasCompositor &&
      XMatchVisualInfo(g_dpy, g_screen, 32, TrueColor, &info)) {
    // info 是 static 局部变量，不归 X 所有：退出时绝不能 XFree。
    g_visual = &info;
    g_visualOwned = false;
    g_cmap = XCreateColormap(g_dpy, g_root, g_visual->visual, AllocNone);
    Log("visual depth=32 (ARGB)");
    return true;
  }

  int attribs[] = {GLX_RGBA,        GLX_DOUBLEBUFFER, GLX_RED_SIZE,   8,
                   GLX_GREEN_SIZE,  8,                GLX_BLUE_SIZE,  8,
                   None};
  g_visual = glXChooseVisual(g_dpy, g_screen, attribs);
  if (g_visual == nullptr) {
    Log("glXChooseVisual failed");
    return false;
  }
  g_visualOwned = true;  // 这条路径是 X 分配的，退出时要 XFree
  g_cmap = XCreateColormap(g_dpy, g_root, g_visual->visual, AllocNone);
  Log("visual depth={} (no compositor, shape clip)", g_visual->depth);
  return true;
}

void InitTexture(Pet& pet) {
  // texture 是 per-context 对象，上传前必须在自己的 GLX context 上。
  if (glXMakeCurrent(g_dpy, pet.win, pet.ctx) == False) {
    Log("pet{} make current failed in InitTexture", pet.index);
    return;
  }
  if (pet.tex != 0 && pet.texW == pet.dec.videoW &&
      pet.texH == pet.dec.videoH) {
    return;  // 尺寸没变：纹理可复用，切段不再重建 GPU 资源
  }
  if (pet.tex != 0) {
    glDeleteTextures(1, &pet.tex);
    pet.tex = 0;
  }
  glGenTextures(1, &pet.tex);
  glBindTexture(GL_TEXTURE_2D, pet.tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, pet.dec.videoW, pet.dec.videoH, 0,
               GL_BGRA, GL_UNSIGNED_BYTE, nullptr);
  pet.texW = pet.dec.videoW;
  pet.texH = pet.dec.videoH;
}

void UploadVideoFrame(Pet& pet) {
  if (pet.tex == 0 || pet.dec.bgra.empty()) return;
  // 上一只宠物的 Render 留下的 context 未必是这一只的；显式切换，
  // 避免关闭宠物解绑 context 后紧接的一帧上传被丢弃。
  if (glXMakeCurrent(g_dpy, pet.win, pet.ctx) == False) {
    Log("pet{} make current failed in UploadVideoFrame", pet.index);
    return;
  }
  if (pet.dec.videoW != pet.texW || pet.dec.videoH != pet.texH) {
    InitTexture(pet);
    if (pet.tex == 0) return;
  }
  glBindTexture(GL_TEXTURE_2D, pet.tex);
  glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, pet.dec.videoW, pet.dec.videoH,
                  GL_BGRA, GL_UNSIGNED_BYTE, pet.dec.bgra.data());
}

void Render(Pet& pet) {
  glXMakeCurrent(g_dpy, pet.win, pet.ctx);
  // 视口按内容补偿量平移：窗口被 WM 挡在工作区内时，身体仍能贴到屏幕边缘。
  // GL 视口允许超出 drawable，超出的部分会被裁剪。
  glViewport(pet.drawOffX, -pet.drawOffY, pet.windowW, pet.windowH);
  glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
  glClear(GL_COLOR_BUFFER_BIT);
  glEnable(GL_BLEND);
  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
  glUseProgram(g_prog);
  glUniform1f(glGetUniformLocation(g_prog, "uScaleY"), pet.scaleY);
  glUniform1f(glGetUniformLocation(g_prog, "uFlip"), pet.flip ? 1.0f : 0.0f);
  glUniform1f(glGetUniformLocation(g_prog, "uDebugMask"),
              g_debug ? 1.0f : 0.0f);
  glUniform1i(glGetUniformLocation(g_prog, "uTex"), 0);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, pet.tex);
  glEnableVertexAttribArray(0);
  const GLfloat verts[8] = {-1.0f, -1.0f, 1.0f, -1.0f,
                            -1.0f, 1.0f,  1.0f, 1.0f};
  glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, verts);
  glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
  glDisableVertexAttribArray(0);
  // 调试时自检一次渲染结果（GL 内容在 GPU，XGetImage 读不到，只能 glReadPixels）。
  static bool probed = false;
  if (g_debug && !probed) {
    probed = true;
    const size_t pixels = static_cast<size_t>(pet.windowW) * pet.windowH;
    std::vector<unsigned char> buf(pixels * 4);
    glReadPixels(0, 0, pet.windowW, pet.windowH, GL_RGBA, GL_UNSIGNED_BYTE,
                 buf.data());
    size_t opaque = 0;
    size_t colored = 0;
    for (size_t i = 0; i < pixels; ++i) {
      if (buf[i * 4 + 3] <= 8) continue;
      ++opaque;
      if ((buf[i * 4] | buf[i * 4 + 1] | buf[i * 4 + 2]) != 0) ++colored;
    }
    Log("readback opaque={}/{} colored={} center={},{},{},{} (gl err {})",
        opaque, pixels, colored, buf[(pet.windowH / 2 * pet.windowW +
                                      pet.windowW / 2) * 4 + 0],
        buf[(pet.windowH / 2 * pet.windowW + pet.windowW / 2) * 4 + 1],
        buf[(pet.windowH / 2 * pet.windowW + pet.windowW / 2) * 4 + 2],
        buf[(pet.windowH / 2 * pet.windowW + pet.windowW / 2) * 4 + 3],
        glGetError());
  }
  glXSwapBuffers(g_dpy, pet.win);
  XFlush(g_dpy);
}

}  // namespace linuxrender
