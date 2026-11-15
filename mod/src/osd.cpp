// SDVR - on-screen mode label (build 011, from FC4VR build 084)
// A text line is rasterised on the CPU with a 5x7 bitmap font into a small texture of the swapchain's format and
// copied into each eye image (same pixel position in both eyes, so it appears far away) for a few seconds.
#include <windows.h>
#include <d3d11_1.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "log.h"

static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-.()/><+ ";
static const uint8_t kFont[][7] = {   // 5x7, bit 4 = leftmost column
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {0,12,12,0,12,12,0},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},{1,1,2,4,8,16,16},{16,8,4,2,4,8,16},{1,2,4,8,4,2,1},{0,4,4,31,4,4,0},{0,0,0,0,0,0,0}};

// build 141: the panel is see-through. It is drawn into the eye image with blending (dark background 60% opaque, letters solid)
// by a small pass of the mod's own, in its own device context state like the sharpening pass. If that cannot be set up
// (no D3D 11.1, no shader compiler) the old opaque copy is used.
extern bool g_rtBypass;
static int g_blend = -1;   // -1 not tried yet, 0 opaque copy, 1 blended
static ID3D11Device* g_bDev; static ID3D11DeviceContext1* g_bCtx; static ID3DDeviceContextState* g_bIso;
static ID3D11VertexShader* g_bVs; static ID3D11PixelShader* g_bPs; static ID3D11Buffer* g_bCb; static ID3D11BlendState* g_bBl;
static ID3D11ShaderResourceView* g_srv;
static struct { ID3D11Texture2D* t; ID3D11RenderTargetView* v; } g_rtv[16];
static const char* kOsdShader = R"(
Texture2D<float4> T : register(t0);
cbuffer C : register(b0) { float2 off; float2 pad; };
float4 VS(uint id : SV_VertexID) : SV_Position { return float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0.0, 1.0); }
float4 PS(float4 pos : SV_Position) : SV_Target { return T.Load(int3(int2(pos.xy) - int2(off), 0)); }
)";
static bool BlendInit(ID3D11DeviceContext* ctx) {
    if (g_blend >= 0) return g_blend == 1;
    g_blend = 0;
    ctx->GetDevice(&g_bDev); if (!g_bDev) return false;
    ID3D11Device1* d1 = nullptr;
    if (FAILED(g_bDev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) || FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_bCtx))) { Log("osd: opaque panel - no D3D 11.1"); return false; }
    D3D_FEATURE_LEVEL lv = g_bDev->GetFeatureLevel();
    UINT fl = (g_bDev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRESULT hr = d1->CreateDeviceContextState(fl, &lv, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_bIso);
    d1->Release();
    if (FAILED(hr)) { Log("osd: opaque panel - no context state (0x%lx)", hr); return false; }
    typedef HRESULT (WINAPI *Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll"); if (!h) h = LoadLibraryW(L"d3dcompiler_46.dll");
    Compile_t f = h ? (Compile_t)GetProcAddress(h, "D3DCompile") : nullptr;
    ID3DBlob* bv = nullptr, *bp = nullptr;
    if (!f || FAILED(f(kOsdShader, strlen(kOsdShader), "sdvr_osd", nullptr, nullptr, "VS", "vs_5_0", 0, 0, &bv, nullptr)) ||
        FAILED(f(kOsdShader, strlen(kOsdShader), "sdvr_osd", nullptr, nullptr, "PS", "ps_5_0", 0, 0, &bp, nullptr))) { Log("osd: opaque panel - shader compiler"); if (bv) bv->Release(); return false; }
    g_bDev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_bVs);
    g_bDev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_bPs);
    bv->Release(); bp->Release();
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 16; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_bDev->CreateBuffer(&bd, nullptr, &g_bCb);
    D3D11_BLEND_DESC bl = {}; bl.RenderTarget[0].BlendEnable = TRUE;
    bl.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA; bl.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA; bl.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bl.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ZERO; bl.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_ONE; bl.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;   // the eye image keeps its own alpha
    bl.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    g_bDev->CreateBlendState(&bl, &g_bBl);
    if (!g_bVs || !g_bPs || !g_bCb || !g_bBl) { Log("osd: opaque panel - shader objects"); return false; }
    Log("osd: see-through panel ready");
    g_blend = 1; return true;
}
static ID3D11RenderTargetView* TargetOf(ID3D11Texture2D* t, DXGI_FORMAT f) {
    int freeI = -1;
    for (int i = 0; i < 16; i++) { if (g_rtv[i].t == t) return g_rtv[i].v; if (!g_rtv[i].t && freeI < 0) freeI = i; }
    if (freeI < 0) { for (auto& e : g_rtv) { if (e.v) e.v->Release(); e.v = nullptr; e.t = nullptr; } freeI = 0; }   // swapchains were recreated: start again
    D3D11_RENDER_TARGET_VIEW_DESC rd = {}; rd.Format = f; rd.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    ID3D11RenderTargetView* v = nullptr;
    if (FAILED(g_bDev->CreateRenderTargetView(t, &rd, &v))) return nullptr;
    g_rtv[freeI].t = t; g_rtv[freeI].v = v; return v;
}
static ID3D11Texture2D* g_tex;
static UINT g_tw, g_th;
static DXGI_FORMAT g_fmt;
static ULONGLONG g_until;
static bool g_multi;   // 090: a menu panel (several lines) sits in the middle of the view, a one-line label above it
static CRITICAL_SECTION g_cs; static bool g_csInit;
static char g_pending[1024]; static int g_pendSecs; static volatile bool g_hasPending;

// any thread: show this text in the headset for a few seconds (made on the render thread at the next eye copy)
void OsdRequest(const char* text, int seconds) {
    if (!g_csInit) { InitializeCriticalSection(&g_cs); g_csInit = true; }
    EnterCriticalSection(&g_cs);
    snprintf(g_pending, sizeof g_pending, "%s", text); g_pendSecs = seconds; g_hasPending = true;
    LeaveCriticalSection(&g_cs);
}
static void Make(ID3D11DeviceContext* ctx, DXGI_FORMAT fmt, const char* text, int seconds) {
    // 089: several lines (split at line breaks) make a menu panel: smaller letters, the line starting with '>' in white
    const char* ln[24]; int len[24]; int nl = 0, maxc = 1;   // 128: room for more menu lines (the menu had reached 16)
    for (const char* p = text; nl < 24; ) { const char* e = strchr(p, '\n'); ln[nl] = p; len[nl] = e ? (int)(e - p) : (int)strlen(p); if (len[nl] > maxc) maxc = len[nl]; nl++; if (!e) break; p = e + 1; }
    const int S = nl > 1 ? 5 : 8, CW = 6 * S, PAD = 24, LH = 7 * S + (nl > 1 ? 2 * S : 0);
    UINT w = (UINT)(maxc * CW + 2 * PAD), h = (UINT)(nl * LH - (nl > 1 ? 2 * S : 0) + 2 * PAD);
    if (g_tex && (w != g_tw || h != g_th)) { g_tex->Release(); g_tex = nullptr; if (g_srv) { g_srv->Release(); g_srv = nullptr; } }
    uint32_t* px = (uint32_t*)calloc((size_t)w * h, 4);
    bool bgr = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    uint32_t bg = 0xFF202020, yel = bgr ? 0xFFFFFF00 : 0xFF00FFFF;   // dark grey background, yellow text (125: the two byte orders were the wrong way round - text showed cyan)
    const bool srgbDst = fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (g_blend == 1) { bg = 0x99141414; yel = 0xFF00FFFF; fmt = srgbDst ? DXGI_FORMAT_R8G8B8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM; }   // 141: own RGBA picture with alpha
    if (g_tex && fmt != g_fmt) { g_tex->Release(); g_tex = nullptr; if (g_srv) { g_srv->Release(); g_srv = nullptr; } }
    for (UINT i = 0; i < w * h; i++) px[i] = bg;
    for (int l = 0; l < nl; l++) {
        uint32_t fg = (nl > 1 && len[l] && ln[l][0] == '>') ? 0xFFFFFFFF : yel;
        for (int c = 0; c < len[l]; c++) {
            char ch = ln[l][c]; if (ch >= 'a' && ch <= 'z') ch -= 32;
            const char* p = strchr(kChars, ch); int gi = (p && ch) ? (int)(p - kChars) : (int)strlen(kChars) - 1;
            for (int y = 0; y < 7; y++) for (int x = 0; x < 5; x++) if (kFont[gi][y] & (16 >> x))
                for (int yy = 0; yy < S; yy++) for (int xx = 0; xx < S; xx++) px[(PAD + l * LH + y * S + yy) * w + PAD + c * CW + x * S + xx] = fg;
        }
    }
    if (!g_tex) {
        ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
        D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = fmt; d.SampleDesc.Count = 1;
        d.Usage = D3D11_USAGE_DEFAULT; if (g_blend == 1) d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
        HRESULT hr = dev ? dev->CreateTexture2D(&d, nullptr, &g_tex) : E_FAIL;
        if (SUCCEEDED(hr) && g_blend == 1 && FAILED(dev->CreateShaderResourceView(g_tex, nullptr, &g_srv))) { g_srv = nullptr; g_tex->Release(); g_tex = nullptr; hr = E_FAIL; }
        if (dev) dev->Release();
        if (FAILED(hr)) { Log("osd: texture failed (format %u)", fmt); free(px); g_tex = nullptr; return; }
        g_tw = w; g_th = h; g_fmt = fmt;
    }
    ctx->UpdateSubresource(g_tex, 0, nullptr, px, w * 4, 0);
    free(px);
    g_multi = nl > 1;
    g_until = GetTickCount64() + (ULONGLONG)seconds * 1000;
}

// render thread, after an eye image was copied into a swapchain image of size ew x eh
void OsdBlit(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT ew, UINT eh) {
    if (g_hasPending && g_csInit) {
        char t[1024]; int s;
        EnterCriticalSection(&g_cs); memcpy(t, g_pending, sizeof t); s = g_pendSecs; g_hasPending = false; LeaveCriticalSection(&g_cs);
        D3D11_TEXTURE2D_DESC d; dst->GetDesc(&d);
        DXGI_FORMAT f = d.Format;
        if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        BlendInit(ctx);
        Make(ctx, f, t, s);
    }
    if (!g_tex || GetTickCount64() > g_until || ew < g_tw || eh < g_th) return;
    UINT x = (ew - g_tw) / 2, y = g_multi ? (eh - g_th) / 2 : (eh * 46 / 100 > g_th / 2 ? eh * 46 / 100 - g_th / 2 : 0);   // 128: one-line messages just above the middle of the view (were 30% from the top)
    if (g_blend != 1 || !g_srv) { ctx->CopySubresourceRegion(dst, 0, x, y, 0, g_tex, 0, nullptr); return; }
    D3D11_TEXTURE2D_DESC dd; dst->GetDesc(&dd); DXGI_FORMAT f = dd.Format;
    if (f == DXGI_FORMAT_R8G8B8A8_TYPELESS) f = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    if (f == DXGI_FORMAT_B8G8R8A8_TYPELESS) f = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    ID3D11RenderTargetView* rv = TargetOf(dst, f);
    if (!rv) { static bool said; if (!said) { said = true; Log("osd: no render view of the eye image (format %d, bind 0x%x) - panel not shown", (int)dd.Format, dd.BindFlags); } return; }
    ID3DDeviceContextState* old = nullptr; g_bCtx->SwapDeviceContextState(g_bIso, &old); g_rtBypass = true;
    float cb[4] = { (float)x, (float)y, 0.f, 0.f };
    g_bCtx->UpdateSubresource(g_bCb, 0, nullptr, cb, 0, 0);
    D3D11_VIEWPORT vp = { (float)x, (float)y, (float)g_tw, (float)g_th, 0, 1 };
    g_bCtx->OMSetRenderTargets(1, &rv, nullptr); g_bCtx->OMSetBlendState(g_bBl, nullptr, 0xffffffff); g_bCtx->RSSetViewports(1, &vp);
    g_bCtx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g_bCtx->IASetInputLayout(nullptr);
    g_bCtx->VSSetShader(g_bVs, nullptr, 0); g_bCtx->PSSetShader(g_bPs, nullptr, 0);
    g_bCtx->PSSetConstantBuffers(0, 1, &g_bCb); g_bCtx->PSSetShaderResources(0, 1, &g_srv);
    g_bCtx->Draw(3, 0);
    ID3D11RenderTargetView* none = nullptr; g_bCtx->OMSetRenderTargets(1, &none, nullptr);
    ID3D11ShaderResourceView* nos = nullptr; g_bCtx->PSSetShaderResources(0, 1, &nos);
    g_bCtx->SwapDeviceContextState(old, nullptr); if (old) old->Release(); g_rtBypass = false;
}
