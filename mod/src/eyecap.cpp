// SDVR - F10 eye capture (build 013): saves both eye images of one frame as BMPs (half size) next to the game exe,
// so differences between the eyes can be compared offline.
#include <windows.h>
#include <d3d11.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include "log.h"

static volatile LONG g_req = 0;      // set by the key thread
static int g_stage = 0;              // 0 idle, 1 capturing this frame
static bool g_done[2];
static int g_n = 0;
static wchar_t g_dir[MAX_PATH];
void EyeCapInit(const wchar_t* dir) { wcsncpy(g_dir, dir, MAX_PATH - 1); }
void EyeCapRequest() { InterlockedExchange(&g_req, 1); }

static void SaveTag(const wchar_t* tag, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, bool alpha, UINT step = 2) {
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (d.Format != DXGI_FORMAT_R8G8B8A8_UNORM && d.Format != DXGI_FORMAT_R8G8B8A8_UNORM_SRGB && d.Format != DXGI_FORMAT_R8G8B8A8_TYPELESS &&
        d.Format != DXGI_FORMAT_B8G8R8A8_UNORM && d.Format != DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { Log("eyecap: format %d not supported", (int)d.Format); return; }
    bool bgra = d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    D3D11_TEXTURE2D_DESC s = d; s.Usage = D3D11_USAGE_STAGING; s.BindFlags = 0; s.CPUAccessFlags = D3D11_CPU_ACCESS_READ; s.MiscFlags = 0;
    s.MipLevels = 1; s.ArraySize = 1; s.SampleDesc.Count = 1;
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev);
    ID3D11Texture2D* st = nullptr;
    if (!dev || FAILED(dev->CreateTexture2D(&s, nullptr, &st))) { Log("eyecap: staging texture failed"); if (dev) dev->Release(); return; }
    dev->Release();
    ctx->CopySubresourceRegion(st, 0, 0, 0, 0, src, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (FAILED(ctx->Map(st, 0, D3D11_MAP_READ, 0, &m))) { st->Release(); Log("eyecap: map failed"); return; }
    UINT w = d.Width / step, h = d.Height / step, row = (w * 3 + 3) & ~3u;
    uint8_t* out = (uint8_t*)malloc((size_t)row * h);
    for (UINT y = 0; y < h; y++) {
        const uint8_t* p = (const uint8_t*)m.pData + (size_t)(y * step) * m.RowPitch;
        uint8_t* o = out + (size_t)(h - 1 - y) * row;
        for (UINT x = 0; x < w; x++) {
            const uint8_t* q = p + (size_t)x * step * 4;
            if (alpha) { o[x * 3 + 0] = o[x * 3 + 1] = o[x * 3 + 2] = q[3]; continue; }
            o[x * 3 + 0] = bgra ? q[0] : q[2]; o[x * 3 + 1] = q[1]; o[x * 3 + 2] = bgra ? q[2] : q[0];
        }
    }
    ctx->Unmap(st, 0); st->Release();
    wchar_t path[MAX_PATH]; if (!wcsncmp(tag, L"lburst", 6) || !wcsncmp(tag, L"check", 5)) swprintf(path, MAX_PATH, L"%ls\\sdvr_%ls.bmp", g_dir, tag); else swprintf(path, MAX_PATH, L"%ls\\sdvr_cap%02d_%ls.bmp", g_dir, g_n, tag);
    FILE* f = _wfopen(path, L"wb");
    if (f) {
        BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {};
        fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + row * h;
        ih.biSize = sizeof ih; ih.biWidth = (LONG)w; ih.biHeight = (LONG)h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = row * h;
        fwrite(&fh, sizeof fh, 1, f); fwrite(&ih, sizeof ih, 1, f); fwrite(out, row, h, f); fclose(f);
        Log("eyecap: saved %ls (%ux%u)", path, w, h);
    } else Log("eyecap: could not write %ls", path);
    free(out);
}
// render thread, from the eye submit (eye 0 then eye 1 of the same frame)
// build 055: left-eye burst (Shift+F10) - every 6th frame for 360 frames (4 s at 90 Hz), quarter size,
// sdvr_lburstNN_fKKK_eye0.bmp; the right eye is saved once at the start for reference
static volatile LONG g_burstReq = 0; static int g_burst = -1, g_burstN = 0;
void EyeCapBurst() { InterlockedExchange(&g_burstReq, 1); }
static void BurstGrab(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    if (eye == 0 && g_burst < 0 && InterlockedExchange(&g_burstReq, 0)) { g_burst = 0; Log("eyecap: left-eye burst %d started", g_burstN); }
    if (g_burst < 0) return;
    if (eye == 1) { if (g_burst == 1) { wchar_t t[40]; swprintf(t, 40, L"lburst%02d_f000_eye1", g_burstN); SaveTag(t, ctx, src, false, 4); } return; }
    if (g_burst % 6 == 0) { wchar_t t[40]; swprintf(t, 40, L"lburst%02d_f%03d_eye0", g_burstN, g_burst); SaveTag(t, ctx, src, false, 4); }
    if (++g_burst >= 360) { g_burst = -1; Log("eyecap: left-eye burst %d done", g_burstN); g_burstN++; }
}
void EyeCapGrab(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    BurstGrab(eye, ctx, src);
    // build 029: 4 consecutive frames per request (sdvr_capNN_fK_eyeE.bmp) to see frame-to-frame flicker
    static int fk = 0;
    if (eye == 0 && g_stage == 0 && InterlockedExchange(&g_req, 0)) { g_stage = 1; fk = 0; g_done[0] = g_done[1] = false; }
    if (g_stage != 1 || g_done[eye]) return;
    wchar_t t[32]; swprintf(t, 32, L"f%d_eye%d", fk, eye);
    SaveTag(t, ctx, src, false); g_done[eye] = true;
    if (g_done[0] && g_done[1]) { g_done[0] = g_done[1] = false; if (++fk >= 4) { g_stage = 0; g_n++; } }
}

// build 022: the HUD image after each UI render of the capture frame (colour and alpha)
static int g_hudSaved = 0; static bool g_hudArm = false;
void EyeCapArmHud() { g_hudArm = true; g_hudSaved = 0; }
void EyeCapHud(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    if (!g_hudArm) return;
    wchar_t t[32];
    swprintf(t, 32, L"hud%d", g_hudSaved); SaveTag(t, ctx, tex, false);
    swprintf(t, 32, L"hud%d_alpha", g_hudSaved); SaveTag(t, ctx, tex, true);
    if (++g_hudSaved >= 2) g_hudArm = false;
}

// build 144: picture check. Asked for with EyeCheck=1 in sdvr.ini (the launcher's "Record a picture check" button). After
// 12 seconds of uninterrupted play (no menu screen) the two eye pictures of one frame and the HUD panel's picture are saved
// at quarter size as sdvr_check_eye0.bmp (left), sdvr_check_eye1.bmp (right), sdvr_check_hud.bmp and sdvr_check_hud_alpha.bmp
// next to the game exe; the launcher's log collection picks them up. These are the pictures as handed to the headset, so
// they show whether the two eyes match and whether anything stray is on the HUD panel.
static volatile LONG g_chkArm = 0, g_chkDone = 0; static DWORD g_chkPlay0 = 0, g_chkEyesAt = 0; static int g_chkStage = 0;   // 1 = eye 0 saved, 2 = both
void XrViewReportNow();
void EyeCheckArm() { g_chkStage = 0; g_chkPlay0 = 0; InterlockedExchange(&g_chkArm, 1); }
bool EyeCheckDone() { return InterlockedExchange(&g_chkDone, 0) != 0; }
void EyeCheckGrab(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src, bool menu) {
    if (!g_chkArm) return;
    if (g_chkStage == 2) {   // eyes saved: the HUD picture follows when the HUD is drawn; without one, finish after 2 s
        if (GetTickCount() - g_chkEyesAt > 2000) { Log("picture check: no HUD picture came (HUD panel off or hidden) - done without it"); g_chkArm = 0; InterlockedExchange(&g_chkDone, 1); }
        return;
    }
    if (menu) { g_chkPlay0 = 0; g_chkStage = 0; return; }
    DWORD now = GetTickCount();
    if (!g_chkPlay0) g_chkPlay0 = now ? now : 1;
    if (now - g_chkPlay0 < 12000) return;
    if (g_chkStage == 0 && eye != 0) return;   // both eyes from the same frame: start with the first
    SaveTag(eye ? L"check_eye1" : L"check_eye0", ctx, src, false, 4);
    if (eye == 0) { g_chkStage = 1; return; }
    g_chkStage = 2; g_chkEyesAt = now;
    Log("picture check: both eye pictures saved (sdvr_check_eye0.bmp = left, sdvr_check_eye1.bmp = right, quarter size)");
    XrViewReportNow();
}
void EyeCheckHud(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    if (!g_chkArm || g_chkStage != 2 || !tex) return;
    SaveTag(L"check_hud", ctx, tex, false, 4); SaveTag(L"check_hud_alpha", ctx, tex, true, 4);
    Log("picture check: HUD panel picture saved (sdvr_check_hud.bmp, and its coverage sdvr_check_hud_alpha.bmp)");
    g_chkArm = 0; InterlockedExchange(&g_chkDone, 1);
}
