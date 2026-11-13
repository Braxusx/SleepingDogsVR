// SDVR - contrast adaptive sharpening (build 086)
// One full-screen pass on each eye image just before it goes to the headset (after DLSS when that is on; also works
// without DLSS and on any graphics card). The method is contrast adaptive sharpening as published by AMD (FidelityFX
// CAS): each pixel is sharpened against its 3x3 neighbourhood, less where the local contrast is already high, so edges
// do not ring. Strength 0..1 from sdvr.ini `Sharpen`, adjustable in game from the Insert panel (main.cpp).
// The HUD is its own OpenXR layer and is not sharpened.
#include <windows.h>
#include <d3d11_1.h>
#include <stdio.h>
#include <string.h>
#include "log.h"

extern bool g_rtBypass;
static volatile LONG g_amount = 0;      // strength x 100
static volatile LONG g_on = 1;
static bool g_dead;
float SharpenAmount() { return (float)g_amount / 100.f; }
bool SharpenOn() { return g_on != 0; }
void SharpenSet(float a, bool on) { if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f; g_amount = (LONG)(a * 100.f + 0.5f); g_on = on ? 1 : 0; }

static const char* kShader = R"(
Texture2D<float4> T : register(t0);
cbuffer C : register(b0) { float2 size; float sharp; float pad; };
float4 VS(uint id : SV_VertexID) : SV_Position { return float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0.0, 1.0); }
float3 L(int2 p) { return T.Load(int3(clamp(p, int2(0, 0), int2(size) - 1), 0)).rgb; }
float4 PS(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    float4 e4 = T.Load(int3(p, 0)); float3 e = e4.rgb;
    float3 a = L(p + int2(-1, -1)), b = L(p + int2(0, -1)), c = L(p + int2(1, -1));
    float3 d = L(p + int2(-1, 0)), f = L(p + int2(1, 0));
    float3 g = L(p + int2(-1, 1)), h = L(p + int2(0, 1)), i = L(p + int2(1, 1));
    float3 mn = min(min(min(d, e), min(f, b)), h); mn += min(mn, min(min(a, c), min(g, i)));
    float3 mx = max(max(max(d, e), max(f, b)), h); mx += max(mx, max(max(a, c), max(g, i)));
    float3 amp = sqrt(saturate(min(mn, 2.0 - mx) / max(mx, 1e-5)));
    float3 w = amp * (-1.0 / lerp(8.0, 5.0, sharp));
    float3 r = saturate(((b + d + f + h) * w + e) / (1.0 + 4.0 * w));
    return float4(r, e4.a);
}
)";

static ID3D11Device* g_dev; static ID3D11DeviceContext1* g_ctx; static ID3DDeviceContextState* g_iso;
static ID3D11VertexShader* g_vs; static ID3D11PixelShader* g_ps; static ID3D11Buffer* g_cb;
static ID3D11Texture2D* g_out[2]; static ID3D11RenderTargetView* g_outR[2]; static UINT g_w[2], g_h[2];
static ID3D11Texture2D* g_srvTex[4]; static ID3D11ShaderResourceView* g_srv[4];

typedef HRESULT (WINAPI *Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static ID3DBlob* Compile(const char* entry, const char* target) {
    static Compile_t f;
    if (!f) { HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll"); if (!h) h = LoadLibraryW(L"d3dcompiler_46.dll"); if (h) f = (Compile_t)GetProcAddress(h, "D3DCompile"); }
    if (!f) return nullptr;
    ID3DBlob* b = nullptr, *e = nullptr;
    HRESULT hr = f(kShader, strlen(kShader), "sdvr_sharpen", nullptr, nullptr, entry, target, 0, 0, &b, &e);
    if (FAILED(hr)) { Log("sharpen: shader %s failed 0x%lx: %s", entry, hr, e ? (const char*)e->GetBufferPointer() : ""); if (e) e->Release(); return nullptr; }
    if (e) e->Release();
    return b;
}
static bool InitOnce(ID3D11DeviceContext* ctx) {
    static bool tried, ok; if (tried) return ok; tried = true;
    ctx->GetDevice(&g_dev);
    ID3D11Device1* d1 = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) || FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx))) { Log("sharpen: off - no D3D 11.1"); return false; }
    D3D_FEATURE_LEVEL lv = g_dev->GetFeatureLevel();
    UINT fl = (g_dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRESULT hr = d1->CreateDeviceContextState(fl, &lv, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_iso);
    d1->Release();
    if (FAILED(hr)) { Log("sharpen: off - no context state (0x%lx)", hr); return false; }
    ID3DBlob* bv = Compile("VS", "vs_5_0"), *bp = Compile("PS", "ps_5_0");
    if (!bv || !bp) { Log("sharpen: off - shader compiler"); return false; }
    g_dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_ps);
    bv->Release(); bp->Release();
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    if (!g_vs || !g_ps || !g_cb) { Log("sharpen: off - shader objects"); return false; }
    Log("sharpen: ready");
    ok = true; return true;
}
// a view of the image to sharpen (the game's two eye images and the two DLSS outputs: at most four textures, kept)
static ID3D11ShaderResourceView* ViewOf(ID3D11Texture2D* t) {
    for (int i = 0; i < 4; i++) if (g_srvTex[i] == t) return g_srv[i];
    static int next = 0; int i = next; next = (next + 1) & 3;
    if (g_srv[i]) { g_srv[i]->Release(); g_srv[i] = nullptr; } if (g_srvTex[i]) { g_srvTex[i]->Release(); g_srvTex[i] = nullptr; }
    if (FAILED(g_dev->CreateShaderResourceView(t, nullptr, &g_srv[i]))) { g_srv[i] = nullptr; return nullptr; }
    t->AddRef(); g_srvTex[i] = t;
    return g_srv[i];
}
// render thread, at the eye copy, after DLSS. Returns the sharpened image, or src when off / not possible.
ID3D11Texture2D* SharpenApply(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    LONG amt = g_amount;
    if (!src || eye < 0 || eye > 1 || g_dead || !g_on || amt <= 0) return src;
    if (!InitOnce(ctx)) { g_dead = true; return src; }
    D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd);
    if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || sd.SampleDesc.Count != 1 || !(sd.BindFlags & D3D11_BIND_SHADER_RESOURCE)) { LOG_FIRST(1, "sharpen: image fmt %d bind 0x%x not handled", (int)sd.Format, sd.BindFlags); return src; }
    if (g_w[eye] != sd.Width || g_h[eye] != sd.Height) {
        if (g_outR[eye]) { g_outR[eye]->Release(); g_outR[eye] = nullptr; } if (g_out[eye]) { g_out[eye]->Release(); g_out[eye] = nullptr; }
        D3D11_TEXTURE2D_DESC d = {}; d.Width = sd.Width; d.Height = sd.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1;
        d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        if (FAILED(g_dev->CreateTexture2D(&d, nullptr, &g_out[eye])) || FAILED(g_dev->CreateRenderTargetView(g_out[eye], nullptr, &g_outR[eye]))) { Log("sharpen: off - output texture"); g_dead = true; return src; }
        g_w[eye] = sd.Width; g_h[eye] = sd.Height;
        Log("sharpen: eye %d output %ux%u", eye, sd.Width, sd.Height);
    }
    ID3D11ShaderResourceView* sv = ViewOf(src); if (!sv) { LOG_FIRST(2, "sharpen: no view of the image"); return src; }
    ID3DDeviceContextState* old = nullptr; g_ctx->SwapDeviceContextState(g_iso, &old); g_rtBypass = true;
    float cb[4] = { (float)sd.Width, (float)sd.Height, (float)amt / 100.f, 0.f };
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
    g_ctx->OMSetRenderTargets(1, &g_outR[eye], nullptr); g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0); g_ctx->PSSetShader(g_ps, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb); g_ctx->PSSetShaderResources(0, 1, &sv);
    g_ctx->Draw(3, 0);
    ID3D11RenderTargetView* none = nullptr; g_ctx->OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* nos = nullptr; g_ctx->PSSetShaderResources(0, 1, &nos);
    g_ctx->SwapDeviceContextState(old, nullptr); if (old) old->Release(); g_rtBypass = false;
    return g_out[eye];
}
