// Windows 渲染后端：D3D11 + DirectComposition（预乘 alpha 透明）。
#include "render_d3d11.h"

#include <d3dcompiler.h>

#include <cstring>
#include <string_view>
#include <utility>

#include "core/log.h"

using Microsoft::WRL::ComPtr;

namespace {

ComPtr<ID3D11Device> g_device;
ComPtr<ID3D11DeviceContext> g_context;
ComPtr<ID3D11SamplerState> g_sampler;
ComPtr<ID3D11VertexShader> g_vs;
ComPtr<ID3D11PixelShader> g_ps;
ComPtr<IDCompositionDevice> g_dcomp;

constexpr std::string_view kShader = R"(
cbuffer Params : register(b0) {
  float scaleY;
  float debugMask;
  float flipX;
  float pad;
};
struct VSOut { float4 pos : SV_POSITION; float2 uv : TEXCOORD0; };
VSOut VSMain(uint id : SV_VertexID) {
  float2 uv = float2((id << 1) & 2, id & 2);
  VSOut o;
  o.pos = float4(uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
  // 底部固定、顶部往下压（y=-1 不动，y=1 压到 2s-1）。
  o.pos.y = o.pos.y * scaleY - (1.0 - scaleY);
  // 只翻纹理坐标，顶点几何保持不动。
  o.uv = float2(flipX > 0.5 ? 1.0 - uv.x : uv.x, uv.y);
  return o;
}
Texture2D tex : register(t0);
Texture2D maskTex : register(t1);
SamplerState smp : register(s0);
float4 PSMain(VSOut i) : SV_TARGET {
  float4 c = tex.Sample(smp, i.uv);
  float4 outColor = float4(c.rgb * c.a, c.a);
  if (debugMask > 0.5) {
    float m = maskTex.Sample(smp, i.uv).r;
    outColor = lerp(outColor, float4(1.0, 0.0, 0.0, 1.0), 0.35 * m);
  }
  return outColor;
}
)";

bool CompileShaders() {
  ComPtr<ID3DBlob> vs_blob;
  ComPtr<ID3DBlob> ps_blob;
  ComPtr<ID3DBlob> err;
  const auto log_err = [&err](const char* stage) {
    if (err == nullptr) return;
    const auto* text = static_cast<const char*>(err->GetBufferPointer());
    const size_t n = err->GetBufferSize();
    core::log::Log("shader {} failed: {}", stage,
                   std::string_view(text, n > 0 ? n - 1 : 0));
  };
  if (FAILED(D3DCompile(kShader.data(), kShader.size(), nullptr, nullptr, nullptr,
                        "VSMain", "vs_5_0", 0, 0, &vs_blob, &err))) {
    log_err("VSMain");
    return false;
  }
  if (FAILED(D3DCompile(kShader.data(), kShader.size(), nullptr, nullptr, nullptr,
                        "PSMain", "ps_5_0", 0, 0, &ps_blob, &err))) {
    log_err("PSMain");
    return false;
  }
  if (FAILED(g_device->CreateVertexShader(vs_blob->GetBufferPointer(),
                                          vs_blob->GetBufferSize(), nullptr,
                                          &g_vs))) {
    core::log::Log("CreateVertexShader failed");
    return false;
  }
  if (FAILED(g_device->CreatePixelShader(ps_blob->GetBufferPointer(),
                                         ps_blob->GetBufferSize(), nullptr,
                                         &g_ps))) {
    core::log::Log("CreatePixelShader failed");
    return false;
  }
  return true;
}

bool CreateSwapChain(WinPetSurface& surface, int w, int h) {
  ComPtr<IDXGIDevice> dxgi;
  if (FAILED(g_device.As(&dxgi))) return false;
  ComPtr<IDXGIAdapter> adapter;
  if (FAILED(dxgi->GetAdapter(&adapter))) return false;
  ComPtr<IDXGIFactory2> factory;
  if (FAILED(adapter->GetParent(IID_PPV_ARGS(&factory)))) return false;

  DXGI_SWAP_CHAIN_DESC1 desc{};
  desc.Width = static_cast<UINT>(w);
  desc.Height = static_cast<UINT>(h);
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
  desc.BufferCount = 2;
  desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
  desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
  return SUCCEEDED(factory->CreateSwapChainForComposition(
      g_device.Get(), &desc, nullptr, &surface.swap));
}

bool CreateBackBufferView(WinPetSurface& surface) {
  ComPtr<ID3D11Texture2D> back_buffer;
  if (FAILED(surface.swap->GetBuffer(0, IID_PPV_ARGS(&back_buffer)))) {
    return false;
  }
  D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
  rtv_desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
  surface.rtv.Reset();
  return SUCCEEDED(g_device->CreateRenderTargetView(back_buffer.Get(), &rtv_desc,
                                                    &surface.rtv));
}

}  // namespace

bool RenderInitShared() {
  const UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
  const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0};
  D3D_FEATURE_LEVEL got{};
  const HRESULT hr_device =
      D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags,
                        levels, 1, D3D11_SDK_VERSION, &g_device, &got,
                        &g_context);
  if (FAILED(hr_device)) {
    core::log::Log("D3D11CreateDevice failed: hr=0x{:x}",
                   static_cast<unsigned>(hr_device));
    return false;
  }

  D3D11_SAMPLER_DESC sd{};
  sd.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
  sd.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
  sd.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
  if (FAILED(g_device->CreateSamplerState(&sd, &g_sampler))) {
    core::log::Log("CreateSamplerState failed");
    return false;
  }

  // DComp device 共享，每只宠物只建 target + visual。
  ComPtr<IDXGIDevice> dxgi;
  if (FAILED(g_device.As(&dxgi))) {
    core::log::Log("QueryInterface IDXGIDevice failed");
    return false;
  }
  const HRESULT hr_dcomp = DCompositionCreateDevice(
      dxgi.Get(), IID_PPV_ARGS(g_dcomp.ReleaseAndGetAddressOf()));
  if (FAILED(hr_dcomp)) {
    // COM 未初始化 / DComp 不可用时单独报错，避免一律误报成 D3D11 问题。
    core::log::Log("DCompositionCreateDevice failed: hr=0x{:x}",
                   static_cast<unsigned>(hr_dcomp));
    return false;
  }
  return CompileShaders();
}

bool RenderCreateTarget(WinPetSurface& surface, HWND hwnd, int w, int h) {
  if (!CreateSwapChain(surface, w, h)) return false;
  if (!CreateBackBufferView(surface)) return false;

  if (FAILED(g_dcomp->CreateTargetForHwnd(hwnd, TRUE, &surface.target))) {
    return false;
  }
  if (FAILED(g_dcomp->CreateVisual(&surface.visual))) return false;
  if (FAILED(surface.visual->SetContent(surface.swap.Get()))) return false;
  if (FAILED(surface.target->SetRoot(surface.visual.Get()))) return false;
  if (FAILED(g_dcomp->Commit())) return false;

  D3D11_BUFFER_DESC params_desc{};
  params_desc.ByteWidth = 16;
  params_desc.Usage = D3D11_USAGE_DYNAMIC;
  params_desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
  params_desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  return SUCCEEDED(g_device->CreateBuffer(&params_desc, nullptr, &surface.params));
}

bool RenderCreateClipTextures(WinClipTexture& t, int videoW, int videoH,
                              int maskW, int maskH) {
  // 事务化：全部创建成功后才原子替换。失败时保留旧纹理与旧尺寸，
  // 避免 outW/outH、bgra 与纹理三者不一致造成上传越界。
  WinClipTexture tmp;
  D3D11_TEXTURE2D_DESC desc{};
  desc.Width = static_cast<UINT>(videoW);
  desc.Height = static_cast<UINT>(videoH);
  desc.MipLevels = 1;
  desc.ArraySize = 1;
  desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
  desc.SampleDesc.Count = 1;
  desc.Usage = D3D11_USAGE_DYNAMIC;
  desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(g_device->CreateTexture2D(&desc, nullptr, &tmp.tex))) return false;
  if (FAILED(g_device->CreateShaderResourceView(tmp.tex.Get(), nullptr,
                                                &tmp.srv))) {
    return false;
  }

  // 掩码纹理（R8）：--debug 时用于红色叠加显示。
  D3D11_TEXTURE2D_DESC mdesc{};
  mdesc.Width = static_cast<UINT>(maskW);
  mdesc.Height = static_cast<UINT>(maskH);
  mdesc.MipLevels = 1;
  mdesc.ArraySize = 1;
  mdesc.Format = DXGI_FORMAT_R8_UNORM;
  mdesc.SampleDesc.Count = 1;
  mdesc.Usage = D3D11_USAGE_DYNAMIC;
  mdesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
  mdesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
  if (FAILED(g_device->CreateTexture2D(&mdesc, nullptr, &tmp.maskTex))) {
    return false;
  }
  if (FAILED(g_device->CreateShaderResourceView(tmp.maskTex.Get(), nullptr,
                                                &tmp.maskSrv))) {
    return false;
  }
  tmp.videoW = videoW;
  tmp.videoH = videoH;
  tmp.maskW = maskW;
  tmp.maskH = maskH;
  t = std::move(tmp);
  return true;
}

bool RenderUploadVideo(WinClipTexture& t, const uint8_t* bgra, int w, int h) {
  if (!t.tex || bgra == nullptr) return false;
  // 尺寸不符说明缩放失败后状态没回滚，宁可跳过也不能越界读写。
  if (w <= 0 || h <= 0 || t.videoW <= 0 || t.videoH <= 0 ||
      w != t.videoW || h != t.videoH) {
    return false;
  }
  D3D11_MAPPED_SUBRESOURCE mapped{};
  if (FAILED(g_context->Map(t.tex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                            &mapped))) {
    return false;
  }
  for (int y = 0; y < h; ++y) {
    std::memcpy(
        static_cast<uint8_t*>(mapped.pData) +
            static_cast<size_t>(y) * mapped.RowPitch,
        bgra + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
  }
  g_context->Unmap(t.tex.Get(), 0);
  return true;
}

void RenderUploadMask(WinClipTexture& t, const char* mask, int mw, int mh) {
  if (!t.maskTex || mask == nullptr) return;
  // 尺寸与纹理不符说明缩放失败后状态没回滚，宁可跳过也不能越界写映射内存。
  if (mw != t.maskW || mh != t.maskH || mw <= 0 || mh <= 0) return;
  D3D11_MAPPED_SUBRESOURCE mm{};
  if (FAILED(g_context->Map(t.maskTex.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0,
                            &mm))) {
    return;
  }
  for (int y = 0; y < mh; ++y) {
    uint8_t* row = static_cast<uint8_t*>(mm.pData) + y * mm.RowPitch;
    const char* src = mask + static_cast<size_t>(y) * mw;
    for (int x = 0; x < mw; ++x) row[x] = src[x] ? 255 : 0;
  }
  g_context->Unmap(t.maskTex.Get(), 0);
}

bool RenderFrame(WinPetSurface& surface, const WinClipTexture& t, int w, int h,
                 float scaleY, bool flip, bool debugMask) {
  if (!surface.rtv || !t.srv) return false;
  const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
  g_context->ClearRenderTargetView(surface.rtv.Get(), clear);
  g_context->OMSetRenderTargets(1, surface.rtv.GetAddressOf(), nullptr);

  if (surface.params) {
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (SUCCEEDED(g_context->Map(surface.params.Get(), 0,
                                 D3D11_MAP_WRITE_DISCARD, 0, &mapped))) {
      auto* data = static_cast<float*>(mapped.pData);
      data[0] = scaleY;
      data[1] = debugMask ? 1.0f : 0.0f;
      data[2] = flip ? 1.0f : 0.0f;
      g_context->Unmap(surface.params.Get(), 0);
    }
    g_context->VSSetConstantBuffers(0, 1, surface.params.GetAddressOf());
    g_context->PSSetConstantBuffers(0, 1, surface.params.GetAddressOf());
  }

  D3D11_VIEWPORT vp{0.0f, 0.0f, static_cast<float>(w), static_cast<float>(h),
                    0.0f, 1.0f};
  g_context->RSSetViewports(1, &vp);
  g_context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  g_context->VSSetShader(g_vs.Get(), nullptr, 0);
  g_context->PSSetShader(g_ps.Get(), nullptr, 0);
  g_context->PSSetShaderResources(0, 1, t.srv.GetAddressOf());
  if (t.maskSrv) {
    g_context->PSSetShaderResources(1, 1, t.maskSrv.GetAddressOf());
  }
  g_context->PSSetSamplers(0, 1, g_sampler.GetAddressOf());
  g_context->Draw(3, 0);
  // DComp 交换链由合成器按 vsync 取最新帧，SyncInterval 交给合成器控制；
  // Present(1) 会再等一个 vsync，白白增加一帧延迟。
  const HRESULT hr = surface.swap->Present(0, 0);
  if (FAILED(hr)) {
    // 设备丢失/重置时每 3 秒最多一条日志，并让调用方保留 needPresent 以便重试。
    static DWORD last_log = 0;
    const DWORD now = GetTickCount();
    if (now - last_log >= 3000) {
      last_log = now;
      core::log::Log("Present failed: hr=0x{:x} reason=0x{:x}",
                     static_cast<unsigned>(hr),
                     static_cast<unsigned>(g_device->GetDeviceRemovedReason()));
    }
    return false;
  }
  return true;
}

bool RenderResize(WinPetSurface& surface, int w, int h) {
  if (!surface.swap) return false;
  g_context->OMSetRenderTargets(0, nullptr, nullptr);
  // 必须先 ResizeBuffers 成功再丢弃旧 RTV：失败时旧交换链与旧 RTV 仍然有效，
  // 提前 Reset 会让之后每一帧的 RenderFrame 直接返回，画面永久冻结。
  if (FAILED(surface.swap->ResizeBuffers(0, static_cast<UINT>(w),
                                         static_cast<UINT>(h),
                                         DXGI_FORMAT_UNKNOWN, 0))) {
    return false;
  }
  surface.rtv.Reset();
  // 纹理属于动画实例，由 winclips::ResizeAll 在缩放时立即重建，不在这里处理。
  return CreateBackBufferView(surface);
}
