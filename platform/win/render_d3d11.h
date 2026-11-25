#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <d3d11.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <cstdint>

// Windows 渲染层：进程内共享一份设备/shader。
// 资源按生命周期分两类：
//   WinPetSurface  —— 每只宠物一份（交换链 / DComp visual / 常量缓冲）；
//   WinClipTexture —— 每个动画实例一份（视频纹理 + 掩码纹理），
//                     同一 clip 的多只宠物共用，省掉 N 份纹理与 N 次上传。
struct WinPetSurface {
  HWND hwnd = nullptr;  // 关联的宠物窗口，重建 DComp target 时使用
  Microsoft::WRL::ComPtr<IDXGISwapChain1> swap;
  Microsoft::WRL::ComPtr<ID3D11RenderTargetView> rtv;
  Microsoft::WRL::ComPtr<IDCompositionTarget> target;
  Microsoft::WRL::ComPtr<IDCompositionVisual> visual;
  Microsoft::WRL::ComPtr<ID3D11Buffer> params;
};

struct WinClipTexture {
  Microsoft::WRL::ComPtr<ID3D11Texture2D> tex;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> srv;
  Microsoft::WRL::ComPtr<ID3D11Texture2D> maskTex;
  Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> maskSrv;
  int videoW = 0;  // tex 的实际尺寸，上传前校验，避免缩放失败后越界写
  int videoH = 0;
  int maskW = 0;  // maskTex 的实际尺寸，用于上传前校验
  int maskH = 0;
};

// 创建 D3D11 device / sampler / DComp device 并编译 shader（进程内一次）。
bool RenderInitShared();

// 为窗口创建交换链、DComp visual 与常量缓冲。
bool RenderCreateTarget(WinPetSurface& surface, HWND hwnd, int w, int h);

// 按视频与掩码尺寸创建（或重建）一份动画纹理。
bool RenderCreateClipTextures(WinClipTexture& tex, int videoW, int videoH,
                              int maskW, int maskH);

// 上传一帧 BGRA（主线程）。
bool RenderUploadVideo(WinClipTexture& tex, const uint8_t* bgra, int w, int h);

// 上传掩码（--debug 红色叠加用）。
void RenderUploadMask(WinClipTexture& tex, const char* mask, int mw, int mh);

// 渲染一帧并 Present；失败返回 false（设备丢失等），调用方应保留 needPresent。
bool RenderFrame(WinPetSurface& surface, const WinClipTexture& tex, int w,
                 int h, float scaleY, bool flip, bool debugMask);

// 缩放：只重建交换链与 RTV；动画纹理由 winclips::ResizeAll 立即重建。
bool RenderResize(WinPetSurface& surface, int w, int h);
