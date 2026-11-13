// SDVR - Sleeping Dogs: Definitive Edition native stereo VR mod
// Build 001: proves the game's own stereo renderer (its former NVIDIA 3D Vision path) can be driven by the mod.
//   * dinput8.dll proxy (the game imports DINPUT8.dll)
//   * replaces the game's stereo driver object (global at +0x243a3e0) with ours: always "active", lets the game
//     create its own swap chain, and receives the two eye images at present (Compose, vtable +0x38)
//   * logs the per-eye frame calls (+0xb790) and the stereo flags
//   * mirror: F7 cycles which eye is shown on the monitor (left / right / alternating every frame)
#include <windows.h>
#include <d3d11.h>
#include <dxgi.h>
#include <psapi.h>
#include <shlwapi.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <math.h>
#include <tlhelp32.h>
#include "MinHook.h"
#include "log.h"
#include "xr.h"
void OsdRequest(const char* text, int seconds);
void XrHudSubmit(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex); void RtDiagInstall(ID3D11DeviceContext* ctx); void RtDiagSetEyes(ID3D11Resource* a, ID3D11Resource* b); void RtDiagArm(bool on); void RtDiagNewFrame(); void RtMinimapArm(ID3D11RenderTargetView* hud); void RtSetHudRtv(ID3D11RenderTargetView* v, int glowFix); LONG RtGlowFixCount(); void RtMinimapFitViewport(D3D11_VIEWPORT* v); void RtSceneryFixStats(char* b, int n); extern volatile int g_sceneryFix; void RtDumpInit(const wchar_t* dir); void RtDumpArm(); void RtDumpFrame(); void ShaderInfoInstall(); int ShaderCount(); void RtQueryTest(int on); void RtQueryStats(char* b, int n); void RtQueryInstall(ID3D11DeviceContext* ctx); int RtSkipNext(char* label, int n); void RtMinimapFit(int on); void RtOutlineTrace(bool on); LONG RtMinimapRedirects(); void RtMinimapSetup(void (*flush)(), float aspect); void RtOutlineMark(const char* m); void RtOutlineArm(); void RtOutlineFrame(); void RtOutlineInstall(ID3D11DeviceContext* ctx); void XrTimingCapture(); void RtUiRedirect(ID3D11RenderTargetView* hud); LONG RtUiRedirectCount(); void XrSetMenuMode(bool m); float XrMenuTan(); void XrSetHudLock(int world); void XrSetHudAnchor(float x, float y, float z, float yaw);
void HalDiagInstall(volatile bool* inEyeFlag); void HalDiagReport(long long frames);
void EyeCapInit(const wchar_t* dir); void EyeCapBurst(); void EyeCapRequest(); void EyeCapArmHud(); void EyeCapHud(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex);

#define SDVR_BUILD "140"

static HMODULE g_self;
static wchar_t g_dir[MAX_PATH], g_ini[MAX_PATH];
uintptr_t g_base;   // sdhdship.exe image base (ASLR)

template <class T> static T& At(uintptr_t rva) { return *(T*)(g_base + rva); }

// ---------------------------------------------------------------------------
// game addresses (RVAs into sdhdship.exe, SHA-256 2A33EC78...C0527DA)
namespace rva {
constexpr uintptr_t StereoCreate   = 0x69f9e0;   // creates the stereo driver object, stores it at StereoObj
constexpr uintptr_t StereoObj      = 0x243a3e0;  // IStereo* (NVIDIA 3D Vision object or a null one)
constexpr uintptr_t EyeFrame       = 0xb790;     // per-eye frame: (float, int eye) - offsets the camera, calls the render callback
constexpr uintptr_t Present        = 0x6a0870;   // render-thread present task (composes the eyes, presents)
constexpr uintptr_t FlagFullscreen = 0x2439a42;
constexpr uintptr_t FlagStereoOn   = 0x2439a8e;  // stereo active this frame (Stereo3D setting && driver active)
constexpr uintptr_t FlagStereoReq  = 0x2439a8d;
constexpr uintptr_t EyeIndex       = 0x2439a78;
constexpr uintptr_t SettingsStereo = 0x243a2f1;  // display settings copy (+0x21 = Stereo3D)
constexpr uintptr_t EyeSeparation  = 0x2093450;
}

struct Sig { uintptr_t rva; const char* bytes; };
static const Sig kSigs[] = {
    { rva::StereoCreate, "40 55 56 57 48 81 EC 80 02 00 00" },
    { rva::EyeFrame,     "48 89 7C 24 20 F3 0F 11 44 24 08 55" },
    { rva::Present,      "48 89 5C 24 08 57 48 83 EC 30 80 3D" },
    { 0x5d860,           "48 8B C4 57 48 81 EC 00 01 00 00 48" },   // ViewRebuild: render view +0x3F0 from the camera, then the stage updates
    { 0x3d8a0,           "48 8B C4 48 89 58 18 48 89 70 20 55" },
    { 0x32700,           "48 8B C4 55 57 48 81 EC F8 00 00 00" },   // RenderStageEnvMap update (view)
    { 0x2ed90,           "40 55 56 57 41 54 41 55 41 56 41 57" },   // RenderStageImposters update (view)
    { 0x33b30,           "40 53 48 83 EC 30 48 8B 91 88 00 00" },   // RenderStageExposureMetering update
    { 0x58910,           "48 83 EC 38 48 C7 44 24 20 FE FF FF FF" },   // game UI render command (Scaleform frame)
    { 0x611f0,           "48 89 5C 24 08 57 48 83 EC 20 8B 41 58" },   // FrontEndPauseEffect update (blur level +0x58)
    { 0x61720,           "40 57 41 56 41 57 48 83 EC 40 48 C7 44 24" },   // render command queue (game thread)
    { 0x69ea60,          "40 53 48 83 EC 70 33 DB 4C 8B C1 8B D1" },      // eye copy render command (render thread)
    { 0xb9a90,           "48 89 5C 24 08 57 48 83 EC 60 48 8B F9" },   // camera SetView (rebuilds +0x40 / +0x80), called by the camera update
    { 0x36420,           "48 8B C4 55 41 54 41 55 41 56 41 57" },
    { 0x3e50c0,          "48 8B C4 53 55 56 57 41 54 41 55 41 56 41 57 48 83 EC 58" },
    { 0xb9d40,           "80 B9 00 01 00 00 00 48 8D 81 10 01 00 00" },
    { 0x6c6f0,           "48 8B C4 44 88 40 18 48 89 50 10 55 56 57 41 54" },   // volumetric fog / scattering update (time-sliced)
    { 0x4ef510,          "F3 0F 11 05 EC 42 C7 01" },                           // DE volumetric fog intensity setter -> 0x142163804   // camera view getter (override +0x110 or view +0x40)
    { 0x188bd0,          "40 53 48 83 EC 40 F3 0F 59 0D" },               // perspective builder (dst, fovY rad, aspect, near, far)
    { 0x615a18,          "E8 C3 40 AA FF" },                              // objective markers: return site of their perspective call
    { 0x615a34,          "48 8B C8 4C 8D 85 40 01 00 00" },              // objective markers: return site of their view call   // cutscene (NIS) manager SetPlayTime(mgr, sceneTime, sync)      // shadow collector cbShadowTransform builder   // RenderStageShadow update (view): fits the shadow cascades to view +0x3F0
};
static bool CheckSig(const Sig& s) {
    const uint8_t* p = (const uint8_t*)(g_base + s.rva);
    const char* b = s.bytes;
    for (int i = 0; *b; ++i) {
        while (*b == ' ') ++b;
        if (!*b) break;
        unsigned v; sscanf(b, "%2x", &v); b += 2;
        if (p[i] != (uint8_t)v) { Log("signature mismatch at +0x%llx byte %d (%02x, expected %02x)", (unsigned long long)s.rva, i, p[i], v); return false; }
    }
    return true;
}

// ---------------------------------------------------------------------------
// settings / state
static volatile int g_swapOrder = 0;       // build 014 (F3): the game's first eye pass renders the RIGHT eye, the second the left
static volatile int g_mirror = 0;          // 0 left, 1 right, 2 alternate
static volatile LONG64 g_presents = 0, g_eyeFrames[2] = {}, g_composes[2] = {};
static volatile float g_lastSep = 0;

// ---------------------------------------------------------------------------
// our stereo driver object. The game calls it through a vtable (x64 MS ABI, this in rcx).
struct Stereo;
static void* g_stereoVtbl[16];
static void* g_gameStereo = nullptr;    // the object the game made (kept, unused)
struct Stereo { void** vtbl; void* pad; uint8_t active; uint8_t pad2[7]; };
static Stereo g_stereo;

static void* __fastcall St_Dtor(Stereo* s, int) { return s; }
static int __fastcall St_Init(Stereo* s) { s->active = 1; Log("stereo: Init"); return 0; }
static int __fastcall St_InitDevice(Stereo*, void* dev, void* p) { Log("stereo: InitDevice dev=%p %p", dev, p); return 0; }
static int __fastcall St_IsActive(Stereo* s) { return s->active; }
static int __fastcall St_Slot4(Stereo*) { return 1; }
static int __fastcall St_CreateSwapChain(Stereo*, void* factory, void* dev, void* desc, void* out) {
    LOG_FIRST(4, "stereo: CreateSwapChain asked (factory %p dev %p) - the game makes its own", factory, dev);
    return (int)0x80004005;   // not handled: the game calls the factory itself
}
static int __fastcall St_SetEye(Stereo*, int eye) { return 0; }
static void DescribeTex(const char* what, ID3D11Resource* r) {
    if (!r) { Log("%s: null", what); return; }
    D3D11_RESOURCE_DIMENSION d; r->GetType(&d);
    if (d != D3D11_RESOURCE_DIMENSION_TEXTURE2D) { Log("%s: %p dimension %d", what, (void*)r, (int)d); return; }
    D3D11_TEXTURE2D_DESC td; ((ID3D11Texture2D*)r)->GetDesc(&td);
    Log("%s: %p %ux%u fmt %d samples %u mips %u bind 0x%x", what, (void*)r, td.Width, td.Height, (int)td.Format, td.SampleDesc.Count, td.MipLevels, td.BindFlags);
}
static ID3D11DeviceContext* volatile g_immCtx;
ID3D11Texture2D* DlssEvaluate(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src);
void DlssSetSeq(long seq); void DlssSetCamera(long seq, int eye, const float* V, const float* P, float jx, float jy);
void DlssJitter(long seq, float* jx, float* jy); void DlssConfigure(const wchar_t* ini, int* w, int* h); const char* DlssCycle(); const char* DlssJitterCycle(); const char* DlssLodCycle();
ID3D11Texture2D* SharpenApply(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src); float SharpenAmount(); bool SharpenOn(); void SharpenSet(float a, bool on);
static void __fastcall St_Compose(Stereo*, int eye, ID3D11DeviceContext* ctx, ID3D11Resource* dst, ID3D11Resource* src, int x) {
    if (eye < 0 || eye > 1) return;
    g_immCtx = ctx;
    { static ID3D11Resource* e[2]; e[eye] = src; RtDiagSetEyes(e[0], e[1]); RtDiagInstall(ctx); RtOutlineInstall(ctx); RtQueryInstall(ctx); }
    LONG64 n = InterlockedIncrement64(&g_composes[eye]);
    if (n <= 2) { char w[64]; snprintf(w, sizeof w, "compose eye %d source", eye); DescribeTex(w, src); DescribeTex("compose destination", dst); }
    if (ctx && src) { ID3D11Texture2D* up = DlssEvaluate(eye, ctx, (ID3D11Texture2D*)src); up = SharpenApply(eye, ctx, up ? up : (ID3D11Texture2D*)src); XrSubmitEye(g_swapOrder ? 1 - eye : eye, ctx, up); }   // 079 DLSS, 086 sharpening
    int show = g_mirror == 2 ? (int)(g_presents & 1) : g_mirror;
    if (eye != show || !ctx || !dst || !src) return;
    D3D11_TEXTURE2D_DESC a, b; ((ID3D11Texture2D*)src)->GetDesc(&a); ((ID3D11Texture2D*)dst)->GetDesc(&b);
    if (a.Width == b.Width && a.Height == b.Height && a.Format == b.Format && a.SampleDesc.Count == b.SampleDesc.Count) ctx->CopyResource(dst, src);
    else {
        UINT w = a.Width < b.Width ? a.Width : b.Width, h = a.Height < b.Height ? a.Height : b.Height;
        UINT sx = (a.Width - w) / 2, sy = (a.Height - h) / 2, dx = (b.Width - w) / 2, dy = (b.Height - h) / 2;   // centre crop (019)
        D3D11_BOX box = { sx, sy, 0, sx + w, sy + h, 1 };
        ctx->CopySubresourceRegion(dst, 0, dx, dy, 0, src, 0, &box);
        LOG_FIRST(1, "compose: eye and back buffer differ (%ux%u fmt %d / %ux%u fmt %d) - copying the overlap", a.Width, a.Height, (int)a.Format, b.Width, b.Height, (int)b.Format);
    }
}
static int __fastcall St_Enable(Stereo*) { Log("stereo: Enable"); return 0; }
static int __fastcall St_Disable(Stereo*) { Log("stereo: Disable"); return 0; }
static int __fastcall St_Release(Stereo*) { return 0; }
static int __fastcall St_Unknown(Stereo*) { LOG_FIRST(8, "stereo: unexpected method called"); return 0; }

// ---------------------------------------------------------------------------
// build 019: windowed mode + per-eye render size.
// (1) The stereo path and the engine BackBuffer target are only used when the Fullscreen flag 0x142439a42 is set. The render
//     side reads of that flag are patched to behave as if it were set; the window / swap chain code keeps the real value,
//     so the game can run in a normal window.
// (2) The render size 0x142439a94/98 (all engine targets incl. the eye targets) is normally the window size. When ini
//     RenderWidth/RenderHeight are set, the settings-apply code stores those instead (code cave at +0x6a166c); the swap
//     chain keeps the window size and the monitor mirror shows a centre crop of the eye.
static int g_rw = 0, g_rh = 0;
static bool PatchBytes(uintptr_t rva, const uint8_t* expect, size_t n, const uint8_t* repl, const char* what) {
    uint8_t* p = (uint8_t*)(g_base + rva);
    if (memcmp(p, expect, n)) { Log("patch %s (+0x%llx): bytes differ - skipped", what, (unsigned long long)rva); return false; }
    DWORD o; VirtualProtect(p, n, PAGE_EXECUTE_READWRITE, &o); memcpy(p, repl, n); VirtualProtect(p, n, o, &o);
    FlushInstructionCache(GetCurrentProcess(), p, n);
    return true;
}
static void PatchWindowed() {
    int ok = 0;
    { const uint8_t e[] = { 0x74, 0x0d }, r[] = { 0x90, 0x90 }; ok += PatchBytes(0xb3e3, e, 2, r, "fs stereo value"); }
    { const uint8_t e[] = { 0x75, 0x08 }, r[] = { 0xEB, 0x08 }; ok += PatchBytes(0x483f0, e, 2, r, "fs target select A"); }
    { const uint8_t e[] = { 0x0f, 0x84, 0xb8, 0, 0, 0 }, r[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; ok += PatchBytes(0x484ee, e, 6, r, "fs eye copy A"); }
    { const uint8_t e[] = { 0x75, 0x0f }, r[] = { 0xEB, 0x0f }; ok += PatchBytes(0x4a26b, e, 2, r, "fs value B"); }
    { const uint8_t e[] = { 0x0f, 0x84, 0xbb, 0, 0, 0 }, r[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; ok += PatchBytes(0x4a442, e, 6, r, "fs eye copy B"); }
    { const uint8_t e[] = { 0x0f, 0x85, 0xd1, 0x01, 0, 0 }, r[] = { 0x90, 0xE9, 0xd1, 0x01, 0, 0 }; ok += PatchBytes(0x53d66, e, 6, r, "fs target select C"); }
    { const uint8_t e[] = { 0x0f, 0x84, 0xea, 0, 0, 0 }, r[] = { 0x90, 0x90, 0x90, 0x90, 0x90, 0x90 }; ok += PatchBytes(0x6a0883, e, 6, r, "fs present path"); }
    { const uint8_t e[] = { 0x74, 0x3c }, r[] = { 0x90, 0x90 }; ok += PatchBytes(0x6a111c, e, 2, r, "fs stereo frame"); }
    Log("patch: windowed - %d of 8 fullscreen checks bypassed on the render side", ok);
}
static void PatchRenderSize() {
    if (g_rw <= 0 || g_rh <= 0) { Log("render size: window size (RenderWidth/RenderHeight not set)"); return; }
    // +0x6a166c: mov [a94],esi; mov [a98],edi; cmp [a90],r14b; je +0x6a1696  (21 bytes, next +0x6a1681)
    const uint8_t e[] = { 0x89, 0x35, 0x22, 0x84, 0xd9, 0x01, 0x89, 0x3d, 0x20, 0x84, 0xd9, 0x01, 0x44, 0x38, 0x35, 0x11, 0x84, 0xd9, 0x01, 0x74, 0x15 };
    uint8_t* site = (uint8_t*)(g_base + 0x6a166c);
    if (memcmp(site, e, sizeof e)) { Log("render size: settings-apply site differs - not patched"); return; }
    uint8_t* cave = nullptr;   // within 2 GB is not needed (absolute addressing), any executable memory will do
    cave = (uint8_t*)VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!cave) { Log("render size: no cave memory"); return; }
    uint8_t* c = cave;
    auto u64 = [&](uint64_t v) { memcpy(c, &v, 8); c += 8; };
    auto u32 = [&](uint32_t v) { memcpy(c, &v, 4); c += 4; };
    *c++ = 0x48; *c++ = 0xB8; u64(g_base + 0x2439a94);                 // mov rax, &renderW
    *c++ = 0xC7; *c++ = 0x00; u32((uint32_t)g_rw);                     // mov dword [rax], W
    *c++ = 0xC7; *c++ = 0x40; *c++ = 0x04; u32((uint32_t)g_rh);        // mov dword [rax+4], H
    *c++ = 0x48; *c++ = 0xB8; u64(g_base + 0x2439a90);                 // mov rax, &flag a90
    *c++ = 0x44; *c++ = 0x38; *c++ = 0x30;                             // cmp [rax], r14b
    *c++ = 0x74; *c++ = 14;                                            // je -> second jump
    *c++ = 0xFF; *c++ = 0x25; u32(0); u64(g_base + 0x6a1681);          // jmp [rip] -> +0x6a1681
    *c++ = 0xFF; *c++ = 0x25; u32(0); u64(g_base + 0x6a1696);          // jmp [rip] -> +0x6a1696
    uint8_t r[21]; memset(r, 0x90, sizeof r);
    r[0] = 0xFF; r[1] = 0x25; uint32_t z = 0; memcpy(r + 2, &z, 4); uint64_t ca = (uint64_t)cave; memcpy(r + 6, &ca, 8);   // jmp [rip] -> cave
    DWORD o; VirtualProtect(site, sizeof r, PAGE_EXECUTE_READWRITE, &o); memcpy(site, r, sizeof r); VirtualProtect(site, sizeof r, o, &o);
    FlushInstructionCache(GetCurrentProcess(), site, sizeof r);
    Log("render size: settings apply now sets %dx%d (window keeps its own size)", g_rw, g_rh);
}

static void InstallStereoObject() {
    void*& slot = At<void*>(rva::StereoObj);
    if (slot == &g_stereo) return;
    g_gameStereo = slot;
    for (auto& v : g_stereoVtbl) v = (void*)St_Unknown;
    g_stereoVtbl[0] = (void*)St_Dtor; g_stereoVtbl[1] = (void*)St_Init; g_stereoVtbl[2] = (void*)St_InitDevice;
    g_stereoVtbl[3] = (void*)St_IsActive; g_stereoVtbl[4] = (void*)St_Slot4; g_stereoVtbl[5] = (void*)St_CreateSwapChain;
    g_stereoVtbl[6] = (void*)St_SetEye; g_stereoVtbl[7] = (void*)St_Compose; g_stereoVtbl[8] = (void*)St_Enable;
    g_stereoVtbl[9] = (void*)St_Disable; g_stereoVtbl[10] = (void*)St_Release;
    g_stereo.vtbl = g_stereoVtbl; g_stereo.active = 1;
    slot = &g_stereo;
    Log("stereo: the game's stereo object %p replaced by the mod's", g_gameStereo);
}

// ---------------------------------------------------------------------------
// hooks
// the device/stereo init returns a bool (success) - it must be passed through (build 003 lost it and the game retried forever)
typedef uintptr_t (__fastcall *StereoCreate_t)(void*, void*, void*, void*);
static StereoCreate_t oStereoCreate;
static uintptr_t __fastcall hkStereoCreate(void* a, void* b, void* c, void* d) {
    uintptr_t r = oStereoCreate(a, b, c, d);
    Log("stereo: device init returned %d, game made its stereo object %p", (int)(r & 0xff), At<void*>(rva::StereoObj));
    if (r & 0xff) InstallStereoObject();
    return r;
}
typedef void (*EyeFrame_t)(float, int);
static EyeFrame_t oEyeFrame;
// build 006: head tracking. The camera's view matrix (camera +0x40, world->camera, row vectors: p_cam = p_world * V) uses
// x right, y up, -z forward like OpenXR, and +0xC0 is an OpenGL-style projection (row vectors, w = -z). Per eye:
//   V_eye = V_game * inverse(E), E = the eye's pose in the camera's space (head pose relative to the recentred yaw/position)
//   P_eye = asymmetric projection from the eye's OpenXR FOV (the game's near/far terms kept)
// and the game's render callback is called once per eye, like its own stereo code did (which only shifted along x).
static volatile bool g_recenterReq = true; static int g_autoRecenter = 1;   // 129: AutoRecenter
static float g_rcYaw = 0, g_rcPos[3] = {0, 0, 0};
static int g_track = 1, g_positional = 1;
static float g_worldScale = 1.0f;
static void QRows(const XrQuaternionf& q, float R[3][3]) {   // rows = the rotated x, y, z axes
    float x = q.x, y = q.y, z = q.z, w = q.w;
    R[0][0] = 1 - 2 * (y * y + z * z); R[0][1] = 2 * (x * y + w * z);     R[0][2] = 2 * (x * z - w * y);
    R[1][0] = 2 * (x * y - w * z);     R[1][1] = 1 - 2 * (x * x + z * z); R[1][2] = 2 * (y * z + w * x);
    R[2][0] = 2 * (x * z + w * y);     R[2][1] = 2 * (y * z - w * x);     R[2][2] = 1 - 2 * (x * x + y * y);
}
static void Mul4(const float* a, const float* b, float* o) {   // o = a * b (row-major 4x4)
    float t[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { float v = 0; for (int k = 0; k < 4; k++) v += a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = v; }
    memcpy(o, t, 64);
}
static void InvRigid(const float* m, float* o) {   // inverse of [R 0; t 1] (row vectors) = [R^T 0; -t R^T 1]
    float t[16] = {};
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) t[r * 4 + c] = m[c * 4 + r];
    for (int c = 0; c < 3; c++) t[12 + c] = -(m[12] * t[0 * 4 + c] + m[13] * t[1 * 4 + c] + m[14] * t[2 * 4 + c]);
    t[15] = 1; memcpy(o, t, 64);
}
static void Recenter(const XrView v[2]) {
    XrQuaternionf q = v[0].pose.orientation; float R[3][3]; QRows(q, R);
    g_rcYaw = atan2f(R[2][0], R[2][2]);   // yaw of the head's back (+z) axis about +y
    for (int k = 0; k < 3; k++) g_rcPos[k] = 0.5f * ((&v[0].pose.position.x)[k] + (&v[1].pose.position.x)[k]);
    Log("recenter: yaw %.1f deg, head at %.3f %.3f %.3f", g_rcYaw * 57.2958f, g_rcPos[0], g_rcPos[1], g_rcPos[2]);
    XrSetHudAnchor(g_rcPos[0], g_rcPos[1], g_rcPos[2], g_rcYaw);
}
static void EyeInCamera(const XrView& v, const XrView& other, float* E) {   // eye -> camera space (recentred), row vectors
    float R[3][3]; QRows(v.pose.orientation, R);
    float c = cosf(-g_rcYaw), sn = sinf(-g_rcYaw);   // rotate by -yaw about +y: (x, z) -> (c x + s z, -s x + c z)
    auto ry = [&](const float* in, float* out) { out[0] = c * in[0] + sn * in[2]; out[1] = in[1]; out[2] = -sn * in[0] + c * in[2]; };
    memset(E, 0, 64);
    for (int r = 0; r < 3; r++) ry(R[r], &E[r * 4]);
    float mid[3] = { 0.5f * (v.pose.position.x + other.pose.position.x), 0.5f * (v.pose.position.y + other.pose.position.y), 0.5f * (v.pose.position.z + other.pose.position.z) };
    float p[3], pr[3];   // eye offset from the head centre, plus the head's movement from the recentre point when Positional=1
    for (int k = 0; k < 3; k++) p[k] = (&v.pose.position.x)[k] - mid[k] + (g_positional ? mid[k] - g_rcPos[k] : 0);
    ry(p, pr);
    for (int k = 0; k < 3; k++) E[12 + k] = pr[k] * g_worldScale;
    E[15] = 1;
}
static void ProjFromFov(const XrFovf& f, const float* game, float* P) {
    float l = tanf(f.angleLeft), r = tanf(f.angleRight), u = tanf(f.angleUp), d = tanf(f.angleDown);
    memcpy(P, game, 64);
    P[0] = 2 / (r - l); P[1] = 0; P[2] = 0; P[3] = 0;
    P[4] = 0; P[5] = 2 / (u - d); P[6] = 0; P[7] = 0;
    P[8] = (r + l) / (r - l); P[9] = (u + d) / (u - d);
}
// build 008: shared state for the view-rebuild and shadow hooks
static volatile int g_curEye = -1;
// build 012: the shadow collector (and likely other screen-space passes) rebuilds positions from depth as if the projection
// were symmetric, ignoring the off-centre terms of the headset's asymmetric eye FOV -> shadows misplaced differently per
// eye and moving with the head. SymFov=1 renders each eye with a symmetric frustum that contains its FOV.
static volatile int g_symFov = 1;
static volatile bool g_inEye = false;          // inside our per-eye render callback
static float g_shadowV[16], g_shadowP[16];     // centre-eye camera (head pose, both eyes' FOV) used for the shadow cascades of both eyes
static bool g_haveShadowCam = false;
static volatile int g_shadowFix = 1; static volatile int g_cullFix = 2;
static volatile float g_shadowScale = 1.0f;   // 114 test: the shadow camera's tangents = both eyes' FOV x this (Home)
static float g_cullScale = 1.25f;              // the culling frustum's tangents = both eyes' FOV x this
static XrView g_lastViews[2]; static bool g_haveLast = false; static volatile LONG g_poseReuse;
static void UnionFov(const XrView v[2], float scale, float& l, float& r, float& u, float& d) {
    l = fminf(tanf(v[0].fov.angleLeft), tanf(v[1].fov.angleLeft)) * scale;
    r = fmaxf(tanf(v[0].fov.angleRight), tanf(v[1].fov.angleRight)) * scale;
    u = fmaxf(tanf(v[0].fov.angleUp), tanf(v[1].fov.angleUp)) * scale;
    d = fminf(tanf(v[0].fov.angleDown), tanf(v[1].fov.angleDown)) * scale;
}
static void ProjFromTan(float l, float r, float u, float d, const float* game, float* P) {
    memcpy(P, game, 64);
    P[0] = 2 / (r - l); P[1] = P[2] = P[3] = 0; P[4] = 0; P[5] = 2 / (u - d); P[6] = P[7] = 0;
    P[8] = (r + l) / (r - l); P[9] = (u + d) / (u - d);
}
// build 010: CullFix=2 - the game camera itself holds the head-turned, widened culling camera outside the eyes (written right
// after the game's camera update, 0x1400b9a90 SetView), so everything the game update reads (NPC LOD / on-screen tests,
// several V*P users found by the F9 census) covers the headset's view. The eyes are built from the saved real game camera.
static float g_gameV[16], g_gameVi[16], g_gameP[16], g_cullV[16];
static volatile bool g_cullValid = false; static volatile int g_cullRestore = 0; static volatile bool g_haveGameV = false;
static uint32_t g_faS[2], g_faE[2], g_faMin = 0xffffffff; static LONG g_faSecond, g_faEmpty, g_faReset; static uint64_t g_faSz[3];
extern volatile int g_rainFix; extern volatile LONG g_hmFixed, g_hmSeen;
static bool g_dev = false;   // 088: DevTools=1 in sdvr.ini - test keys, diagnostic hooks and census logging
static bool g_spatchOk = false;   // set when the VR mod itself started (Enabled=1, right exe)
volatile int g_stateFix = 0; volatile LONG g_stateGen = 1;
extern volatile int g_geoTest; static LONG g_sdCalls[3], g_sdPass[3], g_sdWhite[3]; static void* g_sdView[3]; static void* g_sdState[3]; static LONG g_sdFrame[3], g_sdFrameLast[3]; static LONG g_geoCalls[3], g_geoItems[3], g_fadeCalls[3][3]; static void* g_geoView[3]; static void* g_fadeView[3];   // build 063 (see hkFlush)
static volatile int g_cullKeepInv = 0;   // build 039: CullFix 2 without touching the camera's inverse (world) matrix - F5 mode 4
static void CullCamera(const float* V, const float* P, float* Vc, float* Pc) {
    float E[16], E1[16], Ei[16]; EyeInCamera(g_lastViews[0], g_lastViews[1], E); EyeInCamera(g_lastViews[1], g_lastViews[0], E1);
    for (int k = 12; k < 15; k++) E[k] = 0.5f * (E[k] + E1[k]);
    InvRigid(E, Ei); Mul4(V, Ei, Vc);
    float l, r, u, d; UnionFov(g_lastViews, g_cullScale, l, r, u, d); ProjFromTan(l, r, u, d, P, Pc);
}
static float* MainCamera() {
    uintptr_t mgr = At<uintptr_t>(0x2173df0), obj = mgr ? *(uintptr_t*)(mgr + 0x50) : 0;
    return obj ? (float*)(obj + 0x50) : nullptr;
}
typedef void (__fastcall *SetView_t)(float*, float);
static SetView_t oSetView;
static void __fastcall hkSetView(float* cam, float f) {
    oSetView(cam, f);
    if (g_cullFix != 2 || g_inEye || !g_haveLast || cam != MainCamera() || *(uint8_t*)((uint8_t*)cam + 0x100)) return;
    float* V = cam + 0x40 / 4, *Vi = cam + 0x80 / 4, *P = cam + 0xC0 / 4;
    memcpy(g_gameV, V, 64); memcpy(g_gameVi, Vi, 64); memcpy(g_gameP, P, 64); g_haveGameV = true;
    float Vc[16], Pc[16]; CullCamera(g_gameV, g_gameP, Vc, Pc);
    memcpy(V, Vc, 64); if (!g_cullKeepInv) InvRigid(Vc, Vi); memcpy(P, Pc, 64); memcpy(g_cullV, Vc, 64);
    g_cullValid = true;
    LOG_FIRST(1, "cull: the game camera holds the head-turned culling camera outside the eyes (CullFix 2)");
}
static volatile int g_dumpLeft = 0;
static void LogMat(const char* what, const float* m) {
    Log("  %s: %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.4f %.4f %.4f %.4f | %.3f %.3f %.3f %.4f", what,
        m[0], m[1], m[2], m[3], m[4], m[5], m[6], m[7], m[8], m[9], m[10], m[11], m[12], m[13], m[14], m[15]);
}
// build 023: FrontEndPauseEffect (pause / front-end blurred background). Its update 0x1400611f0 steps the blur level at
// +0x58 (0..20) toward the target +0x5c; level > 0 = a menu with the blurred scene behind it.
static volatile int g_menuLevel = 0; static volatile int g_menuMode = 1;
// build 024: front end / start-up screens: the game camera sits at the world origin (translation ~0) while no level is
// shown (title screen, logo screens); in the game world it is hundreds of metres out. Treated like the pause menu.
static volatile bool g_frontEnd = false;
static volatile int g_camCap = -1; static float g_camStep[240], g_camDt[240];
typedef void (__fastcall *FxUpd_t)(uint8_t*);
static FxUpd_t oFxUpd;
static void __fastcall hkFxUpd(uint8_t* fx) {
    oFxUpd(fx);
    int lv = fx ? *(int*)(fx + 0x58) : 0;
    if ((lv > 0) != (g_menuLevel > 0)) Log("menu background %s (blur level %d)", lv > 0 ? "on" : "off", lv);
    g_menuLevel = lv;
}
// build 028: frame sequence numbers travel with the game's eye-copy render command {eye, 0x14069ea60}: the queue function
// (0x140061720, game thread) puts the sequence number in the upper half of the eye argument, the copy callback (render
// thread) takes it out (the callback only uses the low 32 bits). Also a census of the render commands queued in the eyes.
static volatile long g_curSeq = 0;
struct CmdSite { uintptr_t fn; int key; LONG n; };
static CmdSite g_cmds[64]; static LONG g_nCmds; static SRWLOCK g_cmdLock = SRWLOCK_INIT;
typedef void (__fastcall *Queue_t)(void*, uint32_t, uint32_t, uintptr_t*);
static Queue_t oQueue;
static void __fastcall hkQueue(void* list, uint32_t key, uint32_t w, uintptr_t* rec) {
    if (rec && g_inEye) {
        uintptr_t fn = rec[1];
        if (fn == g_base + 0x69ea60 && g_curSeq > 0) rec[0] = (uint32_t)rec[0] | ((uintptr_t)((uint32_t)g_curSeq | 0x80000000u) << 32);
        if (!g_dev) return oQueue(list, key, w, rec);   // 088: the command census is a test tool
        AcquireSRWLockExclusive(&g_cmdLock);
        LONG j = 0; for (; j < g_nCmds; j++) if (g_cmds[j].fn == fn && g_cmds[j].key == (int)(key & 0xffff)) break;
        if (j == g_nCmds && g_nCmds < 64) { g_cmds[j].fn = fn; g_cmds[j].key = key & 0xffff; g_cmds[j].n = 0; g_nCmds++; }
        if (j < 64) g_cmds[j].n++;
        ReleaseSRWLockExclusive(&g_cmdLock);
    }
    oQueue(list, key, w, rec);
}

// build 041: objective markers (0x1406155e0) project their world position with the game camera's view (0x1400b9d40)
// and a perspective of their own (0x140188bd0: the camera FOV, aspect 16:9) onto the full render target, which Scaleform
// maps into its stage. With CullFix the view holds the head-turned culling camera, so the markers turned with the head;
// and the frustum did not match the HUD panel. For that one caller: the real game view, and a frustum that is exactly the
// world-locked HUD panel as seen from the head (full HUD texture: HudWidth wide at HudDistance), so a marker on the panel
// lies on the line from the head to its target.
typedef float* (__fastcall *CamView_t)(void*);
static CamView_t oCamView; static float g_markV[16]; static int g_markerFix = 1;
static volatile LONG g_mkView, g_mkViewSwap, g_mkViewOther, g_mkPersp;
static float* __fastcall hkCamView(void* cam) {
    float* r = oCamView(cam);
    if ((uintptr_t)__builtin_return_address(0) != g_base + 0x615a34) return r;
    InterlockedIncrement(&g_mkView);
    // 042: no exact-match test (041 compared with the culling view and evidently never swapped): whenever the main
    // camera is in use and CullFix has a saved real view, the markers get that
    if (g_markerFix && g_cullFix == 2 && g_haveGameV && (float*)cam == MainCamera()) { InterlockedIncrement(&g_mkViewSwap); memcpy(g_markV, g_gameV, 64); return g_markV; }
    InterlockedIncrement(&g_mkViewOther);
    return r;
}
typedef void (__fastcall *Persp_t)(float*, float, float, float, float);
static Persp_t oPersp;
static void __fastcall hkPersp(float* dst, float fovY, float aspect, float zn, float zf) {
    if (!g_markerFix || (uintptr_t)__builtin_return_address(0) != g_base + 0x615a18) { oPersp(dst, fovY, aspect, zn, zf); return; }
    InterlockedIncrement(&g_mkPersp);
    float tx, ty, ty0; XrHudTangents(&tx, &ty, &ty0);
    oPersp(dst, 2.f * atanf(ty), tx / ty, zn, zf);
    if (ty0 != 0.f) { float k = ty0 / ty; dst[9] -= k * dst[11]; dst[13] -= k * dst[15]; }   // panel centre above / below eye level
    LOG_FIRST(1, "markers: objective marker projection uses the game camera and the HUD panel frustum (tan %.3f x %.3f)", tx, ty);
}

// build 048: volumetric fog / light-scattering update (0x14006c6f0, per render view, game thread). It is time-sliced:
// globals 0x142143344 / 0x142143348 advance on every call and pick the ring buffer ([tod+0xe0] entries at tod+0xe8)
// and the depth slices to refresh (tod+0x108 offset, tod+0x110 interval). With two eye passes per frame the counters
// advance twice, so the first eye pass always lands on the same buffer / slice phase - candidate for the unlit
// lanterns / signs in the first eye. FogSync (Ctrl+F9 cycles): 0 = game as is, 1 = the second eye pass replays the
// first pass's counter state (one advance per frame, as in flat), 2 = DE volumetric fog intensity 0 (test only).
typedef void* (__fastcall *FogUpd_t)(void*, void*, uint8_t, uint8_t);
static FogUpd_t oFogUpd; static volatile int g_fogSync = 0;
static uint32_t g_fogPre[3], g_fogPost[3]; static bool g_fogHavePre; static volatile LONG g_fogCalls[3];
static void* __fastcall hkFogUpd(void* tod, void* a, uint8_t b, uint8_t c) {
    uint32_t* c44 = (uint32_t*)(g_base + 0x2143344); uint32_t* c48 = (uint32_t*)(g_base + 0x2143348); uint32_t* off = (uint32_t*)((uint8_t*)tod + 0x108);
    int eye = g_curEye; InterlockedIncrement(&g_fogCalls[eye == 0 ? 0 : eye == 1 ? 1 : 2]);
    if (g_fogSync == 2) *(float*)(g_base + 0x2163804) = 0.f;
    if (g_fogSync == 1 && eye == 1 && g_fogHavePre) {
        *c44 = g_fogPre[0]; *c48 = g_fogPre[1]; *off = g_fogPre[2];
        void* r = oFogUpd(tod, a, b, c);
        *c44 = g_fogPost[0]; *c48 = g_fogPost[1]; *off = g_fogPost[2];
        return r;
    }
    if (eye == 0) { g_fogPre[0] = *c44; g_fogPre[1] = *c48; g_fogPre[2] = *off; }
    void* r = oFogUpd(tod, a, b, c);
    if (eye == 0) { g_fogPost[0] = *c44; g_fogPost[1] = *c48; g_fogPost[2] = *off; g_fogHavePre = true; }
    LOG_FIRST(1, "fog: volumetric update ring %u entries, slice interval %u", *(uint32_t*)((uint8_t*)tod + 0xe0), *(uint32_t*)((uint8_t*)tod + 0x110));
    return r;
}
static void HudBeginAfterEyeCopy();
typedef void (__fastcall *EyeCopy_t)(uintptr_t);
static EyeCopy_t oEyeCopy;
static void __fastcall hkEyeCopy(uintptr_t arg) {
    uint32_t hi = (uint32_t)(arg >> 32);
    if (hi & 0x80000000u) { XrSetPresentSeq((long)(hi & 0x7fffffffu)); DlssSetSeq((long)(hi & 0x7fffffffu)); }
    RtOutlineMark((uint32_t)arg ? "eye copy (eye 1?)" : "eye copy (eye 0?)");
    oEyeCopy((uint32_t)arg);
    RtOutlineMark("eye copy end");
    HudBeginAfterEyeCopy();
}
static void LogCmds() {
    AcquireSRWLockShared(&g_cmdLock);
    Log("render commands queued inside the eyes (key | function | count):");
    for (LONG j = 0; j < g_nCmds; j++) Log("  0x%x | %s+0x%llx | %ld", g_cmds[j].key, g_cmds[j].fn >= g_base && g_cmds[j].fn < g_base + 0x3000000 ? "" : "abs ",
        (unsigned long long)(g_cmds[j].fn >= g_base ? g_cmds[j].fn - g_base : g_cmds[j].fn), g_cmds[j].n);
    ReleaseSRWLockShared(&g_cmdLock);
}
// ---------------------------------------------------------------------------
// build 124 (prototype): first-person view. The eye camera sits at Wei's head bone instead of at the game's third-person
// camera; the direction is the game camera's heading with the horizon levelled (the right stick still turns the view,
// the headset looks around from there). Because the head bone goes with the character, it carries into cars and onto
// motorcycles. The bone is read with the game's own routines (found from its "Bip01 Head" lookups):
//   player SimObject  = [base + 0x235C488]
//   animation comp.   = 0x190A60(simObject, [base + 0x208A040])      component of that type
//   creature          = [component + 0x98];  skeleton = [creature + 0x1E0] when [creature + 0x1B8] is set
//   bone index        = 0x1B94E0(skeleton, 0x18BB00("Bip01 Head", -1))
//   world transform   = 0x3A94E0(creature, bone, &matrix)            position in row 3
// World up is +z. When the head is not within FpMaxDist of the game camera (cutscene cameras elsewhere, no player) the
// game's camera is used as before.
static volatile int g_fp = 0; static float g_fpFwd = 0.10f, g_fpUp = 0.07f, g_fpMaxDist = 15.f;
static bool SafeRd(uintptr_t at, void* out, size_t n) { SIZE_T got = 0; return at > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (void*)at, out, n, &got) && got == n; }
static bool FpHeadPos(float* pos) {
    uintptr_t so = 0, comp, creature = 0, rig = 0, skel = 0; uint32_t type = At<uint32_t>(0x208a040); uint8_t probe[0x70];
    if (!type || !SafeRd(g_base + 0x235C488, &so, 8) || !SafeRd(so, probe, sizeof probe)) return false;
    comp = (uintptr_t)((void* (*)(void*, uint32_t))(g_base + 0x190a60))((void*)so, type);
    if (!comp || !SafeRd(comp + 0x98, &creature, 8) || !SafeRd(creature + 0x1b8, &rig, 8) || !rig || !SafeRd(creature + 0x1e0, &skel, 8) || !SafeRd(skel, probe, 0x18)) return false;
    static uint32_t sym = 0; if (!sym) sym = ((uint32_t (*)(const char*, uint32_t))(g_base + 0x18bb00))("Bip01 Head", 0xFFFFFFFFu);
    static uintptr_t lastSkel = 0; static int bone = -1;
    if (skel != lastSkel) { bone = ((int (*)(void*, uint32_t))(g_base + 0x1b94e0))((void*)skel, sym); lastSkel = skel; Log("first person: player creature %p skeleton %p head bone %d", (void*)creature, (void*)skel, bone); }
    if (bone < 0) return false;
    alignas(16) float m[16] = {};
    ((void (*)(void*, int, float*))(g_base + 0x3a94e0))((void*)creature, bone, m);
    for (int k = 0; k < 3; k++) { if (!(m[12 + k] == m[12 + k]) || fabsf(m[12 + k]) > 1e6f) return false; pos[k] = m[12 + k]; }
    return true;
}
// the game's view matrix (world -> camera, row vectors) rebuilt for the head position; false = keep the game's
static bool FpView(const float* src, float* out) {
    if (!g_fp || (g_menuMode && (g_menuLevel > 0 || g_frontEnd))) return false;
    float cp[3]; for (int k = 0; k < 3; k++) cp[k] = -(src[12] * src[k * 4 + 0] + src[13] * src[k * 4 + 1] + src[14] * src[k * 4 + 2]);   // game camera position
    if (fabsf(cp[0]) < 0.5f && fabsf(cp[1]) < 0.5f && fabsf(cp[2]) < 0.5f) return false;                                              // front-end screens
    float hp[3]; static LONG st = -1;
    bool ok = FpHeadPos(hp);
    float dd = ok ? sqrtf((hp[0] - cp[0]) * (hp[0] - cp[0]) + (hp[1] - cp[1]) * (hp[1] - cp[1]) + (hp[2] - cp[2]) * (hp[2] - cp[2])) : 0.f;
    if (ok && dd > g_fpMaxDist) ok = false;
    if ((LONG)ok != st) { st = ok; Log("first person: %s (head %.2f %.2f %.2f, game camera %.2f %.2f %.2f, %.2f m apart)", ok ? "head camera in use" : "game camera in use", hp[0], hp[1], hp[2], cp[0], cp[1], cp[2], dd); }
    if (!ok) return false;
    // The game's camera space is the headset's (x right, y up, z BACK - the eye matrices are applied to it directly), so
    // column 2 of the view matrix is the camera's back axis in the world and the camera looks along minus that.
    float r[3] = { src[0], src[4], src[8] }, u[3] = { src[1], src[5], src[9] }, b[3] = { src[2], src[6], src[10] };
    float un[3] = { 0, 0, u[2] >= 0 ? 1.f : -1.f };                                    // level: up = world up
    float bh[3] = { b[0], b[1], 0 }; float bl = sqrtf(bh[0] * bh[0] + bh[1] * bh[1]);
    static float keep[3] = { 1, 0, 0 };
    if (bl < 0.05f) memcpy(bh, keep, 12); else { bh[0] /= bl; bh[1] /= bl; memcpy(keep, bh, 12); }   // looking straight up / down: keep the last heading
    float rn[3] = { un[1] * bh[2] - un[2] * bh[1], un[2] * bh[0] - un[0] * bh[2], un[0] * bh[1] - un[1] * bh[0] };   // right = up x back
    if (rn[0] * r[0] + rn[1] * r[1] + rn[2] * r[2] < 0) { rn[0] = -rn[0]; rn[1] = -rn[1]; rn[2] = -rn[2]; }
    float ps[3]; for (int k = 0; k < 3; k++) ps[k] = hp[k] - bh[k] * g_fpFwd + un[k] * g_fpUp;   // a little forward of and above the bone: the eyes
    memset(out, 0, 64);
    for (int k = 0; k < 3; k++) { out[k * 4 + 0] = rn[k]; out[k * 4 + 1] = un[k]; out[k * 4 + 2] = bh[k]; }
    for (int j = 0; j < 3; j++) out[12 + j] = -(ps[0] * out[0 + j] + ps[1] * out[4 + j] + ps[2] * out[8 + j]);
    out[15] = 1;
    return true;
}
static void hkEyeFrameInner(float a, int eye) {
    if (eye == 0) {   // 136: the game thread's own frame time, for the long-frame notes
        extern volatile float g_gameFrameMs; static LARGE_INTEGER pq, qf; LARGE_INTEGER q; QueryPerformanceCounter(&q); if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        if (pq.QuadPart) g_gameFrameMs = (float)(q.QuadPart - pq.QuadPart) * 1000.f / (float)qf.QuadPart;
        pq = q;
    }
    if (eye == 0 || eye == 1) InterlockedIncrement64(&g_eyeFrames[eye]);
    g_lastSep = At<float>(rva::EyeSeparation);
    At<int>(0x243a31c) = 0;   // Option3DConvergence: no sideways image shift when the eye images are made
    void* cb = At<void*>(0x2129980); void* ud = At<void*>(0x21299d8);
    uintptr_t mgr = At<uintptr_t>(0x2173df0), obj = mgr ? *(uintptr_t*)(mgr + 0x50) : 0;
    static XrView views[2]; static bool have;
    if (eye == 0) {
        have = g_track && XrLocateEyes(views);
        if (g_track && !have && g_haveLast) { memcpy(views, g_lastViews, sizeof views); have = true; InterlockedIncrement(&g_poseReuse); }   // build 033: never fall back to the game's own camera
        if (have) {
            {   // 129: automatic recentre at start. The first one happens on the very first tracked frame, which can come before the
                // headset reports a settled position (the menus and logos then sit low or off to one side until a restart).
                // Repeat it 1, 3 and 6 seconds in, while the logos are still showing.
                static ULONGLONG t0 = 0; static int step = 0; static const ULONGLONG kAt[3] = { 1000, 3000, 6000 };
                ULONGLONG now = GetTickCount64(); if (!t0) t0 = now;
                if (g_autoRecenter && step < 3 && now - t0 >= kAt[step]) { step++; g_recenterReq = true; Log("automatic recentre %d of 3 (%.0f s after the first tracked frame)", step, (double)(now - t0) / 1000.0); }
            }
            if (g_recenterReq) { g_recenterReq = false; Recenter(views); }
            if (g_symFov) for (int e = 0; e < 2; e++) {   // build 012: symmetric frustum per eye, covering the eye's own FOV
                XrFovf& f = views[e].fov;
                float mx = fmaxf(fabsf(tanf(f.angleLeft)), fabsf(tanf(f.angleRight))), my = fmaxf(fabsf(tanf(f.angleUp)), fabsf(tanf(f.angleDown)));
                f.angleLeft = -atanf(mx); f.angleRight = atanf(mx); f.angleUp = atanf(my); f.angleDown = -atanf(my);
            }
            XrSetMenuMode(g_menuMode && (g_menuLevel > 0 || g_frontEnd));
            g_curSeq = XrSetFrameViews(views);   // the runtime gets the same FOV the eye is rendered with
            memcpy(g_lastViews, views, sizeof views); g_haveLast = true;
        }
    }
    if (!have || !cb || !obj || eye < 0 || eye > 1) { oEyeFrame(a, eye); return; }
    At<int>(rva::EyeIndex) = eye;
    float* cam = (float*)(obj + 0x50);
    float* V = cam + 0x40 / 4, *Vi = cam + 0x80 / 4, *P = cam + 0xC0 / 4;
    const float* src = *(uint8_t*)((uint8_t*)cam + 0x100) ? cam + 0x110 / 4 : V;
    float sV[16], sVi[16], sP[16];
    memcpy(sV, V, 64); memcpy(sVi, Vi, 64); memcpy(sP, P, 64);
    const float* gP = sP;
    if (g_cullValid && !memcmp(V, g_cullV, 64)) { src = g_gameV; gP = g_gameP; }   // the camera holds the culling camera: use the real one
    static float fpV[2][16]; static bool fpOk = false;   // 124: first person - one head read per frame (first eye), the same camera for both eyes
    if (eye == 0) fpOk = FpView(src, fpV[0]);
    if (fpOk) src = fpV[0];
    float E[16], Ei[16], Ve[16], Pe[16];
    int pe = g_swapOrder ? 1 - eye : eye;   // which headset eye this pass renders
    EyeInCamera(views[pe], views[1 - pe], E); InvRigid(E, Ei);
    Mul4(src, Ei, Ve);
    ProjFromFov(views[pe].fov, gP, Pe);
    if (eye == 0) {
        bool fe = fabsf(src[12]) < 0.5f && fabsf(src[13]) < 0.5f && fabsf(src[14]) < 0.5f;
        if (fe != g_frontEnd) Log("front-end screen %s (game camera at %.2f %.2f %.2f)", fe ? "on" : "off", src[12], src[13], src[14]);
        g_frontEnd = fe;
    }
    if (eye == 0) {   // build 032: camera motion per frame (F9 with the timing capture): world position step in mm and frame time
        static float prev[3]; static LARGE_INTEGER pt, qf; LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
        float pos[3]; for (int k = 0; k < 3; k++) pos[k] = -(src[12] * src[k * 4 + 0] + src[13] * src[k * 4 + 1] + src[14] * src[k * 4 + 2]);
        float d = sqrtf((pos[0] - prev[0]) * (pos[0] - prev[0]) + (pos[1] - prev[1]) * (pos[1] - prev[1]) + (pos[2] - prev[2]) * (pos[2] - prev[2]));
        float dt = pt.QuadPart ? (float)(t.QuadPart - pt.QuadPart) * 1000.f / (float)qf.QuadPart : 0.f;
        memcpy(prev, pos, 12); pt = t;
        if (g_camCap >= 0 && g_camCap < 240) { g_camStep[g_camCap] = d * 1000.f; g_camDt[g_camCap] = dt; if (++g_camCap == 240) {
            Log("camera: 240 game frames - camera step mm / frame time ms");
            char line[1024]; int o = 0;
            for (int i = 0; i < 240; i++) { o += snprintf(line + o, sizeof line - o, " %.0f/%.1f", g_camStep[i], g_camDt[i]); if (i % 24 == 23) { Log("  %s", line); o = 0; } }
            g_camCap = -1; } }
    }
    bool menu = g_menuMode && (g_menuLevel > 0 || g_frontEnd);
    if (menu) {   // mono from the game camera, fixed symmetric FOV matching the world-locked menu screen
        memcpy(Ve, src, 64);
        float t = XrMenuTan(), ty = t * (float)At<int>(0x2439a98) / (float)(At<int>(0x2439a94) ? At<int>(0x2439a94) : 1);
        ProjFromTan(-t, t, ty, -ty, gP, Pe);
    }
    {   // 079: DLSS gets the camera of this eye pass; with jitter on, the projection is shifted by a sub-pixel offset
        float jx, jy; DlssJitter(g_curSeq, &jx, &jy);
        DlssSetCamera(g_curSeq, eye, Ve, Pe, jx, jy);
        if ((jx != 0.f || jy != 0.f) && g_rw > 0 && g_rh > 0) { Pe[8] += Pe[11] * 2.f * jx / (float)g_rw; Pe[9] -= Pe[11] * 2.f * jy / (float)g_rh; }
    }
    memcpy(V, Ve, 64); InvRigid(Ve, Vi); memcpy(P, Pe, 64);
    if (eye == 0) {   // centre-eye camera for the shadow cascades: both eyes then use the same shadow setup
        float Ec[16], E1[16], Eci[16]; EyeInCamera(views[0], views[1], Ec); EyeInCamera(views[1], views[0], E1);
        for (int k = 12; k < 15; k++) Ec[k] = 0.5f * (Ec[k] + E1[k]);
        InvRigid(Ec, Eci); Mul4(src, Eci, g_shadowV);
        float l, r, u, d; UnionFov(views, g_shadowScale, l, r, u, d); ProjFromTan(l, r, u, d, gP, g_shadowP);
        if (menu) { memcpy(g_shadowV, Ve, 64); memcpy(g_shadowP, Pe, 64); }
        g_haveShadowCam = true;
    }
    if (g_eyeFrames[eye] <= 2 || g_eyeFrames[eye] % 1800 == 0)
        Log("eye %d: view row3 %.3f %.3f %.3f (game %.3f %.3f %.3f), proj %.3f %.3f %.3f %.3f (game %.3f %.3f)", eye, Ve[12], Ve[13], Ve[14], src[12], src[13], src[14],
            Pe[0], Pe[5], Pe[8], Pe[9], gP[0], gP[5]);
    if (false) { g_dumpLeft = 3; Log("dump: frame of eye frame %lld", g_eyeFrames[0]); }
    if (g_dumpLeft > 0) { char w[32]; snprintf(w, sizeof w, "eye %d view", eye); LogMat(w, Ve); snprintf(w, sizeof w, "eye %d proj", eye); LogMat(w, Pe); }
    g_curEye = eye; g_inEye = true;
    ((void (*)(float, void*))cb)(a, ud);
    g_inEye = false; g_curEye = -1;
    memcpy(V, sV, 64); memcpy(Vi, sVi, 64); memcpy(P, sP, 64);
    // build 030: after the last eye, give the game its real camera back (the culling camera written after SetView stayed
    // in the camera until the next update; the camera code reads it again for smoothing -> the view jumped between two points)
    if (eye == 1 && g_cullRestore && g_cullValid && !memcmp(V, g_cullV, 64)) { memcpy(V, g_gameV, 64); memcpy(Vi, g_gameVi, 64); memcpy(P, g_gameP, 64); g_cullValid = false; }
}
// RenderStageShadow update: inside an eye, fit the cascades to the centre-eye camera instead of the eye's own view, so both
// eyes get the same shadow maps and cascade transforms (build 007 showed shadows desynced between the eyes)
typedef void (__fastcall *ShadowUpd_t)(void*, uint8_t*, float, float);
static ShadowUpd_t oShadowUpd;
// build 009: ShadowFix modes (F6 cycles them): 0 = each eye fits its own cascades (007), 1 = centre-eye camera for both (008),
// 2 = only eye 0 updates (eye 1 reuses eye 0's), 3 = only eye 1 updates (eye 0 keeps the game-update one)

// build 010: 4 = no update in the eyes (the game-update one is used by both), 5 = frozen (no updates at all)
static void DumpShadow(const char* when, void* stage, uint8_t* view) {
    if (g_dumpLeft <= 0) return;
    Log("dump %s (%s), mode %d:", when, g_inEye ? (g_curEye ? "eye 1" : "eye 0") : "game update", g_shadowFix);
    if (view) LogMat("view+0x3f0", (float*)(view + 0x3f0));
    for (int c = 0; c < 2; c++) {
        char w[32]; snprintf(w, sizeof w, "cascade %d A", c); LogMat(w, (float*)((uint8_t*)stage + 0x300 + c * 0x90));
        snprintf(w, sizeof w, "cascade %d B", c); LogMat(w, (float*)((uint8_t*)stage + 0x340 + c * 0x90));
    }
}
static void __fastcall hkShadowUpd(void* stage, uint8_t* view, float x1, float a) {
    struct D { void* s; uint8_t* v; ~D() { DumpShadow("after shadow update", s, v); if (!g_inEye && g_dumpLeft > 0) g_dumpLeft = 0; else if (g_dumpLeft > 0) g_dumpLeft--; } } dmp{ stage, view };
    if (g_shadowFix == 5) return;
    if (g_inEye && g_shadowFix == 4) return;
    if (g_inEye && g_shadowFix == 2 && g_curEye == 1) return;
    if (g_inEye && g_shadowFix == 3 && g_curEye == 0) return;
    if (g_shadowFix != 1 || !g_inEye || !g_haveShadowCam || !view) { oShadowUpd(stage, view, x1, a); return; }
    float save[32]; memcpy(save, view + 0x3f0, 128);
    memcpy(view + 0x3f0, g_shadowV, 64); memcpy(view + 0x430, g_shadowP, 64);
    oShadowUpd(stage, view, x1, a);
    memcpy(view + 0x3f0, save, 128);
    LOG_FIRST(1, "shadow: cascades fitted to the centre-eye camera");
}
// build 118: radial sun cascades. The game fits each cascade as the bounding sphere of a slice (split n..f) of a frustum
// built from ONE scalar FOV + aspect (0x140038310): centre = camera + forward * c, c = (n+f)/2 * (1 + tan^2 + tan'^2).
// The sphere sits AHEAD of the camera and only spans the flat game's FOV, so in the headset anything toward the side of the
// view lies outside it and its shadow switches off as the head turns. Radial: centre the sphere on the camera with radius
// = the cascade's far distance x ShadowRadialScale - shadows then cover that distance in every direction and no longer
// depend on where the head points. Done through the function's own inputs (FOV 0, splits -R..+R => c = 0, r = R), so its
// matrices, texel snapping and everything derived from the sphere stay the game's own work.
typedef void (__fastcall *CascFit_t)(uint8_t*, void*, float*, float, float, void*, void*, float*, void*, void*, float, float, int);
static CascFit_t oCascFit;
static volatile int g_shadowRadial = 1; static volatile float g_shadowRadialScale = 1.5f;
static void __fastcall hkCascFit(uint8_t* stage, void* view, float* cam, float fov, float aspect, void* A, void* B, float* sph, void* p40, void* p48, float f50, float f58, int idx) {
    static LONG nlog = 0;
    float* sp = (float*)(stage + 0x550);
    if (!g_shadowRadial || idx < 0 || idx > 3) {
        oCascFit(stage, view, cam, fov, aspect, A, B, sph, p40, p48, f50, f58, idx);
        if (nlog < 12 && InterlockedIncrement(&nlog) <= 12) Log("cascade fit %d (game): splits %.2f %.2f %.2f %.2f, fov %.3f aspect %.3f -> centre %.1f %.1f %.1f radius %.2f (camera %.1f %.1f %.1f)", idx, sp[0], sp[1], sp[2], sp[3], fov, aspect, sph[0], sph[1], sph[2], sqrtf(sph[3]), cam[12], cam[13], cam[14]);
        return;
    }
    float keep[4]; memcpy(keep, sp, 16);
    int fi = idx == 3 ? 1 : idx + 1;                 // cascade 3 reuses the first range
    float R = keep[fi] * g_shadowRadialScale;
    float c2[16]; memcpy(c2, cam, 64);
    if (idx == 2) {                                  // this one's near is forced to 0 by the game: c = f/2, so f = 2R and the camera one R back
        sp[3] = 2.f * R; c2[12] += R * c2[8]; c2[13] += R * c2[9]; c2[14] += R * c2[10];
    } else { int ni = idx == 3 ? 0 : idx; sp[ni] = -R; sp[ni + 1] = R; }
    oCascFit(stage, view, c2, 0.f, 1.f, A, B, sph, p40, p48, f50, f58, idx);
    memcpy(sp, keep, 16);
    if (nlog < 12 && InterlockedIncrement(&nlog) <= 12) Log("cascade fit %d (radial x%.2f): splits %.2f %.2f %.2f %.2f, game fov %.3f aspect %.3f -> centre %.1f %.1f %.1f radius %.2f (camera %.1f %.1f %.1f)", idx, (double)g_shadowRadialScale, keep[0], keep[1], keep[2], keep[3], fov, aspect, sph[0], sph[1], sph[2], sqrtf(sph[3]), cam[12], cam[13], cam[14]);
}
// the shadow collector's cbShadowTransform builder (AO stage, 0x140036420): when is it made, with which cascade data
typedef uintptr_t (__fastcall *ShadowCb_t)(void*, void*, void*);
static ShadowCb_t oShadowCb;
static volatile LONG64 g_shadowCbCalls[3];
static uintptr_t __fastcall hkShadowCb(void* a, void* view, void* ctx) {
    InterlockedIncrement64(&g_shadowCbCalls[g_inEye ? g_curEye : 2]);
    if (g_dumpLeft > 0) { void* st = At<void*>(0x2136370); if (st) DumpShadow("at shadow collector constants", st, (uint8_t*)view); }
    return oShadowCb(a, view, ctx);
}
// ViewRebuild outside the eyes (the frame's main view, made in the game update): turn the camera with the head and widen its
// frustum to both eyes' FOV x CullScale while the view is built, so visibility / LOD work covers what the headset can see
typedef void (*ViewRebuild_t)(float);
static ViewRebuild_t oViewRebuild;
static volatile bool g_inRebuild = false;
static void hkViewRebuild(float a) {
    struct Flag { Flag() { g_inRebuild = true; } ~Flag() { g_inRebuild = false; } } flag;
    uintptr_t mgr = At<uintptr_t>(0x2173df0), obj = mgr ? *(uintptr_t*)(mgr + 0x50) : 0;
    if (g_inEye || g_cullFix != 1 || !g_haveLast || !obj) { oViewRebuild(a); return; }
    float* cam = (float*)(obj + 0x50);
    if (*(uint8_t*)((uint8_t*)cam + 0x100)) { LOG_FIRST(2, "cull: camera override active - left as is"); oViewRebuild(a); return; }
    float* V = cam + 0x40 / 4, *Vi = cam + 0x80 / 4, *P = cam + 0xC0 / 4;
    float sV[16], sVi[16], sP[16]; memcpy(sV, V, 64); memcpy(sVi, Vi, 64); memcpy(sP, P, 64);
    float E[16], E1[16], Ei[16]; EyeInCamera(g_lastViews[0], g_lastViews[1], E); EyeInCamera(g_lastViews[1], g_lastViews[0], E1);
    for (int k = 12; k < 15; k++) E[k] = 0.5f * (E[k] + E1[k]);
    float fpC[16]; const float* baseV = FpView(sV, fpC) ? fpC : sV;   // 124: first person - cull from the head too
    InvRigid(E, Ei); float Vc[16]; Mul4(baseV, Ei, Vc);
    float l, r, u, d; UnionFov(g_lastViews, g_cullScale, l, r, u, d); float Pc[16]; ProjFromTan(l, r, u, d, sP, Pc);
    memcpy(V, Vc, 64); InvRigid(Vc, Vi); memcpy(P, Pc, 64);
    oViewRebuild(a);
    memcpy(V, sV, 64); memcpy(Vi, sVi, 64); memcpy(P, sP, 64);
    static LONG64 n = 0; if (++n <= 2 || n % 3600 == 0) Log("cull: main view built with the head-turned camera, tangents l %.2f r %.2f u %.2f d %.2f (game x %.2f y %.2f)", l, r, u, d, 1 / sP[0], 1 / sP[5]);
}
// build 015: lantern test. F2 cycles which other per-view stage update (run by the view rebuild in each eye) is skipped
// inside the eyes, so both eyes use the game-update result: 0 none, 1 EnvMap, 2 Imposters, 3 ExposureMetering, 4 all three
static volatile int g_stageSkip = 0;
typedef void (__fastcall *Stage3_t)(void*, void*, float, float);
static Stage3_t oEnvMapUpd, oImpostUpd;
static void __fastcall hkEnvMapUpd(void* st, void* v, float x, float a) { if (g_inEye && (g_stageSkip == 1 || g_stageSkip == 4)) return; oEnvMapUpd(st, v, x, a); }
static void __fastcall hkImpostUpd(void* st, void* v, float x, float a) { if (g_inEye && (g_stageSkip == 2 || g_stageSkip == 4)) return; oImpostUpd(st, v, x, a); }
typedef void (__fastcall *Exposure_t)(void*, float);
static Exposure_t oExposureUpd;
static void __fastcall hkExposureUpd(void* st, float a) { if (g_inEye && (g_stageSkip == 3 || g_stageSkip == 4)) return; oExposureUpd(st, a); }
// build 021: HUD panel. The game queues its Scaleform UI render (0x140058910, render thread) into the engine BackBuffer
// AFTER the eye copy of each eye pass, so the eye images never contain the UI. Clear the BackBuffer to transparent first,
// let the UI draw, and send the result to the OpenXR HUD quad layer.
static volatile int g_hudMode = 1;
typedef void (*UiRender_t)();
static UiRender_t oUiRender;
// build 022: the UI is drawn straight into the eye texture (LeftEye / RightEye) after the scene was copied there, so
// clearing it (021) erased the scene. Now the UI's render target is swapped for the mod's own HUD texture (same size),
// cleared to transparent; the eye images keep the scene only.
static ID3D11Texture2D* g_hudTex; static ID3D11RenderTargetView* g_hudRtv; static UINT g_hudTW, g_hudTH; static int g_glowFix = 1;
static bool EnsureHudTarget(ID3D11DeviceContext* ctx, const D3D11_TEXTURE2D_DESC& ed) {
    if (g_hudTex && g_hudTW == ed.Width && g_hudTH == ed.Height) return true;
    if (g_hudRtv) { g_hudRtv->Release(); g_hudRtv = nullptr; } if (g_hudTex) { g_hudTex->Release(); g_hudTex = nullptr; }
    ID3D11Device* dev = nullptr; ctx->GetDevice(&dev); if (!dev) return false;
    D3D11_TEXTURE2D_DESC d = {}; d.Width = ed.Width; d.Height = ed.Height; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_DEFAULT; d.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    bool ok = SUCCEEDED(dev->CreateTexture2D(&d, nullptr, &g_hudTex)) && SUCCEEDED(dev->CreateRenderTargetView(g_hudTex, nullptr, &g_hudRtv));
    dev->Release();
    if (ok) { g_hudTW = ed.Width; g_hudTH = ed.Height; Log("ui: HUD target %ux%u created", ed.Width, ed.Height); }
    RtSetHudRtv(ok ? g_hudRtv : nullptr, g_glowFix);
    return ok;
}
static bool g_hudPending; static int g_minimapFix = 1; static volatile int g_menuMapFit = 1;
static void HudFlush() {   // render thread: give the finished HUD texture (UI + minimap) to the headset
    if (!g_hudPending) return;
    g_hudPending = false;
    RtMinimapArm(nullptr);
    ID3D11DeviceContext* ctx = g_immCtx; if (!ctx || !g_hudTex) return;
    EyeCapHud(ctx, g_hudTex);
    XrHudSubmit(ctx, g_hudTex);
}
// build 038: the minimap MAP is drawn by 9 draws straight after the eye copy (no new render target bound - the eye
// texture is still bound), before the UI render; the icons come after the UI render (037). So the HUD target is bound
// right after the eye copy, with the 16:9 stage viewport, and the UI render then continues on it.
static ID3D11RenderTargetView* g_preRtv; static ID3D11DepthStencilView* g_preDsv; static bool g_hudPre;
static void HudBeginAfterEyeCopy() {
    ID3D11DeviceContext* ctx = g_immCtx;
    if (g_preRtv) { g_preRtv->Release(); g_preRtv = nullptr; } if (g_preDsv) { g_preDsv->Release(); g_preDsv = nullptr; }
    g_hudPre = false;
    if (!g_hudMode || !g_minimapFix || !ctx) return;
    HudFlush();
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Texture2D* tex = nullptr;
    if (rtv) { ID3D11Resource* r = nullptr; rtv->GetResource(&r); if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex); r->Release(); } }
    D3D11_TEXTURE2D_DESC ed = {}; if (tex) tex->GetDesc(&ed);
    if (!tex || !EnsureHudTarget(ctx, ed)) { if (tex) tex->Release(); if (rtv) rtv->Release(); if (dsv) dsv->Release(); return; }
    tex->Release();
    const float z[4] = { 0, 0, 0, 0 };
    ctx->ClearRenderTargetView(g_hudRtv, z);
    ctx->OMSetRenderTargets(1, &g_hudRtv, dsv);
    // 044: in menus (pause map) the map is laid out like the menu text, over the whole target - fit only on the gameplay HUD
    bool inMenu = g_menuLevel > 0 || g_frontEnd; int fit = inMenu ? g_menuMapFit : 1;
    RtMinimapFit(fit);
    { static int last = -1; if (fit != last) { last = fit; Log("map: radar/map draws %s (%s)", fit ? "fitted to the 16:9 HUD area" : "over the whole HUD target", inMenu ? "menu" : "gameplay"); } }
    D3D11_VIEWPORT vp = { 0, 0, (float)ed.Width, (float)ed.Height, 0, 1 }; if (fit) RtMinimapFitViewport(&vp); ctx->RSSetViewports(1, &vp);
    g_preRtv = rtv; g_preDsv = dsv; g_hudPre = true;
    RtOutlineTrace(true);
}
static void hkUiRender() {
    ID3D11DeviceContext* ctx = g_immCtx;
    RtOutlineTrace(false);
    if (!g_hudPre) HudFlush();   // the previous eye's HUD, if its minimap pass never came
    RtOutlineMark("UI render");
    if (!g_hudMode || !ctx) { oUiRender(); return; }
    ID3D11RenderTargetView* rtv = nullptr; ID3D11DepthStencilView* dsv = nullptr;
    bool pre = g_hudPre; g_hudPre = false;
    if (pre) {   // the HUD target is already bound and holds the map: continue with the UI on it
        rtv = g_preRtv; dsv = g_preDsv; g_preRtv = nullptr; g_preDsv = nullptr;
        D3D11_VIEWPORT vp = { 0, 0, (float)g_hudTW, (float)g_hudTH, 0, 1 }; ctx->RSSetViewports(1, &vp);
    } else ctx->OMGetRenderTargets(1, &rtv, &dsv);
    ID3D11Texture2D* tex = nullptr;
    if (rtv) { ID3D11Resource* r = nullptr; rtv->GetResource(&r); if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tex); r->Release(); } }
    D3D11_TEXTURE2D_DESC ed = {}; if (tex) tex->GetDesc(&ed);
    if (!tex || !EnsureHudTarget(ctx, ed)) { oUiRender(); if (tex) tex->Release(); if (rtv) rtv->Release(); if (dsv) dsv->Release(); return; }
    const float z[4] = { 0, 0, 0, 0 };
    if (!pre) ctx->ClearRenderTargetView(g_hudRtv, z);
    ctx->OMSetRenderTargets(1, &g_hudRtv, dsv);
    RtUiRedirect(g_hudRtv);
    oUiRender();
    RtUiRedirect(nullptr);
    RtOutlineMark("UI render end");
    static int logged = 0;
    if (logged < 4) {   // what did the UI render leave bound, and with which viewport?
        logged++;
        ID3D11RenderTargetView* a = nullptr; ctx->OMGetRenderTargets(1, &a, nullptr);
        D3D11_VIEWPORT vp[4]; UINT nvp = 4; ctx->RSGetViewports(&nvp, vp);
        Log("ui: after the UI render the bound target is %s, %u viewport(s) %.0f,%.0f %.0fx%.0f", a == g_hudRtv ? "the mod's HUD target" : a == rtv ? "the eye target" : a ? "another target" : "none",
            nvp, nvp ? vp[0].TopLeftX : 0.f, nvp ? vp[0].TopLeftY : 0.f, nvp ? vp[0].Width : 0.f, nvp ? vp[0].Height : 0.f);
        if (a) a->Release();
    }
    ctx->OMSetRenderTargets(1, &rtv, dsv);   // put the eye target back
    // build 037: the minimap map is drawn by the next render pass, straight into the eye image. That pass is redirected
    // to the HUD texture too (rtdiag), so the HUD is handed to the headset only after it (HudFlush)
    g_hudPending = true;
    RtMinimapArm(g_minimapFix ? g_hudRtv : nullptr);
    if (!g_minimapFix) HudFlush();
    tex->Release(); rtv->Release(); if (dsv) dsv->Release();
}

// build 033: cutscene timing probe. build 034: cutscene smoothing.
// The cutscene manager SetPlayTime(mgr, sceneTime, sync) gets the scene time once per game frame. The game advances it
// in ~21 ms steps (about 47 per second): at 72 Hz two of every three frames move the scene and the third repeats it.
// CutsceneSmooth=1 (F2) gives the manager a scene time that moves with the real frame time and is pulled gently
// towards the game's own value, so it never drifts (audio and events stay in sync). Off: the game's value as is.
typedef void (*NisSetPlayTime_t)(void*, float, bool);
static NisSetPlayTime_t oNisSetPlayTime;
static float g_nisDelta[180], g_nisDt[180]; static int g_nisN = -1;
static volatile int g_nisSmooth = 1; static volatile LONG64 g_cutsceneQpc;
static void hkNisSetPlayTime(void* mgr, float t, bool sync) {
    static float prevT, applied; static void* prevMgr; static int prevState, caps; static bool valid; static float stillT;
    static LARGE_INTEGER pq, qf; LARGE_INTEGER q; QueryPerformanceCounter(&q); if (!qf.QuadPart) QueryPerformanceFrequency(&qf);
    float dt = pq.QuadPart ? (float)(q.QuadPart - pq.QuadPart) / (float)qf.QuadPart : 0.f;
    int state = mgr ? *(int*)((uint8_t*)mgr + 0x44) : 0;
    if (state == 4) g_cutsceneQpc = q.QuadPart;
    if (state == 4 && (prevState != 4 || sync) && g_nisN < 0 && caps < 4) { caps++; g_nisN = 0; Log("cutscene: started - capturing 180 frames of its timing (smoothing %s)", g_nisSmooth ? "on" : "off"); }
    bool same = state == 4 && prevState == 4 && !sync && mgr == prevMgr && valid && t >= prevT && t - prevT < 0.25f;
    prevState = state;
    float out = t;
    if (same && g_nisSmooth) {
        stillT = (t == prevT) ? stillT + dt : 0.f;   // build 035: time-based - at 90 Hz two zero steps in a row are normal
        float step = stillT > 0.06f ? 0.f : fminf(fmaxf(dt, 0.f), 0.05f);   // the game's time stopped (pause): stop too
        float pred = applied + step, err = t - pred;
        if (fabsf(err) > 0.1f) out = t;                                   // far off (seek / hitch): take the game's value
        else { out = pred + err * 0.15f; if (out < applied) out = applied; }
        if (out < t - 0.05f) out = t - 0.05f;
    } else stillT = 0.f;
    if (same && g_nisN >= 0 && g_nisN < 180) {
        g_nisDelta[g_nisN] = (out - applied) * 1000.f; g_nisDt[g_nisN] = dt * 1000.f;
        if (++g_nisN == 180) {
            Log("cutscene: 180 frames - scene time step ms / real frame time ms (smoothing %s)", g_nisSmooth ? "on" : "off");
            char line[1024]; int o = 0;
            for (int i = 0; i < 180; i++) { o += snprintf(line + o, sizeof line - o, " %.1f/%.1f", g_nisDelta[i], g_nisDt[i]); if (i % 20 == 19) { Log("  %s", line); o = 0; } }
            g_nisN = -1;
        }
    }
    {   // 135: cutscene hitch notes - anything that is not a plain, even step (at most 400 lines per run)
        static int ev; static float cutT0; static bool was4;
        if (state == 4 && !was4) cutT0 = t;
        was4 = state == 4;
        if (g_dev && state == 4 && ev < 400 && valid) {
            float stepMs = (out - applied) * 1000.f, gameMs = (t - prevT) * 1000.f, dtMs = dt * 1000.f; const char* why = nullptr;
            if (sync) why = "game sync";
            else if (mgr != prevMgr) why = "other timeline";
            else if (!same) why = !valid ? nullptr : t < prevT ? "game time went back" : "game time jumped";
            else if (dtMs > 22.f) why = "long real frame";
            else if (fabsf(stepMs - dtMs) > 6.f && fabsf(gameMs - stepMs) > 6.f) why = "scene step differs from the frame time and from the game's";
            else { static int quiet; if (quiet > 0) quiet--; else if (fabsf(t - out) > 0.03f) { why = "behind / ahead of the game's time"; quiet = 36; } }
            if (why) { ev++; Log("cutscene note %d (%s): %.2f s into it - real frame %.1f ms, game's step %.1f ms, step given %.1f ms, game time %.3f, given %.3f", ev, why, t - cutT0, dtMs, gameMs, stepMs, t, out); }
        }
    }
    valid = state == 4; prevT = t; prevMgr = mgr; pq = q; applied = out;
    oNisSetPlayTime(mgr, out, sync);
}
typedef void (*Present_t)(int);
static Present_t oPresent;
static void hkPresent(int sync) {
    LONG64 n = InterlockedIncrement64(&g_presents);
    if (n % 600 == 1)
        Log("frame %lld: stereo on %d (requested %d, fullscreen %d, Stereo3D setting %d), eye frames %lld/%lld, composes %lld/%lld, separation %.4f, mirror %d",
            n, At<uint8_t>(rva::FlagStereoOn), At<uint8_t>(rva::FlagStereoReq), At<uint8_t>(rva::FlagFullscreen), At<uint8_t>(rva::SettingsStereo),
            g_eyeFrames[0], g_eyeFrames[1], g_composes[0], g_composes[1], g_lastSep, g_mirror);
    if (n == 601 || n == 1801 || n % 3600 == 1) HalDiagReport(n);
    {   // build 025: render target census for 120 frames, every 1800 frames, in the game world only
        static long long armedAt = -1;
        if (n == 1800 || n == 5400) LogCmds();
        if (n % 1800 == 900) Log("  ui: %ld minimap passes moved to the HUD target so far, %ld glow blend states adjusted", RtMinimapRedirects(), RtGlowFixCount());
        RtDiagNewFrame();
        HudFlush();
        { LARGE_INTEGER q, f; QueryPerformanceCounter(&q); QueryPerformanceFrequency(&f); bool cut = g_cutsceneQpc && q.QuadPart - g_cutsceneQpc < f.QuadPart / 4;
          static bool was; if (cut != was) { was = cut; Log("cutscene %s: HUD panel %s", cut ? "on" : "off", cut ? "enlarged for subtitles" : "normal size"); } XrSetHudCutscene(cut); }
        RtOutlineFrame();
        if (g_stateFix) InterlockedIncrement(&g_stateGen);
        RtDumpFrame();
        if (armedAt < 0 && n % 1800 == 900 && !g_frontEnd && g_menuLevel == 0) { RtDiagArm(true); armedAt = n; }
        else if (armedAt >= 0 && n - armedAt >= 120) { RtDiagArm(false); armedAt = -1; }
    }
    if (n % 600 == 1) Log("  render size %dx%d, fullscreen flag %d", At<int>(0x2439a94), At<int>(0x2439a98), At<uint8_t>(0x2439a42));
    if (n % 600 == 1) Log("  headset pose not located for %ld eye passes (last pose reused %ld times)", XrLocateFails(), (long)g_poseReuse);
    if (n % 600 == 1) { char b[160]; RtQueryStats(b, sizeof b); Log("  %s; shaders recorded %d", b, ShaderCount()); }
        if (n % 600 == 1) Log("  instance colours: GeoManager draws eye0 %ld (items %ld, view %p) eye1 %ld (items %ld, view %p) outside %ld (items %ld, view %p); faded-item draws [list0/1/2] eye0 %ld/%ld/%ld (view %p) eye1 %ld/%ld/%ld (view %p) outside %ld/%ld/%ld (view %p); test %d", g_geoCalls[0], g_geoItems[0], g_geoView[0], g_geoCalls[1], g_geoItems[1], g_geoView[1], g_geoCalls[2], g_geoItems[2], g_geoView[2], g_fadeCalls[0][0], g_fadeCalls[0][1], g_fadeCalls[0][2], g_fadeView[0], g_fadeCalls[1][0], g_fadeCalls[1][1], g_fadeCalls[1][2], g_fadeView[1], g_fadeCalls[2][0], g_fadeCalls[2][1], g_fadeCalls[2][2], g_fadeView[2], g_geoTest);
        for (int q = 0; q < 3; q++) { g_sdFrameLast[q] = g_sdFrame[q]; g_sdFrame[q] = 0; }
        if (n % 600 == 1) Log("  simple drawables: this frame eye0 %ld eye1 %ld outside %ld | total calls eye0 %ld (flag ok %ld, white %ld, view %p state %p) eye1 %ld (flag ok %ld, white %ld, view %p state %p) outside %ld (flag ok %ld, white %ld, view %p state %p)", g_sdFrameLast[0], g_sdFrameLast[1], g_sdFrameLast[2], g_sdCalls[0], g_sdPass[0], g_sdWhite[0], g_sdView[0], g_sdState[0], g_sdCalls[1], g_sdPass[1], g_sdWhite[1], g_sdView[1], g_sdState[1], g_sdCalls[2], g_sdPass[2], g_sdWhite[2], g_sdView[2], g_sdState[2]);
        if (n % 600 == 1) { char sb[700]; RtSceneryFixStats(sb, sizeof sb); Log("  %s", sb); }
        if (n % 600 == 1) Log("  frame allocator: eye0 start %08x end %08x, eye1 start %08x end %08x (buffers %llx / %llx / %llx); least left %u; eye ended in 2nd buffer %ld times, nearly empty %ld, reset between the eyes %ld", g_faS[0], g_faE[0], g_faS[1], g_faE[1], (unsigned long long)g_faSz[0], (unsigned long long)g_faSz[1], (unsigned long long)g_faSz[2], g_faMin, g_faSecond, g_faEmpty, g_faReset);
    if (n % 600 == 1) Log("  fog updates: eye0 %ld eye1 %ld outside the eyes %ld (sync %d)", (long)g_fogCalls[0], (long)g_fogCalls[1], (long)g_fogCalls[2], (int)g_fogSync);
    if (n % 600 == 1) Log("  markers: view calls %ld (real view given %ld, left as is %ld), perspective calls %ld", (long)g_mkView, (long)g_mkViewSwap, (long)g_mkViewOther, (long)g_mkPersp);
    if (n % 600 == 1) Log("  shadow collector constants made: eye 0 %lld, eye 1 %lld, outside the eyes %lld; shadow mode %d, cull mode %d", g_shadowCbCalls[0], g_shadowCbCalls[1], g_shadowCbCalls[2], g_shadowFix, g_cullFix);
    oPresent(sync);
    XrAfterPresent();
}

// ---------------------------------------------------------------------------
// build 009: camera reader census (F9). Hardware data breakpoints (read/write) on the game camera's projection and view
// for 2 seconds on all game threads; logs every code address that touched them, with two callers and where it happened
// (inside an eye's render callback, inside the main-view rebuild, or elsewhere = game update). Finds what the NPC LOD reads.
struct Hit { uintptr_t rip, c1, c2; LONG n[4]; LONG ctx[3]; };
static Hit g_hits[512]; static volatile LONG g_nHits = 0, g_hitLock = 0, g_hitOverflow = 0;
static volatile bool g_census = false;
static uintptr_t g_imgEnd;
static uintptr_t Rel(uintptr_t a) { return a >= g_base && a < g_imgEnd ? a - g_base : a; }
static LONG CALLBACK CensusVeh(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode != EXCEPTION_SINGLE_STEP) return EXCEPTION_CONTINUE_SEARCH;
    CONTEXT* c = e->ContextRecord; DWORD64 dr6 = c->Dr6;
    if (!(dr6 & 0xF)) return EXCEPTION_CONTINUE_SEARCH;
    c->Dr6 = 0;
    if (g_census) {
        CONTEXT u = *c; uintptr_t calls[2] = {};
        for (int i = 0; i < 2; i++) {
            DWORD64 base = 0; PRUNTIME_FUNCTION f = RtlLookupFunctionEntry(u.Rip, &base, nullptr);
            if (f) { void* hd; DWORD64 est; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, u.Rip, f, &u, &hd, &est, nullptr); }
            else { u.Rip = *(DWORD64*)u.Rsp; u.Rsp += 8; }
            calls[i] = (uintptr_t)u.Rip;
        }
        uintptr_t rip = (uintptr_t)e->ExceptionRecord->ExceptionAddress;
        int where = g_inEye ? 0 : g_inRebuild ? 1 : 2;
        while (InterlockedCompareExchange(&g_hitLock, 1, 0)) YieldProcessor();
        Hit* h = nullptr;
        for (LONG i = 0; i < g_nHits; i++) if (g_hits[i].rip == rip && g_hits[i].c1 == calls[0]) { h = &g_hits[i]; break; }
        if (!h && g_nHits < 512) { h = &g_hits[g_nHits++]; memset(h, 0, sizeof *h); h->rip = rip; h->c1 = calls[0]; h->c2 = calls[1]; }
        if (h) { for (int b = 0; b < 4; b++) if (dr6 & (1ull << b)) h->n[b]++; h->ctx[where]++; } else g_hitOverflow++;
        InterlockedExchange(&g_hitLock, 0);
    }
    return EXCEPTION_CONTINUE_EXECUTION;
}
static void SetDebugRegs(const uintptr_t* addr, int count) {
    DWORD pid = GetCurrentProcessId(), me = GetCurrentThreadId(); int done = 0;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    THREADENTRY32 te = { sizeof te };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == me) continue;
        HANDLE t = OpenThread(THREAD_GET_CONTEXT | THREAD_SET_CONTEXT | THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID);
        if (!t) continue;
        if (SuspendThread(t) != (DWORD)-1) {
            CONTEXT c = {}; c.ContextFlags = CONTEXT_DEBUG_REGISTERS;
            if (GetThreadContext(t, &c)) {
                DWORD64 dr7 = 0; DWORD64* drs[4] = { &c.Dr0, &c.Dr1, &c.Dr2, &c.Dr3 };
                for (int i = 0; i < 4; i++) {
                    *drs[i] = i < count ? addr[i] : 0;
                    if (i < count) dr7 |= (1ull << (i * 2)) | (3ull << (16 + i * 4)) | (3ull << (18 + i * 4));
                }
                c.Dr7 = dr7; c.Dr6 = 0;
                if (SetThreadContext(t, &c)) done++;
            }
            ResumeThread(t);
        }
        CloseHandle(t);
    }
    CloseHandle(snap);
    Log("census: debug registers %s on %d threads", count ? "set" : "cleared", done);
}
static void RunCensus() {
    uintptr_t mgr = At<uintptr_t>(0x2173df0), obj = mgr ? *(uintptr_t*)(mgr + 0x50) : 0;
    if (!obj) { Log("census: no camera"); return; }
    uintptr_t cam = obj + 0x50;
    uintptr_t a[4] = { cam + 0xC0, cam + 0xD4, cam + 0x48, cam + 0x70 };   // proj [0][0], proj [1][1], view [0][2], view translation x
    static PVOID veh; if (!veh) veh = AddVectoredExceptionHandler(1, CensusVeh);
    g_nHits = 0; g_hitOverflow = 0; g_census = true;
    Log("census: camera %p, watching +0xC0 +0xD4 +0x48 +0x70 for 2 s (camera override flag %d)", (void*)cam, *(uint8_t*)(cam + 0x100));
    SetDebugRegs(a, 4);
    Sleep(2000);
    SetDebugRegs(a, 0);
    Sleep(100); g_census = false;
    Log("census: %ld code sites (overflow %ld). columns: site rva | caller | caller's caller | hits on proj00 proj11 view02 viewT | in eye / in main-view rebuild / elsewhere", g_nHits, g_hitOverflow);
    for (LONG i = 0; i < g_nHits; i++) {
        Hit& h = g_hits[i];
        Log("  site +0x%llx | +0x%llx | +0x%llx | %ld %ld %ld %ld | %ld / %ld / %ld", (unsigned long long)Rel(h.rip), (unsigned long long)Rel(h.c1), (unsigned long long)Rel(h.c2),
            h.n[0], h.n[1], h.n[2], h.n[3], h.ctx[0], h.ctx[1], h.ctx[2]);
    }
    LogFlush();
}

static bool Hook(uintptr_t r, void* fn, void** orig, const char* name) {
    void* t = (void*)(g_base + r);
    MH_STATUS a = MH_CreateHook(t, fn, orig), b = a == MH_OK ? MH_EnableHook(t) : a;
    Log("hook %s (+0x%llx): %s", name, (unsigned long long)r, b == MH_OK ? "ok" : MH_StatusToString(b));
    return b == MH_OK;
}

// build 065: frame allocator watch. Per-draw parameter blocks (e.g. cbSceneryInstance, 0x1401d4770) come from a
// lock-free linear frame allocator (object at [0x142167810]; alloc 0x140177670; counter at *[obj+0x28]: low 31 bits =
// bytes left in the current buffer, top bit = second buffer in use; returns NULL when both are used up). The eyes
// submit the scene twice per frame, so this logs the counter at each eye's start and end to see whether it runs out
// or is reset between the eyes.
// build 066: the two instance-colour drawers. 0x1401d4720 (GeoManager 0x142362568: sets the cbSceneryInstance state
// param per geo and leaves it set in the view's state) and 0x1401d3d00 (0x142362520: faded/occluded items). Calls are
// counted per eye, and Ctrl+F7 cycles tests: 1 = cbSceneryInstance param cleared from the view state after the
// GeoManager draw, 2 = GeoManager draw skipped, 3 = faded-item draw skipped, 4 = both skipped.
static volatile int g_eyeCtx = -1; volatile int g_geoTest = 0;
typedef void (*GeoDraw_t)(void*, void*, int, float); static GeoDraw_t oGeoDraw;
typedef void (*FadeDraw_t)(void*, void*, float, int, int); static FadeDraw_t oFadeDraw;
static void hkGeoDraw(void* mgr, void* view, int z, float f) {
    int c = g_eyeCtx < 0 ? 2 : g_eyeCtx; InterlockedIncrement(&g_geoCalls[c]); g_geoItems[c] = (LONG)*(uint32_t*)mgr; g_geoView[c] = view;
    if (g_geoTest == 2 || g_geoTest == 4) return;
    oGeoDraw(mgr, view, z, f);
    if (g_geoTest == 1 && view) { int slot = *(int16_t*)((uint8_t*)mgr + 0x10); uint64_t* st = (uint64_t*)(*(uintptr_t*)((uint8_t*)view + 8) + 8);
        if (slot >= 0 && slot < 64) st[0] &= ~(1ull << slot); else if (slot >= 64 && slot < 128) st[1] &= ~(1ull << (slot - 64)); }
}
static void hkFadeDraw(void* mgr, void* view, float f, int list, int flag) {
    int c = g_eyeCtx < 0 ? 2 : g_eyeCtx; if (list >= 0 && list < 3) InterlockedIncrement(&g_fadeCalls[c][list]); g_fadeView[c] = view;
    if (g_geoTest == 3 || g_geoTest == 4) return;
    oFadeDraw(mgr, view, f, list, flag);
}
// build 067: Render::SimpleDrawableComponent::Draw (0x140001920, this, view). Copies the instance's 0x40-byte colour
// block [this+0xc0] to frame memory and sets it as the view's cbSceneryInstance state param (bit 34, pointer at
// state+0x120), then submits its meshes (0x14000e010). Runs only if ([this+0x38] & 3) == 1. This is where the eye-1
// lantern gets its 1 1 1 1 colours. Counted per eye here: calls, calls that pass the flag test, distinct views.
typedef void (*SdDraw_t)(void*, void*); static SdDraw_t oSdDraw;
static void hkSdDraw(void* self, void* view) {
    int c = g_eyeCtx < 0 ? 2 : g_eyeCtx; InterlockedIncrement(&g_sdCalls[c]); g_sdFrame[c]++;
    if ((*((uint8_t*)self + 0x38) & 3) == 1) { InterlockedIncrement(&g_sdPass[c]); const float* col = (const float*)((uint8_t*)self + 0xc0); if (col[0] == 1.f && col[1] == 1.f && col[2] == 1.f) InterlockedIncrement(&g_sdWhite[c]); }
    g_sdView[c] = view; g_sdState[c] = view ? *(void**)((uint8_t*)view + 8) : nullptr;
    oSdDraw(self, view);
}
static uint32_t FaRead() { uintptr_t a = At<uintptr_t>(0x2167810); if (!a) return 0; uint32_t* c = *(uint32_t**)(a + 0x28); g_faSz[0] = *(uint64_t*)(a + 0x10); g_faSz[1] = *(uint32_t*)(a + 0x18); g_faSz[2] = *(uint64_t*)(a + 0x20); return c ? *c : 0; }
static void hkEyeFrame(float a, int eye) {
    uint32_t s = FaRead();
    g_eyeCtx = (eye == 0 || eye == 1) ? eye : -1;
    hkEyeFrameInner(a, eye);
    g_eyeCtx = -1;
    uint32_t e = FaRead();
    if (eye == 0 || eye == 1) {
        g_faS[eye] = s; g_faE[eye] = e;
        if (e & 0x80000000u) InterlockedIncrement(&g_faSecond);
        if ((e & 0x7fffffffu) < 4096) InterlockedIncrement(&g_faEmpty);
        if (eye == 1 && !((g_faE[0] ^ s) & 0x80000000u) && (s & 0x7fffffffu) > (g_faE[0] & 0x7fffffffu)) InterlockedIncrement(&g_faReset);
        if (!(e & 0x80000000u) && e < g_faMin) g_faMin = e;
    }
}
// build 063: render state cache fix. The game applies D3D state through a shadow-state cache (flush 0xa20ce0: the
// pending state at obj+0x918 is compared with the "current" copy at obj+0x8 (0x910 bytes) and only differences are
// sent to D3D). In VR the eye-0 lantern draw got another material's constant buffer in PS slot 1: the cache believed
// the slot already held the lantern buffer. Clearing the current copy makes the next flush re-send the whole state.
//   StateFix 0 off | 1 at each eye pass start (after each eye copy and after present) | 2 also at every render target
//   change | 3 every flush. Ctrl+F7 cycles.
static LONG g_stateInv;
typedef void (*Flush_t)(void*); static Flush_t oFlush;
static void hkFlush(void* obj) {
    if (g_stateFix && obj) {
        static void* objs[8]; static LONG gens[8];
        int i = 0; for (; i < 8 && objs[i] && objs[i] != obj; i++) {}
        if (i < 8) { if (!objs[i]) objs[i] = obj;
            if (g_stateFix == 3 || gens[i] != g_stateGen) { gens[i] = g_stateGen; memset((uint8_t*)obj + 8, 0, 0x910); InterlockedIncrement(&g_stateInv); } }
    }
    oFlush(obj);
}
LONG StateFixInvalidations() { return g_stateInv; }
// ---------------------------------------------------------------------------
// 089: in-game VR menu (headset panel). Insert opens / closes it. Up / Down arrows move the selection, Left / Right
// change the selected setting. Every change applies at once and is saved to sdvr.ini; the DLSS quality mode is the one
// setting that needs a game restart (the game's render size is fixed at start).
void XrHudGet(float* dist, float* width); void XrHudSet(float dist, float width);
bool DlssDead(); int DlssQualityActive(); void DlssSetOn(bool on); int DlssMode();
void CrashLogInstall();
static int g_hudSt = 0;
void InputBlockSet(bool on); void InputBlockOwnThread(DWORD tid); void InputBlockInstall(); void InputBlockReport();
static const char* kHudName[3] = { "HUD: PANEL FIXED IN FRONT", "HUD: PANEL FOLLOWS HEAD", "HUD: OFF" };
float XrHudScaleGet(); void XrHudScaleSet(float s); void XrSetHudHidden(bool h);
static bool g_hudPanelIni = true;   // HudPanel=0 in sdvr.ini: the game draws its HUD into the world image (flat behaviour)
static void HudModeSet(int st) { g_hudSt = st; XrSetHudHidden(st == 2); if (g_hudPanelIni) g_hudMode = 1; XrSetHudLock(st == 0); }   // 093: off = panel hidden, not the flat HUD
static void IniInt(const wchar_t* k, int v) { wchar_t b[16]; swprintf(b, 16, L"%d", v); WritePrivateProfileStringW(L"VR", k, b, g_ini); }
static void IniFloat(const wchar_t* k, float v) { wchar_t b[16]; swprintf(b, 16, L"%.2f", v); WritePrivateProfileStringW(L"VR", k, b, g_ini); }
static void MenuTick() {
    enum { I_DLSS, I_QUAL, I_SHARP, I_BLOOM, I_HUD, I_HUDDIST, I_HUDSIZE, I_WORLD, I_FP, I_MIRROR, I_RECENTER, I_COUNT };
    static bool open = false, pIns = false, pUp = false, pDn = false, pInc = false, pDec = false; static int sel = 0; static ULONGLONG nextRep = 0, nextDraw = 0;
    static int qualWant = -1; static const int kQOrder[5] = { 0, 4, 1, 2, 3 };   // DLAA, Ultra Quality, Quality, Balanced, Performance
    static const char* kQ[5] = { "DLAA (FULL SIZE)", "QUALITY", "BALANCED", "PERFORMANCE", "ULTRA QUALITY" };
    if (qualWant < 0) qualWant = GetPrivateProfileIntW(L"VR", L"DlssQuality", 0, g_ini), qualWant = (qualWant < 0 || qualWant > 4) ? 0 : qualWant;
    ULONGLONG now = GetTickCount64(); bool draw = false;
    bool ins = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
    if (ins && !pIns) { open = !open; draw = true; InputBlockSet(open); if (!open) { OsdRequest("VR MENU CLOSED", 1); InputBlockReport(); } }
    pIns = ins;
    if (!open) return;
    bool up = (GetAsyncKeyState(VK_UP) & 0x8000) != 0, dn = (GetAsyncKeyState(VK_DOWN) & 0x8000) != 0;   // 090: arrow keys
    bool inc = (GetAsyncKeyState(VK_RIGHT) & 0x8000) != 0, dec = (GetAsyncKeyState(VK_LEFT) & 0x8000) != 0;
    if (up && !pUp) { sel = (sel + I_COUNT - 1) % I_COUNT; draw = true; }
    if (dn && !pDn) { sel = (sel + 1) % I_COUNT; draw = true; }
    int d = 0;
    if ((inc || dec) && ((inc && !pInc) || (dec && !pDec) || now >= nextRep)) { d = inc ? 1 : -1; nextRep = now + (((inc && !pInc) || (dec && !pDec)) ? 400 : 120); }
    pUp = up; pDn = dn; pInc = inc; pDec = dec;
    if (d) {
        draw = true;
        float hd, hw; XrHudGet(&hd, &hw);
        switch (sel) {
        case I_DLSS: if (!DlssDead()) { bool on = DlssMode() == 0; DlssSetOn(on); IniInt(L"Dlss", on ? 1 : 0); Log("menu: DLSS %s", on ? "on" : "off"); } break;
        case I_QUAL: { int i = 0; for (int k = 0; k < 5; k++) if (kQOrder[k] == qualWant) i = k; i = (i + d + 5) % 5; qualWant = kQOrder[i]; IniInt(L"DlssQuality", qualWant); Log("menu: DLSS quality %d (next start)", qualWant); } break;
        case I_SHARP: { float a = SharpenOn() ? SharpenAmount() : 0.f; a += 0.05f * (float)d; if (a < 0.f) a = 0.f; if (a > 1.f) a = 1.f; SharpenSet(a, a > 0.001f); IniFloat(L"Sharpen", SharpenAmount()); IniInt(L"SharpenOn", 1); } break;
        case I_BLOOM: { extern volatile int g_hideBloom; g_hideBloom = !g_hideBloom; IniInt(L"Bloom", g_hideBloom ? 0 : 1); Log("menu: bloom %s", g_hideBloom ? "off" : "on"); } break;
        case I_HUD: HudModeSet((g_hudSt + d + 3) % 3); IniInt(L"HudMode", g_hudSt); break;
        case I_HUDDIST: { float nd = hd + 0.1f * (float)d; if (nd < 0.5f) nd = 0.5f; if (nd > 5.f) nd = 5.f; hw *= nd / hd; XrHudSet(nd, hw); IniFloat(L"HudDistance", nd); IniFloat(L"HudWidth", hw); } break;   // 093: depth only - the panel keeps its size in the view
        case I_HUDSIZE: { float sc = XrHudScaleGet() + 0.05f * (float)d; if (sc < 0.5f) sc = 0.5f; if (sc > 3.f) sc = 3.f; XrHudScaleSet(sc); IniFloat(L"HudScale", sc); } break;   // 093: scale about the minimap
        case I_WORLD: { float w = 1.f / (g_worldScale > 0.01f ? g_worldScale : 1.f); w = floorf(w * 20.f + 0.5f) / 20.f + 0.05f * (float)d; if (w < 0.5f) w = 0.5f; if (w > 2.f) w = 2.f; g_worldScale = 1.f / w; wchar_t b[24]; swprintf(b, 24, L"%.4f", g_worldScale); WritePrivateProfileStringW(L"VR", L"WorldScale", b, g_ini); Log("menu: world scale %.2f (eye / movement factor %.3f)", w, g_worldScale); } break;   // 124: world size as seen: 1 / the eye-separation factor
        case I_FP: g_fp = !g_fp; IniInt(L"FirstPerson", g_fp); Log("menu: first person %d", (int)g_fp); break;
        case I_MIRROR: g_mirror = (g_mirror + d + 3) % 3; IniInt(L"Mirror", g_mirror); break;
        case I_RECENTER: g_recenterReq = true; break;
        }
    }
    if (!draw && now < nextDraw) return;
    nextDraw = now + 1000;
    float hd, hw; XrHudGet(&hd, &hw);
    char v[I_COUNT][48];
    if (DlssDead()) snprintf(v[I_DLSS], 48, "NOT AVAILABLE"); else snprintf(v[I_DLSS], 48, "%s", DlssMode() ? "ON" : "OFF");
    snprintf(v[I_QUAL], 48, "%s%s", kQ[qualWant], qualWant != DlssQualityActive() ? " (AFTER RESTART)" : "");
    if (SharpenOn() && SharpenAmount() > 0.001f) snprintf(v[I_SHARP], 48, "%.2f", SharpenAmount()); else snprintf(v[I_SHARP], 48, "OFF");
    { extern volatile int g_hideBloom; snprintf(v[I_BLOOM], 48, "%s", g_hideBloom ? "OFF" : "ON"); }
    static const char* hs[3] = { "FIXED IN FRONT", "FOLLOWS HEAD", "OFF" }; snprintf(v[I_HUD], 48, "%s", hs[g_hudSt]);
    snprintf(v[I_HUDDIST], 48, "%.1f M", hd); snprintf(v[I_HUDSIZE], 48, "%.2f", XrHudScaleGet());
    snprintf(v[I_WORLD], 48, "%.2f", 1.f / (g_worldScale > 0.01f ? g_worldScale : 1.f)); snprintf(v[I_FP], 48, "%s", g_fp ? "ON (EXPERIMENTAL)" : "OFF");
    static const char* ms[3] = { "LEFT EYE", "RIGHT EYE", "ALTERNATING" }; snprintf(v[I_MIRROR], 48, "%s", ms[g_mirror % 3]);
    snprintf(v[I_RECENTER], 48, "PRESS RIGHT");
    static const char* nm[I_COUNT] = { "DLSS", "DLSS MODE", "SHARPENING", "BLOOM", "HUD PANEL", "HUD DISTANCE", "HUD SCALE", "WORLD SCALE", "FIRST PERSON", "MONITOR WINDOW", "RECENTER VIEW" };
    char t[800]; int o = snprintf(t, sizeof t, "SLEEPING DOGS VR MENU\n \n");
    for (int i = 0; i < I_COUNT; i++) o += snprintf(t + o, sizeof t - o, "%s %-15s %s\n", i == sel ? ">" : " ", nm[i], v[i]);
    snprintf(t + o, sizeof t - o, " \nUP / DOWN: SELECT     LEFT / RIGHT: CHANGE\nINSERT: CLOSE");
    OsdRequest(t, 3);
}
// build 127 / 130: gamepad shortcuts, read straight from XInput beside the game (the game still sees the buttons).
// Both need LB held, so neither can happen from a single button used in play (the right stick click is the car horn):
//   LB held + Y pressed and held for PadHoldMs (700)  -> recentre the view
//   LB held + B pressed and held for PadHoldMs        -> first person on / off
static int g_padCombo = 1, g_padRecenter = 1, g_padHoldMs = 700;
static void PadTick() {
    struct XState { DWORD packet; WORD buttons; BYTE lt, rt; SHORT lx, ly, rx, ry; };
    typedef DWORD (WINAPI *Get_t)(DWORD, XState*);
    static Get_t get = nullptr; static bool tried = false;
    if (!tried) { tried = true;
        static const wchar_t* kDll[3] = { L"xinput1_3.dll", L"xinput1_4.dll", L"xinput9_1_0.dll" };
        for (int i = 0; i < 3 && !get; i++) { HMODULE h = GetModuleHandleW(kDll[i]); if (h && (get = (Get_t)GetProcAddress(h, "XInputGetState"))) Log("gamepad shortcuts: using %ls", kDll[i]); }   // the one the game already has loaded
        for (int i = 0; i < 3 && !get; i++) { HMODULE h = LoadLibraryW(kDll[i]); if (h && (get = (Get_t)GetProcAddress(h, "XInputGetState"))) Log("gamepad shortcuts: loaded %ls", kDll[i]); }
        if (!get) Log("gamepad shortcuts: XInput not found - off");
    }
    if (!get || (!g_padCombo && !g_padRecenter)) return;
    WORD b = 0; for (DWORD i = 0; i < 4; i++) { XState st = {}; if (get(i, &st) == 0) b |= st.buttons; }
    ULONGLONG now = GetTickCount64();
    static ULONGLONG ySince = 0, bSince = 0; static bool yDone = false, bDone = false;
    const bool lb = (b & 0x0100) != 0, y = lb && (b & 0x8000), bb = lb && (b & 0x2000);
    if (!y) { ySince = 0; yDone = false; }
    else { if (!ySince) ySince = now;
        if (g_padRecenter && !yDone && !bb && now - ySince >= (ULONGLONG)g_padHoldMs) { yDone = true; g_recenterReq = true; Log("recenter requested (LB + Y held)"); OsdRequest("RECENTERED", 2); } }
    if (!bb) { bSince = 0; bDone = false; }
    else { if (!bSince) bSince = now;
        if (g_padCombo && !bDone && !y && now - bSince >= (ULONGLONG)g_padHoldMs) { bDone = true; g_fp = !g_fp; IniInt(L"FirstPerson", g_fp); Log("first person %d (LB + B held)", (int)g_fp); OsdRequest(g_fp ? "FIRST PERSON: ON" : "FIRST PERSON: OFF", 2); } }
}
static DWORD WINAPI KeyThread(LPVOID) {
    InputBlockOwnThread(GetCurrentThreadId());
    bool prev = false;
    for (;;) {
        Sleep(30);
        bool k8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
        static bool p8 = false; if (g_dev && k8 && !p8 && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) { const char* t = DlssCycle(); Log("%s (Ctrl+F8)", t); OsdRequest(t, 4); }
        else if (g_dev && k8 && !p8 && (GetAsyncKeyState(VK_MENU) & 0x8000)) { const char* t = DlssLodCycle(); Log("%s (Alt+F8)", t); OsdRequest(t, 4); }
        else if (k8 && !p8) { g_recenterReq = true; Log("recenter requested (F8)"); OsdRequest("RECENTERED", 2); } p8 = k8;
        bool kPU = (GetAsyncKeyState(VK_PRIOR) & 0x8000) != 0, kPD = (GetAsyncKeyState(VK_NEXT) & 0x8000) != 0, kHM = (GetAsyncKeyState(VK_HOME) & 0x8000) != 0;   // 116: F6 recentres the game camera - shadow test keys moved to Page Up / Page Down / Home
        MenuTick();   // 089: Insert = in-game VR menu
        PadTick();    // 130: LB + Y held = recentre, LB + B held = first person
        static bool pPU = false, pPD = false, pHM = false;
        if (g_dev && kHM && !pHM) { extern volatile int g_hideHurt; g_hideHurt = !g_hideHurt; Log("hurt vignette hidden %d (Home)", (int)g_hideHurt); OsdRequest(g_hideHurt ? "LOW HEALTH RED EDGES: REMOVED" : "LOW HEALTH RED EDGES: GAME DEFAULT", 4); }
        { static bool pEN = false; bool kEN = (GetAsyncKeyState(VK_END) & 0x8000) != 0; if (g_dev && kEN && !pEN) { g_shadowRadial = !g_shadowRadial; Log("radial shadows %d (End)", (int)g_shadowRadial); OsdRequest(g_shadowRadial ? "SHADOWS: ALL ROUND (FIX ON)" : "SHADOWS: GAME DEFAULT (FIX OFF)", 4); } pEN = kEN; }
        if (g_dev && kPU && !pPU) { extern volatile int g_hideDrops; g_hideDrops = !g_hideDrops; Log("screen water drops hidden %d (Page Up)", (int)g_hideDrops); OsdRequest(g_hideDrops ? "SCREEN WATER DROPS: REMOVED" : "SCREEN WATER DROPS: GAME DEFAULT", 4); }
        if (g_dev && kPD && !pPD) { extern volatile int g_hideBlur; g_hideBlur = !g_hideBlur; Log("blur hidden %d (Page Down)", (int)g_hideBlur); OsdRequest(g_hideBlur ? "DEPTH OF FIELD / BLUR: REMOVED" : "DEPTH OF FIELD / BLUR: GAME DEFAULT", 4); } pPU = kPU; pPD = kPD; pHM = kHM;
        bool k5 = (GetAsyncKeyState(VK_F5) & 0x8000) != 0;
        static bool p5 = false; if (g_dev && k5 && !p5 && !(GetAsyncKeyState(VK_MENU) & 0x8000)) {   // 039: 2 -> 4 (2, real inverse kept) -> 3 (2 + real camera restored after the eyes) -> 1 -> 0 -> 2
            int cur = g_cullFix == 2 ? (g_cullRestore ? 3 : g_cullKeepInv ? 4 : 2) : g_cullFix;
            int m = cur == 2 ? 4 : cur == 4 ? 3 : cur == 3 ? 1 : cur == 1 ? 0 : 2;
            g_cullRestore = m == 3; g_cullKeepInv = m == 4; g_cullFix = m >= 2 ? 2 : m; g_cullValid = false;
            static const char* cn[5] = { "CULL MODE 0: OFF (GAME CAMERA ONLY)", "CULL MODE 1: MAIN VIEW ONLY (OLD)", "CULL MODE 2: GAME CAMERA FOLLOWS HEAD", "CULL MODE 3: FOLLOWS HEAD + CAMERA RESTORED", "CULL MODE 4: FOLLOWS HEAD, WORLD MATRIX KEPT" };
            Log("%s (F5)", cn[m]); OsdRequest(cn[m], 4); } p5 = k5;
        bool k4 = (GetAsyncKeyState(VK_F4) & 0x8000) != 0;
        static bool p4 = false; if (g_dev && k4 && !p4) { g_symFov = !g_symFov; Log("symmetric eye FOV %d (F4)", g_symFov); OsdRequest(g_symFov ? "EYE FOV: SYMMETRIC (SHADOW FIX)" : "EYE FOV: HEADSET ASYMMETRIC", 4); } p4 = k4;
        bool k1 = (GetAsyncKeyState(VK_F1) & 0x8000) != 0;   // 044: F1 = menu map fit (the old first-eye block test is retired)
        static bool p1 = false; if (!g_dev) { }
        else if (k1 && !p1) { g_menuMapFit = !g_menuMapFit; Log("menu map fitted to the 16:9 HUD area: %d (F1)", (int)g_menuMapFit); OsdRequest(g_menuMapFit ? "MENU MAP: 16:9 AREA" : "MENU MAP: FULL PANEL", 3); } p1 = k1;
        bool k2 = (GetAsyncKeyState(VK_F2) & 0x8000) != 0;
        static bool p2 = false; if (g_dev && k2 && !p2) { g_nisSmooth = !g_nisSmooth; Log("cutscene smoothing %s (F2)", g_nisSmooth ? "on" : "off"); OsdRequest(g_nisSmooth ? "CUTSCENE SMOOTHING ON" : "CUTSCENE SMOOTHING OFF", 3); } p2 = k2;
        bool k3 = (GetAsyncKeyState(VK_F3) & 0x8000) != 0;
        static bool p3 = false; if (g_dev && k3 && !p3) { g_swapOrder = !g_swapOrder; Log("eye order %d (F3)", g_swapOrder); OsdRequest(g_swapOrder ? "EYE ORDER: RIGHT EYE RENDERED FIRST" : "EYE ORDER: LEFT EYE RENDERED FIRST", 4); } p3 = k3;
        bool k11 = (GetAsyncKeyState(VK_F11) & 0x8000) != 0;
        static bool p11 = false; if (k11 && !p11) { HudModeSet((g_hudSt + 1) % 3); IniInt(L"HudMode", g_hudSt); Log("%s (F11)", kHudName[g_hudSt]); OsdRequest(kHudName[g_hudSt], 3); } p11 = k11;
        bool k10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
        static bool p10 = false; if (g_dev && k10 && !p10 && (GetAsyncKeyState(VK_SHIFT) & 0x8000)) { RtDumpArm(); EyeCapBurst(); Log("render target dump + left-eye burst requested (Shift+F10)"); OsdRequest("LEFT EYE RECORDING 4 S", 4); }
        else if (g_dev && k10 && !p10 && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) { char t[64]; RtSkipNext(t, sizeof t); OsdRequest(t, 4); }
        else if (g_dev && k10 && !p10) { EyeCapRequest(); EyeCapArmHud(); RtOutlineArm(); Log("eye capture requested (F10)"); OsdRequest("EYE CAPTURE SAVED", 2); } p10 = k10;
        bool k9 = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
        { static bool pc5 = false; bool c5 = g_dev && k5 && (GetAsyncKeyState(VK_MENU) & 0x8000);   // 106: Alt+F5 = mist fix on / off (test)
          if (c5 && !pc5) { g_rainFix = !g_rainFix; Log("heightmap fix %d (Alt+F5); draws seen %ld, corrected %ld", g_rainFix, g_hmSeen, g_hmFixed); OsdRequest(g_rainFix ? "MIST FIX ON" : "MIST FIX OFF", 4); } pc5 = c5; }
        static bool p9 = false; if (!g_dev) { }   // 125: timing capture is a developer tool
        else if (k9 && !p9) { XrTimingCapture(); g_camCap = 0; if (g_nisN < 0) g_nisN = 0; Log("timing capture requested (F9)"); OsdRequest("TIMING CAPTURE (4 S) - KEEP MOVING", 3); } p9 = k9;
        bool k = (GetAsyncKeyState(VK_F7) & 0x8000) != 0;
        if (g_dev && k && !prev && (GetAsyncKeyState(VK_CONTROL) & 0x8000)) { g_sceneryFix = g_sceneryFix == 3 ? 0 : g_sceneryFix + 1; char sb[700]; RtSceneryFixStats(sb, sizeof sb); static const char* fn[4] = { "SCENERY FIX OFF", "SCENERY FIX 1: OTHER EYE COLOURS", "SCENERY FIX 2: FULL, WHITE", "SCENERY FIX 3: GAME DEFAULT" }; Log("%s (Ctrl+F7); %s", fn[g_sceneryFix], sb); OsdRequest(fn[g_sceneryFix], 3); }
        else if (k && !prev) { g_mirror = (g_mirror + 1) % 3; static const char* nm[3] = { "left eye", "right eye", "alternating" }; Log("mirror: %s (F7)", nm[g_mirror]); char t[64]; snprintf(t, sizeof t, "MIRROR: %s", nm[g_mirror]); OsdRequest(t, 3); }
        prev = k;
    }
}

// ---------------------------------------------------------------------------
// dinput8 proxy
static HMODULE g_real;
static FARPROC Real(const char* name) {
    if (!g_real) { wchar_t p[MAX_PATH]; GetSystemDirectoryW(p, MAX_PATH); wcscat(p, L"\\dinput8.dll"); g_real = LoadLibraryW(p); }
    return g_real ? GetProcAddress(g_real, name) : nullptr;
}
void InputBlockDirectInput(void* di);
extern "C" {
HRESULT WINAPI Proxy_DirectInput8Create(HINSTANCE h, DWORD v, REFIID r, LPVOID* o, LPUNKNOWN u) {
    auto f = (HRESULT(WINAPI*)(HINSTANCE, DWORD, REFIID, LPVOID*, LPUNKNOWN))Real("DirectInput8Create");
    HRESULT hr = f ? f(h, v, r, o, u) : E_FAIL;
    if (SUCCEEDED(hr) && o && *o && g_spatchOk) InputBlockDirectInput(*o);   // 091: keyboard devices made from it can hide the menu keys
    return hr;
}
HRESULT WINAPI Proxy_DllCanUnloadNow() { auto f = (HRESULT(WINAPI*)())Real("DllCanUnloadNow"); return f ? f() : S_FALSE; }
HRESULT WINAPI Proxy_DllGetClassObject(REFCLSID c, REFIID r, LPVOID* o) { auto f = (HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*))Real("DllGetClassObject"); return f ? f(c, r, o) : E_FAIL; }
HRESULT WINAPI Proxy_DllRegisterServer() { auto f = (HRESULT(WINAPI*)())Real("DllRegisterServer"); return f ? f() : E_FAIL; }
HRESULT WINAPI Proxy_DllUnregisterServer() { auto f = (HRESULT(WINAPI*)())Real("DllUnregisterServer"); return f ? f() : E_FAIL; }
LPCVOID WINAPI Proxy_GetdfDIJoystick() { auto f = (LPCVOID(WINAPI*)())Real("GetdfDIJoystick"); return f ? f() : nullptr; }
}

static void Init() {
    GetModuleFileNameW(g_self, g_dir, MAX_PATH);
    if (wchar_t* s = wcsrchr(g_dir, L'\\')) *s = 0;
    wchar_t lp[MAX_PATH]; swprintf(lp, MAX_PATH, L"%ls\\sdvr.log", g_dir); LogInit(lp);
    swprintf(g_ini, MAX_PATH, L"%ls\\sdvr.ini", g_dir);
    EyeCapInit(g_dir); RtDumpInit(g_dir);
    g_base = (uintptr_t)GetModuleHandleW(nullptr);
    { MODULEINFO mi = {}; GetModuleInformation(GetCurrentProcess(), (HMODULE)g_base, &mi, sizeof mi); g_imgEnd = g_base + mi.SizeOfImage; }
    char exe[MAX_PATH]; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    Log("SDVR build " SDVR_BUILD " (" __DATE__ " " __TIME__ ") in %s, base %p", exe, (void*)g_base);
    if (!StrStrIA(exe, "sdhdship.exe")) { Log("not sdhdship.exe - inactive"); return; }
    for (const Sig& s : kSigs) if (!CheckSig(s)) { Log("this game version isn't the one the mod was made for - inactive"); LogFlush(); return; }
    if (!GetPrivateProfileIntW(L"VR", L"Enabled", 1, g_ini)) { Log("Enabled=0 in sdvr.ini - the mod is off, the game runs unmodified (flat)"); LogFlush(); return; }   // build 064
    g_dev = GetPrivateProfileIntW(L"VR", L"DevTools", 0, g_ini) != 0; if (g_dev) Log("DevTools=1: test keys and diagnostic hooks are on");
    g_mirror = GetPrivateProfileIntW(L"VR", L"Mirror", 0, g_ini) % 3;
    g_track = GetPrivateProfileIntW(L"VR", L"TrackHead", 1, g_ini);
    g_positional = GetPrivateProfileIntW(L"VR", L"Positional", 1, g_ini);
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"WorldScale", L"1.0", b, 32, g_ini); g_worldScale = (float)_wtof(b); }
    if (!(g_worldScale >= 0.25f && g_worldScale <= 4.f)) g_worldScale = 1.f;
    g_fp = GetPrivateProfileIntW(L"VR", L"FirstPerson", 0, g_ini) != 0;
    g_autoRecenter = GetPrivateProfileIntW(L"VR", L"AutoRecenter", 1, g_ini);
    g_padCombo = GetPrivateProfileIntW(L"VR", L"PadFirstPersonCombo", 1, g_ini); g_padRecenter = GetPrivateProfileIntW(L"VR", L"PadRecenterHold", 1, g_ini);
    g_padHoldMs = GetPrivateProfileIntW(L"VR", L"PadHoldMs", 700, g_ini); if (g_padHoldMs < 100) g_padHoldMs = 100;
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"FpForward", L"0.10", b, 32, g_ini); g_fpFwd = (float)_wtof(b); GetPrivateProfileStringW(L"VR", L"FpUp", L"0.07", b, 32, g_ini); g_fpUp = (float)_wtof(b); GetPrivateProfileStringW(L"VR", L"FpMaxDist", L"15", b, 32, g_ini); g_fpMaxDist = (float)_wtof(b); }
    g_shadowFix = GetPrivateProfileIntW(L"VR", L"ShadowFix", 1, g_ini);
    g_cullFix = GetPrivateProfileIntW(L"VR", L"CullFix", 2, g_ini);
    g_stateFix = GetPrivateProfileIntW(L"VR", L"StateFix", 0, g_ini) & 3;
    { extern volatile int g_hideBloom; g_hideBloom = GetPrivateProfileIntW(L"VR", L"Bloom", 1, g_ini) == 0; }
    { extern volatile int g_hideDrops, g_hideHurt, g_hideBlur; g_hideDrops = GetPrivateProfileIntW(L"VR", L"HideScreenDrops", 1, g_ini) != 0; g_hideHurt = GetPrivateProfileIntW(L"VR", L"HideHurtOverlay", 1, g_ini) != 0; g_hideBlur = GetPrivateProfileIntW(L"VR", L"HideBlur", 1, g_ini) != 0; }
    g_rainFix = GetPrivateProfileIntW(L"VR", L"MistFix", 1, g_ini) != 0;   // 113: vehicle mist in the second eye (heightmap texture)
    g_sceneryFix = GetPrivateProfileIntW(L"VR", L"SceneryFix", 1, g_ini); if (g_sceneryFix < 0 || g_sceneryFix > 3) g_sceneryFix = 1;
    g_cullRestore = GetPrivateProfileIntW(L"VR", L"CullRestore", 0, g_ini);
    g_cullKeepInv = GetPrivateProfileIntW(L"VR", L"CullKeepWorldMatrix", 1, g_ini);   // 040: default (user confirmed the radar keeps the game's heading)   // 031: off by default (030 made the game hang on loading); F5 test mode 3
    g_shadowRadial = GetPrivateProfileIntW(L"VR", L"ShadowRadial", 1, g_ini);
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"ShadowRadialScale", L"1.5", b, 32, g_ini); float v = (float)_wtof(b); if (v >= 0.25f && v <= 4.f) g_shadowRadialScale = v; }
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"ShadowScale", L"1.0", b, 32, g_ini); float v = (float)_wtof(b); if (v >= 0.5f && v <= 4.f) g_shadowScale = v; }
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"CullScale", L"1.25", b, 32, g_ini); g_cullScale = (float)_wtof(b); if (g_cullScale < 0.5f) g_cullScale = 1.25f; }
    g_symFov = GetPrivateProfileIntW(L"VR", L"SymFov", 1, g_ini);
    g_nisSmooth = GetPrivateProfileIntW(L"VR", L"CutsceneSmooth", 1, g_ini);
    Log("settings: SymFov %d, ShadowFix %d, CullFix %d (world matrix kept %d, restore %d), CullScale %.2f, CutsceneSmooth %d, MinimapFix %d", g_symFov, g_shadowFix, g_cullFix, (int)g_cullKeepInv, (int)g_cullRestore, g_cullScale, (int)g_nisSmooth, g_minimapFix);
    XrConfigure(g_ini);
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"Sharpen", L"0", b, 32, g_ini); SharpenSet((float)_wtof(b), GetPrivateProfileIntW(L"VR", L"SharpenOn", 1, g_ini) != 0); Log("sharpening: %.2f %s (Insert panel)", SharpenAmount(), SharpenOn() ? "on" : "off"); }
    g_rw = GetPrivateProfileIntW(L"VR", L"RenderWidth", 3072, g_ini);
    g_rh = GetPrivateProfileIntW(L"VR", L"RenderHeight", 3264, g_ini);
    DlssConfigure(g_ini, &g_rw, &g_rh);   // 081: with a DLSS upscaling mode the game renders smaller than the headset image
    MH_Initialize();
    g_spatchOk = true;
    ShaderInfoInstall();
    InputBlockInstall();
    CrashLogInstall();
    PatchWindowed();
    PatchRenderSize();
    {   // build 007: the game's stereo stage rebuilds the render view from the camera only for eye 1 (+0x595fa:
        // cmp eye,1 / jne): eye 0 kept the view made before the eye loop (no head pose, game projection). Rebuild it for both eyes.
        uint8_t* p = (uint8_t*)(g_base + 0x595fa);
        static const uint8_t want[] = { 0x83, 0x3d, 0x77, 0x04, 0x3e, 0x02, 0x01, 0x75, 0x05, 0xe8 };
        if (!memcmp(p, want, sizeof want)) { DWORD o; VirtualProtect(p + 7, 2, PAGE_EXECUTE_READWRITE, &o); p[7] = 0x90; p[8] = 0x90; VirtualProtect(p + 7, 2, o, &o); Log("patch: view rebuilt for both eyes (+0x59601)"); }
        else Log("patch: eye view rebuild site not as expected - skipped");
    }
    Hook(rva::StereoCreate, (void*)hkStereoCreate, (void**)&oStereoCreate, "StereoCreate");
    Hook(rva::EyeFrame, (void*)hkEyeFrame, (void**)&oEyeFrame, "EyeFrame");
    if (g_dev) Hook(0xa20ce0, (void*)hkFlush, (void**)&oFlush, "StateFlush");
    if (g_dev) Hook(0x1920, (void*)hkSdDraw, (void**)&oSdDraw, "SimpleDrawableDraw");
    if (g_dev) Hook(0x1d4720, (void*)hkGeoDraw, (void**)&oGeoDraw, "GeoDraw");
    if (g_dev) Hook(0x1d3d00, (void*)hkFadeDraw, (void**)&oFadeDraw, "FadeDraw");
    Hook(rva::Present, (void*)hkPresent, (void**)&oPresent, "Present");
    Hook(0x5d860, (void*)hkViewRebuild, (void**)&oViewRebuild, "ViewRebuild");
    Hook(0x3d8a0, (void*)hkShadowUpd, (void**)&oShadowUpd, "ShadowUpdate");
    Hook(0x38310, (void*)hkCascFit, (void**)&oCascFit, "CascadeFit");
    Hook(0x32700, (void*)hkEnvMapUpd, (void**)&oEnvMapUpd, "EnvMapUpdate");
    Hook(0x2ed90, (void*)hkImpostUpd, (void**)&oImpostUpd, "ImpostersUpdate");
    Hook(0x33b30, (void*)hkExposureUpd, (void**)&oExposureUpd, "ExposureUpdate");
    Hook(0xb9a90, (void*)hkSetView, (void**)&oSetView, "CameraSetView");
    Hook(0x36420, (void*)hkShadowCb, (void**)&oShadowCb, "ShadowCollectorCb");
    g_hudMode = GetPrivateProfileIntW(L"VR", L"HudPanel", 1, g_ini); g_hudPanelIni = g_hudMode != 0;
    { int hm = GetPrivateProfileIntW(L"VR", L"HudMode", 0, g_ini); if (hm >= 0 && hm <= 2 && g_hudPanelIni) HudModeSet(hm); }   // 093: 0 fixed in front, 1 follows head, 2 off
    g_minimapFix = GetPrivateProfileIntW(L"VR", L"MinimapFix", 1, g_ini);
    g_markerFix = GetPrivateProfileIntW(L"VR", L"MarkerFix", 1, g_ini);
    g_glowFix = GetPrivateProfileIntW(L"VR", L"HudGlowFix", 1, g_ini);
    g_menuMapFit = GetPrivateProfileIntW(L"VR", L"MenuMapFit", 1, g_ini);   // 045: default 1 (user preferred the fitted map)
    g_fogSync = GetPrivateProfileIntW(L"VR", L"FogSync", 0, g_ini) % 2;   // 049: default off (no effect on the lanterns)
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"MinimapAspect", L"1.7778", b, 32, g_ini); RtMinimapSetup(HudFlush, (float)_wtof(b)); }
    Hook(0x58910, (void*)hkUiRender, (void**)&oUiRender, "UiRender");
    g_menuMode = GetPrivateProfileIntW(L"VR", L"MenuScreen", 1, g_ini);
    Hook(0x611f0, (void*)hkFxUpd, (void**)&oFxUpd, "PauseEffectUpdate");
    Hook(0x61720, (void*)hkQueue, (void**)&oQueue, "RenderQueue");
    Hook(0x69ea60, (void*)hkEyeCopy, (void**)&oEyeCopy, "EyeCopy");
    { void TimerFixStartup(const wchar_t*); TimerFixStartup(g_ini); }   // 140
    Hook(0x3e50c0, (void*)hkNisSetPlayTime, (void**)&oNisSetPlayTime, "NisSetPlayTime");
    Hook(0xb9d40, (void*)hkCamView, (void**)&oCamView, "CameraView");
    Hook(0x6c6f0, (void*)hkFogUpd, (void**)&oFogUpd, "FogUpdate");
    Hook(0x188bd0, (void*)hkPersp, (void**)&oPersp, "Perspective");
    if (GetPrivateProfileIntW(L"VR", L"HalDiag", 0, g_ini)) HalDiagInstall(&g_inEye);
    if (HANDLE t = CreateThread(nullptr, 0, KeyThread, nullptr, 0, nullptr)) CloseHandle(t);
    LogFlush();
}

extern "C" void SdvrLog(const char* text) { Log("%s", text); }   // exported: sdvr_spatch.dll writes its start line here
// 085: SPatch (xowny, GPLv3) is its own module, sdvr_spatch.dll, next to this dll. Loaded when present and not switched off.
static void LoadSpatch() {
    if (!GetPrivateProfileIntW(L"SPatch", L"Enabled", 1, g_ini)) { Log("spatch: off in sdvr.ini"); return; }
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%ls\\sdvr_spatch.dll", g_dir);
    if (GetFileAttributesW(p) == INVALID_FILE_ATTRIBUTES) { Log("spatch: sdvr_spatch.dll not installed - SPatch fixes are off"); return; }
    HMODULE m = LoadLibraryW(p);
    Log("spatch: sdvr_spatch.dll %s (error %lu)", m ? "loaded" : "FAILED to load", m ? 0ul : GetLastError());
}
BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) { g_self = h; DisableThreadLibraryCalls(h); Init(); if (g_spatchOk) LoadSpatch(); LogFlush(); }
    else if (reason == DLL_PROCESS_DETACH) { Log("process detach after %lld presents", g_presents); LogFlush(); }
    return TRUE;
}
