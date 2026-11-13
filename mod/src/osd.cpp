// SDVR - on-screen mode label (build 011, from FC4VR build 084)
// A text line is rasterised on the CPU with a 5x7 bitmap font into a small texture of the swapchain's format and
// copied into each eye image (same pixel position in both eyes, so it appears far away) for a few seconds.
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "log.h"

static const char* kChars = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789:-.()/>< ";
static const uint8_t kFont[][7] = {   // 5x7, bit 4 = leftmost column
    {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},{30,17,17,17,17,17,30},{31,16,16,30,16,16,31},
    {31,16,16,30,16,16,16},{14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},{7,2,2,2,2,18,12},
    {17,18,20,24,20,18,17},{16,16,16,16,16,16,31},{17,27,21,21,17,17,17},{17,17,25,21,19,17,17},{14,17,17,17,17,17,14},
    {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},{15,16,16,14,1,1,30},{31,4,4,4,4,4,4},
    {17,17,17,17,17,17,14},{17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},{17,17,10,4,4,4,4},
    {31,1,2,4,8,16,31},
    {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},{31,2,4,2,1,17,14},{2,6,10,18,31,2,2},
    {31,16,30,1,1,17,14},{6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},{14,17,17,15,1,2,12},
    {0,12,12,0,12,12,0},{0,0,0,31,0,0,0},{0,0,0,0,0,12,12},{2,4,8,8,8,4,2},{8,4,2,2,2,4,8},{1,1,2,4,8,16,16},{16,8,4,2,4,8,16},{1,2,4,8,4,2,1},{0,0,0,0,0,0,0}};

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
    if (g_tex && (w != g_tw || h != g_th || fmt != g_fmt)) { g_tex->Release(); g_tex = nullptr; }
    uint32_t* px = (uint32_t*)calloc((size_t)w * h, 4);
    bool bgr = fmt == DXGI_FORMAT_B8G8R8A8_UNORM || fmt == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB || fmt == DXGI_FORMAT_B8G8R8A8_TYPELESS;
    uint32_t bg = 0xFF202020, yel = bgr ? 0xFFFFFF00 : 0xFF00FFFF;   // dark grey background, yellow text (125: the two byte orders were the wrong way round - text showed cyan)
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
        d.Usage = D3D11_USAGE_DEFAULT;
        HRESULT hr = dev ? dev->CreateTexture2D(&d, nullptr, &g_tex) : E_FAIL;
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
        Make(ctx, f, t, s);
    }
    if (!g_tex || GetTickCount64() > g_until || ew < g_tw || eh < g_th) return;
    UINT x = (ew - g_tw) / 2, y = g_multi ? (eh - g_th) / 2 : (eh * 46 / 100 > g_th / 2 ? eh * 46 / 100 - g_th / 2 : 0);   // 128: one-line messages just above the middle of the view (were 30% from the top)
    ctx->CopySubresourceRegion(dst, 0, x, y, 0, g_tex, 0, nullptr);
}
