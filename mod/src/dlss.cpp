// SDVR - DLAA / DLSS (build 079)
// NVIDIA NGX, D3D11, in the game's own device. One DLSS feature per eye (each eye keeps its own history).
//   game thread:   DlssJitter + DlssSetCamera per eye pass (view, clean projection, jitter) under the frame sequence number
//   render thread: DlssNoteTargets (finds the scene depth buffer: the G-buffer bind, 3 targets + depth at eye size)
//                  DlssEvaluate at the eye copy: depth -> R32F + camera motion vectors (one full-screen pass), then DLSS;
//                  the result goes to the headset instead of the game's eye image.
// The HUD is its own OpenXR layer, so it never passes through DLSS.
// The NGX runtime (nvngx_dlss.dll) is NOT part of this source tree: it must sit next to SDHDShip.exe.
// This software contains source code provided by NVIDIA Corporation (NGX SDK headers, linked SDK library).
#define NOMINMAX
#include <windows.h>
#include <d3d11_1.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <nvsdk_ngx.h>
#include <nvsdk_ngx_helpers.h>
#include "log.h"

void OsdRequest(const char* text, int seconds);
bool g_rtBypass;   // rtdiag: our own passes are not the game's (no tracking, no scenery fix)

// mode: 0 off, 1 on (with sub-pixel jitter), 2 on without jitter (test), 3 input view (depth + motion vectors shown)
// quality: 0 DLAA (render size = headset size), 1 Quality 67%, 2 Balanced 58%, 3 Performance 50%, 4 Ultra Quality 77%
static volatile int g_mode = 0;
static volatile LONG g_resetEye[2] = { 1, 1 };
static bool g_dead; static const char* g_stage = "not started";
static int g_jitSign[2] = { 1, 1 };
static int g_quality; static int g_preset = 11; static UINT g_cfgOutW, g_cfgOutH; static int g_phases = 8;
static float g_lodBias; static volatile int g_lodOn = 1; static bool g_lodAuto = true;
static const float kScale[5] = { 1.f, 0.667f, 0.58f, 0.5f, 0.77f };
static const char* kQName[5] = { "DLAA", "DLSS QUALITY", "DLSS BALANCED", "DLSS PERFORMANCE", "DLSS ULTRA QUALITY" };
int DlssMode() { return g_dead ? 0 : g_mode; }
bool DlssJitterOn() { return !g_dead && g_mode == 1; }

struct Cam { float V[16], P[16]; float jx, jy; long seq; bool ok; };
static Cam g_ring[8][2];
static SRWLOCK g_camLock = SRWLOCK_INIT;

static float Halton(int i, int b) { float f = 1.f, r = 0.f; while (i > 0) { f /= (float)b; r += f * (float)(i % b); i /= b; } return r; }
// game thread: the sub-pixel offset of this frame (pixels, -0.5..0.5); both eyes of a frame use the same one
void DlssJitter(long seq, float* jx, float* jy) {
    if (!DlssJitterOn()) { *jx = *jy = 0.f; return; }
    int i = (int)(seq % g_phases) + 1;
    *jx = Halton(i, 2) - 0.5f; *jy = Halton(i, 3) - 0.5f;
}
// game thread: the camera an eye pass is rendered with (P without the jitter)
void DlssSetCamera(long seq, int eye, const float* V, const float* P, float jx, float jy) {
    if (eye < 0 || eye > 1) return;
    AcquireSRWLockExclusive(&g_camLock);
    Cam& c = g_ring[seq & 7][eye]; memcpy(c.V, V, 64); memcpy(c.P, P, 64); c.jx = jx; c.jy = jy; c.seq = seq; c.ok = true;
    ReleaseSRWLockExclusive(&g_camLock);
}
static long g_seq;
void DlssSetSeq(long seq) { g_seq = seq; }

// ---------------------------------------------------------------------------------------------------------------
static ID3D11Device* g_dev; static ID3D11DeviceContext1* g_ctx; static ID3DDeviceContextState* g_iso;
static NVSDK_NGX_Parameter* g_params; static NVSDK_NGX_Handle* g_feat[2];
static ID3D11Texture2D* g_out[2]; static UINT g_w, g_h, g_ow, g_oh; static bool g_inverted;
static ID3D11Texture2D* g_depthF, *g_mv, *g_viz, *g_depthCopy; static ID3D11RenderTargetView* g_depthFR, *g_mvR, *g_vizR;
static ID3D11ShaderResourceView* g_depthFS, *g_mvS;
static ID3D11VertexShader* g_vs; static ID3D11PixelShader* g_psPrep, *g_psViz; static ID3D11Buffer* g_cb;
static ID3D11Texture2D* g_gameDepth; static ID3D11DepthStencilView* g_lastDsv; static ID3D11ShaderResourceView* g_gameDepthS; static ID3D11Texture2D* g_srvFor;
static Cam g_prev[2]; static bool g_havePrev[2];
static UINT g_eyeW, g_eyeH;
static LONG64 g_evals; static double g_ms;

static const char* kShader = R"(
Texture2D<float> D : register(t0);
Texture2D<float2> MV : register(t1);
cbuffer C : register(b0) { row_major float4x4 M; float2 size; float inverted; float pad; };
float4 VS(uint id : SV_VertexID) : SV_Position { return float4((id == 1) ? 3.0 : -1.0, (id == 2) ? -3.0 : 1.0, 0.0, 1.0); }
struct O { float d : SV_Target0; float2 mv : SV_Target1; };
O Prep(float4 pos : SV_Position) {
    float d = D.Load(int3(pos.xy, 0));
    float2 uv = pos.xy / size;
    float4 p = mul(float4(uv.x * 2.0 - 1.0, 1.0 - uv.y * 2.0, d, 1.0), M);
    O o; o.d = d; o.mv = float2(0.0, 0.0);
    if (abs(p.w) > 1e-7) { float2 q = float2(p.x / p.w * 0.5 + 0.5, 0.5 - p.y / p.w * 0.5); o.mv = (q - uv) * size; }
    return o;
}
float4 Viz(float4 pos : SV_Position) : SV_Target {
    float d = D.Load(int3(pos.xy, 0)); float2 m = MV.Load(int3(pos.xy, 0));
    float z = inverted > 0.5 ? d : 1.0 - d;
    return float4(saturate(abs(m.x) / 16.0), saturate(abs(m.y) / 16.0), saturate(pow(saturate(z), 0.25)), 1.0);
}
)";

static bool Inv4(const double* m, double* o) {
    double inv[16];
    inv[0] = m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] + m[9]*m[7]*m[14] + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4] = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] - m[8]*m[7]*m[14] - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8] = m[4]*m[9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] + m[8]*m[7]*m[13] + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] - m[8]*m[6]*m[13] - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];
    inv[1] = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] - m[9]*m[3]*m[14] - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5] = m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] + m[8]*m[3]*m[14] + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9] = -m[0]*m[9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] - m[8]*m[3]*m[13] - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] = m[0]*m[9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] + m[8]*m[2]*m[13] + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];
    inv[2] = m[1]*m[6]*m[15] - m[1]*m[7]*m[14] - m[5]*m[2]*m[15] + m[5]*m[3]*m[14] + m[13]*m[2]*m[7] - m[13]*m[3]*m[6];
    inv[6] = -m[0]*m[6]*m[15] + m[0]*m[7]*m[14] + m[4]*m[2]*m[15] - m[4]*m[3]*m[14] - m[12]*m[2]*m[7] + m[12]*m[3]*m[6];
    inv[10] = m[0]*m[5]*m[15] - m[0]*m[7]*m[13] - m[4]*m[1]*m[15] + m[4]*m[3]*m[13] + m[12]*m[1]*m[7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[5]*m[14] + m[0]*m[6]*m[13] + m[4]*m[1]*m[14] - m[4]*m[2]*m[13] - m[12]*m[1]*m[6] + m[12]*m[2]*m[5];
    inv[3] = -m[1]*m[6]*m[11] + m[1]*m[7]*m[10] + m[5]*m[2]*m[11] - m[5]*m[3]*m[10] - m[9]*m[2]*m[7] + m[9]*m[3]*m[6];
    inv[7] = m[0]*m[6]*m[11] - m[0]*m[7]*m[10] - m[4]*m[2]*m[11] + m[4]*m[3]*m[10] + m[8]*m[2]*m[7] - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[5]*m[11] + m[0]*m[7]*m[9] + m[4]*m[1]*m[11] - m[4]*m[3]*m[9] - m[8]*m[1]*m[7] + m[8]*m[3]*m[5];
    inv[15] = m[0]*m[5]*m[10] - m[0]*m[6]*m[9] - m[4]*m[1]*m[10] + m[4]*m[2]*m[9] + m[8]*m[1]*m[6] - m[8]*m[2]*m[5];
    double det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (det == 0.0) return false;
    for (int i = 0; i < 16; i++) o[i] = inv[i] / det;
    return true;
}
static void MulD(const double* a, const double* b, double* o) {
    double t[16];
    for (int r = 0; r < 4; r++) for (int c = 0; c < 4; c++) { double s = 0; for (int k = 0; k < 4; k++) s += a[r * 4 + k] * b[k * 4 + c]; t[r * 4 + c] = s; }
    memcpy(o, t, sizeof t);
}
static void ViewProj(const Cam& c, double* o) { double v[16], p[16]; for (int i = 0; i < 16; i++) { v[i] = c.V[i]; p[i] = c.P[i]; } MulD(v, p, o); }

// SEH kept out of frames with C++ objects: a fault inside the NGX runtime switches DLSS off instead of taking the game down
static NVSDK_NGX_Result NgxInit(ID3D11Device* d, const wchar_t* dir, NVSDK_NGX_FeatureCommonInfo* info, bool* fault) {
    __try { return NVSDK_NGX_D3D11_Init_with_ProjectID("5d3c1a8e-7b42-4f09-9a6e-2c81d4e0b7f3", NVSDK_NGX_ENGINE_TYPE_CUSTOM, "SDVR", dir, d, info, NVSDK_NGX_Version_API); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *fault = true; return NVSDK_NGX_Result_Fail; }
}
static NVSDK_NGX_Result NgxCreate(ID3D11DeviceContext* c, NVSDK_NGX_Handle** h, NVSDK_NGX_Parameter* p, NVSDK_NGX_DLSS_Create_Params* cp, bool* fault) {
    __try { return NGX_D3D11_CREATE_DLSS_EXT(c, h, p, cp); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *fault = true; return NVSDK_NGX_Result_Fail; }
}
static NVSDK_NGX_Result NgxEval(ID3D11DeviceContext* c, NVSDK_NGX_Handle* h, NVSDK_NGX_Parameter* p, NVSDK_NGX_D3D11_DLSS_Eval_Params* ep, bool* fault) {
    __try { return NGX_D3D11_EVALUATE_DLSS_EXT(c, h, p, ep); }
    __except (EXCEPTION_EXECUTE_HANDLER) { *fault = true; return NVSDK_NGX_Result_Fail; }
}
static void Dead(const char* why, long code) {
    g_dead = true; Log("dlss: OFF for this run - %s (stage %s, code 0x%lx)", why, g_stage, code);
    char t[96]; snprintf(t, sizeof t, "DLSS UNAVAILABLE: %s", why); OsdRequest(t, 6);
}

typedef HRESULT (WINAPI *Compile_t)(LPCVOID, SIZE_T, LPCSTR, const void*, void*, LPCSTR, LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
static ID3DBlob* Compile(const char* entry, const char* target) {
    static Compile_t f;
    if (!f) { HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll"); if (!h) h = LoadLibraryW(L"d3dcompiler_46.dll"); if (h) f = (Compile_t)GetProcAddress(h, "D3DCompile"); }
    if (!f) return nullptr;
    ID3DBlob* b = nullptr, *e = nullptr;
    HRESULT hr = f(kShader, strlen(kShader), "sdvr_dlss", nullptr, nullptr, entry, target, 0, 0, &b, &e);
    if (FAILED(hr)) { Log("dlss: shader %s failed 0x%lx: %s", entry, hr, e ? (const char*)e->GetBufferPointer() : ""); if (e) e->Release(); return nullptr; }
    if (e) e->Release();
    return b;
}
static bool Tex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D11Texture2D** t, ID3D11RenderTargetView** r, ID3D11ShaderResourceView** s) {
    D3D11_TEXTURE2D_DESC d = {}; d.Width = w; d.Height = h; d.MipLevels = 1; d.ArraySize = 1; d.Format = f; d.SampleDesc.Count = 1; d.BindFlags = bind;
    if (FAILED(g_dev->CreateTexture2D(&d, nullptr, t))) return false;
    if (r && FAILED(g_dev->CreateRenderTargetView(*t, nullptr, r))) return false;
    if (s && FAILED(g_dev->CreateShaderResourceView(*t, nullptr, s))) return false;
    return true;
}
template <class T> static void Rel(T*& p) { if (p) { p->Release(); p = nullptr; } }

static bool InitOnce(ID3D11DeviceContext* ctx) {
    static bool tried, ok; if (tried) return ok; tried = true;
    g_stage = "device";
    ctx->GetDevice(&g_dev);
    ID3D11Device1* d1 = nullptr;
    if (FAILED(g_dev->QueryInterface(__uuidof(ID3D11Device1), (void**)&d1)) || FAILED(ctx->QueryInterface(__uuidof(ID3D11DeviceContext1), (void**)&g_ctx))) { Dead("no D3D 11.1", 0); return false; }
    D3D_FEATURE_LEVEL lv = g_dev->GetFeatureLevel();
    UINT fl = (g_dev->GetCreationFlags() & D3D11_CREATE_DEVICE_SINGLETHREADED) ? D3D11_1_CREATE_DEVICE_CONTEXT_STATE_SINGLETHREADED : 0;
    HRESULT hr = d1->CreateDeviceContextState(fl, &lv, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &g_iso);
    d1->Release();
    if (FAILED(hr)) { Dead("no context state", hr); return false; }
    g_stage = "runtime file";
    wchar_t dir[MAX_PATH]; DWORD n = GetModuleFileNameW(nullptr, dir, MAX_PATH); while (n && dir[n - 1] != L'\\' && dir[n - 1] != L'/') n--; if (n) dir[n - 1] = 0;
    wchar_t dll[MAX_PATH]; swprintf(dll, MAX_PATH, L"%ls\\nvngx_dlss.dll", dir);
    if (GetFileAttributesW(dll) == INVALID_FILE_ATTRIBUTES) { Dead("nvngx_dlss.dll is not in the game folder", 0); return false; }
    g_stage = "shaders";
    ID3DBlob* bv = Compile("VS", "vs_5_0"), *bp = Compile("Prep", "ps_5_0"), *bz = Compile("Viz", "ps_5_0");
    if (!bv || !bp || !bz) { Dead("shader compiler", 0); return false; }
    g_dev->CreateVertexShader(bv->GetBufferPointer(), bv->GetBufferSize(), nullptr, &g_vs);
    g_dev->CreatePixelShader(bp->GetBufferPointer(), bp->GetBufferSize(), nullptr, &g_psPrep);
    g_dev->CreatePixelShader(bz->GetBufferPointer(), bz->GetBufferSize(), nullptr, &g_psViz);
    bv->Release(); bp->Release(); bz->Release();
    D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 80; bd.Usage = D3D11_USAGE_DEFAULT; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    g_dev->CreateBuffer(&bd, nullptr, &g_cb);
    if (!g_vs || !g_psPrep || !g_psViz || !g_cb) { Dead("shader objects", 0); return false; }
    g_stage = "ngx init";
    const wchar_t* paths[] = { dir }; NVSDK_NGX_FeatureCommonInfo info = {}; info.PathListInfo.Path = paths; info.PathListInfo.Length = 1;
    bool fault = false; NVSDK_NGX_Result r;
    {
        ID3DDeviceContextState* old = nullptr; g_ctx->SwapDeviceContextState(g_iso, &old);
        r = NgxInit(g_dev, dir, &info, &fault);
        g_ctx->SwapDeviceContextState(old, nullptr); if (old) old->Release();
    }
    if (fault || NVSDK_NGX_FAILED(r)) { Dead(fault ? "NGX crashed at start" : "NGX did not start (NVIDIA RTX card and current driver needed)", (long)r); return false; }
    g_stage = "ngx capability";
    r = NVSDK_NGX_D3D11_GetCapabilityParameters(&g_params);
    if (NVSDK_NGX_FAILED(r) || !g_params) { Dead("NGX parameters", (long)r); return false; }
    int avail = 0, needDrv = 0; unsigned maj = 0, min = 0;
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_Available, &avail);
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_NeedsUpdatedDriver, &needDrv);
    g_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMajor, &maj); g_params->Get(NVSDK_NGX_Parameter_SuperSampling_MinDriverVersionMinor, &min);
    Log("dlss: NGX started; DLSS available %d, driver update needed %d (minimum %u.%u)", avail, needDrv, maj, min);
    if (!avail) { Dead(needDrv ? "graphics driver too old" : "DLSS not supported on this card", 0); return false; }
    ok = true; return true;
}

static void DropSized() {
    for (int e = 0; e < 2; e++) { if (g_feat[e]) { NVSDK_NGX_D3D11_ReleaseFeature(g_feat[e]); g_feat[e] = nullptr; } Rel(g_out[e]); g_havePrev[e] = false; }
    Rel(g_depthFR); Rel(g_depthFS); Rel(g_depthF); Rel(g_mvR); Rel(g_mvS); Rel(g_mv); Rel(g_vizR); Rel(g_viz); Rel(g_depthCopy);
    g_w = g_h = g_ow = g_oh = 0;
}
static bool MakeSized(UINT w, UINT h, UINT ow, UINT oh) {
    DropSized();
    g_stage = "work textures";
    if (!Tex(w, h, DXGI_FORMAT_R32_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g_depthF, &g_depthFR, &g_depthFS) ||
        !Tex(w, h, DXGI_FORMAT_R16G16_FLOAT, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g_mv, &g_mvR, &g_mvS) ||
        !Tex(w, h, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE, &g_viz, &g_vizR, nullptr)) return false;
    for (int e = 0; e < 2; e++)
        if (!Tex(ow, oh, DXGI_FORMAT_R8G8B8A8_UNORM, D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET, &g_out[e], nullptr, nullptr)) return false;
    g_stage = "ngx create";
    bool up = ow != w || oh != h; int q = up ? (g_quality ? g_quality : 1) : 0;
    static const NVSDK_NGX_PerfQuality_Value kQ[5] = { NVSDK_NGX_PerfQuality_Value_DLAA, NVSDK_NGX_PerfQuality_Value_MaxQuality, NVSDK_NGX_PerfQuality_Value_Balanced, NVSDK_NGX_PerfQuality_Value_MaxPerf, NVSDK_NGX_PerfQuality_Value_MaxQuality };   // Ultra Quality runs in the Quality slot (input size is explicit)
    {   // DlssPreset: the DLSS model. 10 J, 11 K (default), 12 L, 13 M; 16 (or 0) = NVIDIA's own choice per mode
        int pv = (g_preset >= 10 && g_preset <= 13) ? g_preset : (int)NVSDK_NGX_DLSS_Hint_Render_Preset_Default;
        static const char* const kKeys[6] = { NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                                              NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance };
        for (const char* k : kKeys)
            g_params->Set(k, pv);
    }
    for (int e = 0; e < 2; e++) {
        NVSDK_NGX_DLSS_Create_Params cp = {};
        cp.Feature.InWidth = w; cp.Feature.InHeight = h; cp.Feature.InTargetWidth = ow; cp.Feature.InTargetHeight = oh;
        cp.Feature.InPerfQualityValue = kQ[q];
        cp.InFeatureCreateFlags = NVSDK_NGX_DLSS_Feature_Flags_MVLowRes | NVSDK_NGX_DLSS_Feature_Flags_AutoExposure | (g_inverted ? NVSDK_NGX_DLSS_Feature_Flags_DepthInverted : 0);
        bool fault = false; NVSDK_NGX_Result r = NgxCreate(g_ctx, &g_feat[e], g_params, &cp, &fault);
        if (fault || NVSDK_NGX_FAILED(r) || !g_feat[e]) { g_feat[e] = nullptr; Dead(fault ? "NGX crashed creating DLSS" : "DLSS could not be created", (long)r); return false; }
    }
    g_w = w; g_h = h; g_ow = ow; g_oh = oh;
    Log("dlss: %s ready, %ux%u -> %ux%u per eye, preset %s, depth %s", kQName[q], w, h, ow, oh, g_preset == 10 ? "J" : g_preset == 11 ? "K" : g_preset == 12 ? "L" : g_preset == 13 ? "M" : "NVIDIA default", g_inverted ? "reversed (near = 1)" : "normal (near = 0)");
    return true;
}

// render thread, every OMSetRenderTargets of the game: remember the scene depth buffer (G-buffer pass)
static ID3D11DepthStencilView* g_seenDsv[16]; static int g_nSeen;
void DlssNoteTargets(UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (!g_mode || g_dead || !dsv || dsv == g_lastDsv || !g_eyeW) return;
    UINT live = 0; if (rtv) for (UINT i = 0; i < n && i < 8; i++) if (rtv[i]) live++;
    bool seen = false; for (int i = 0; i < g_nSeen; i++) if (g_seenDsv[i] == dsv) seen = true;
    if (seen && live < 2) return;
    ID3D11Resource* r = nullptr; dsv->GetResource(&r); if (!r) return;
    ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    if (!seen && g_nSeen < 16) { g_seenDsv[g_nSeen++] = dsv; Log("dlss: depth bind seen: view %p tex %p %ux%u fmt %d samples %u bind 0x%x, %u targets (%u set)", (void*)dsv, (void*)t, d.Width, d.Height, (int)d.Format, d.SampleDesc.Count, d.BindFlags, n, live); }
    if (live < 2 || d.Width != g_eyeW || d.Height != g_eyeH || d.SampleDesc.Count != 1) { t->Release(); return; }
    if (t != g_gameDepth) LOG_FIRST(6, "dlss: scene depth buffer %p (%u targets)", (void*)t, live);
    Rel(g_gameDepth); g_gameDepth = t; g_lastDsv = dsv;
}

// a shader view of the game's depth buffer (directly when the game made it readable, else through a copy)
static ID3D11ShaderResourceView* DepthView() {
    if (!g_gameDepth) return nullptr;
    if (g_srvFor == g_gameDepth && g_gameDepthS) { if (g_depthCopy) g_ctx->CopyResource(g_depthCopy, g_gameDepth); return g_gameDepthS; }
    Rel(g_gameDepthS); Rel(g_depthCopy); g_srvFor = nullptr;
    D3D11_TEXTURE2D_DESC d; g_gameDepth->GetDesc(&d);
    DXGI_FORMAT tl, sv;
    switch (d.Format) {
    case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT: tl = DXGI_FORMAT_R24G8_TYPELESS; sv = DXGI_FORMAT_R24_UNORM_X8_TYPELESS; break;
    case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: tl = DXGI_FORMAT_R32_TYPELESS; sv = DXGI_FORMAT_R32_FLOAT; break;
    case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT: tl = DXGI_FORMAT_R32G8X24_TYPELESS; sv = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS; break;
    default: Log("dlss: depth format %d not handled", (int)d.Format); return nullptr;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC sd = {}; sd.Format = sv; sd.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D; sd.Texture2D.MipLevels = 1;
    ID3D11Texture2D* from = g_gameDepth;
    if (!(d.BindFlags & D3D11_BIND_SHADER_RESOURCE) || d.Format != tl) {
        D3D11_TEXTURE2D_DESC c = d; c.Format = tl; c.BindFlags = D3D11_BIND_SHADER_RESOURCE; c.Usage = D3D11_USAGE_DEFAULT; c.CPUAccessFlags = 0; c.MiscFlags = 0;
        if (FAILED(g_dev->CreateTexture2D(&c, nullptr, &g_depthCopy))) { Log("dlss: depth copy texture failed"); return nullptr; }
        g_ctx->CopyResource(g_depthCopy, g_gameDepth); from = g_depthCopy;
    }
    if (FAILED(g_dev->CreateShaderResourceView(from, &sd, &g_gameDepthS))) { Log("dlss: depth view failed (fmt %d)", (int)d.Format); Rel(g_depthCopy); return nullptr; }
    g_srvFor = g_gameDepth;
    Log("dlss: depth is read %s", g_depthCopy ? "through a copy" : "directly");
    return g_gameDepthS;
}

// render thread, at the eye copy. eye = the eye pass (0 first). Returns the image to send to the headset, or null = use src.
ID3D11Texture2D* DlssEvaluate(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    if (!src || eye < 0 || eye > 1) return nullptr;
    D3D11_TEXTURE2D_DESC sd; src->GetDesc(&sd); g_eyeW = sd.Width; g_eyeH = sd.Height;
    int mode = g_mode;
    if (!mode || g_dead) return nullptr;
    if (!InitOnce(ctx)) return nullptr;
    if (sd.Format != DXGI_FORMAT_R8G8B8A8_UNORM || sd.SampleDesc.Count != 1) { LOG_FIRST(1, "dlss: eye image fmt %d samples %u not handled", (int)sd.Format, sd.SampleDesc.Count); return nullptr; }
    if (!g_gameDepth) { LOG_FIRST(1, "dlss: waiting for the scene depth buffer"); return nullptr; }
    Cam cur; { AcquireSRWLockShared(&g_camLock); cur = g_ring[g_seq & 7][eye]; ReleaseSRWLockShared(&g_camLock); }
    if (!cur.ok || cur.seq != g_seq) { LOG_FIRST(4, "dlss: no camera for frame %ld eye %d", g_seq, eye); return nullptr; }
    bool inv = cur.P[11] != 0.f && (cur.P[10] / cur.P[11]) < 0.5f;   // where far lands in the depth buffer
    if (!g_w) g_inverted = inv;
    LARGE_INTEGER t0, t1, qf; QueryPerformanceCounter(&t0);
    ID3DDeviceContextState* old = nullptr; g_ctx->SwapDeviceContextState(g_iso, &old); g_rtBypass = true;
    struct Back { ID3DDeviceContextState* o; ~Back() { g_ctx->SwapDeviceContextState(o, nullptr); if (o) o->Release(); g_rtBypass = false; } } back{ old };
    UINT ow = sd.Width, oh = sd.Height;
    if (g_quality && g_cfgOutW >= sd.Width && g_cfgOutH >= sd.Height && g_cfgOutW <= sd.Width * 3) { ow = g_cfgOutW; oh = g_cfgOutH; }
    if ((sd.Width != g_w || sd.Height != g_h || ow != g_ow || oh != g_oh || inv != g_inverted) && !g_dead) { g_inverted = inv; if (!MakeSized(sd.Width, sd.Height, ow, oh)) { if (!g_dead) Dead("work textures", 0); return nullptr; } InterlockedExchange(&g_resetEye[0], 1); InterlockedExchange(&g_resetEye[1], 1); }
    if (g_dead) return nullptr;
    ID3D11ShaderResourceView* dv = DepthView(); if (!dv) { Dead("scene depth not readable", 0); return nullptr; }
    bool reset = InterlockedExchange(&g_resetEye[eye], 0) != 0; if (reset) g_havePrev[eye] = false;
    // previous clip position of every pixel: ndc -> world (inverse of this frame's view-projection) -> last frame's clip
    double vpc[16], vpp[16], ivc[16], M[16];
    ViewProj(cur, vpc); ViewProj(g_havePrev[eye] ? g_prev[eye] : cur, vpp);
    if (!Inv4(vpc, ivc)) { LOG_FIRST(2, "dlss: camera matrix not invertible"); return nullptr; }
    MulD(ivc, vpp, M);
    struct { float M[16]; float size[2]; float inverted, pad; } cb;
    for (int i = 0; i < 16; i++) cb.M[i] = (float)M[i];
    cb.size[0] = (float)sd.Width; cb.size[1] = (float)sd.Height; cb.inverted = g_inverted ? 1.f : 0.f; cb.pad = 0;
    g_ctx->UpdateSubresource(g_cb, 0, nullptr, &cb, 0, 0);
    D3D11_VIEWPORT vp = { 0, 0, (float)sd.Width, (float)sd.Height, 0, 1 };
    ID3D11RenderTargetView* rts[2] = { g_depthFR, g_mvR };
    g_ctx->OMSetRenderTargets(2, rts, nullptr); g_ctx->RSSetViewports(1, &vp);
    g_ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST); g_ctx->IASetInputLayout(nullptr);
    g_ctx->VSSetShader(g_vs, nullptr, 0); g_ctx->PSSetShader(g_psPrep, nullptr, 0);
    g_ctx->PSSetConstantBuffers(0, 1, &g_cb); g_ctx->PSSetShaderResources(0, 1, &dv);
    g_ctx->Draw(3, 0);
    ID3D11RenderTargetView* none[2] = {}; g_ctx->OMSetRenderTargets(2, none, nullptr);
    ID3D11ShaderResourceView* nos[2] = {}; g_ctx->PSSetShaderResources(0, 2, nos);
    g_prev[eye] = cur; g_havePrev[eye] = true;
    if (g_evals < 2) Log("dlss: eye %d frame %ld: depth %s, P10 %.5f P11 %.1f P14 %.5f, jitter %.3f %.3f, first frame %d", eye, g_seq, g_inverted ? "reversed" : "normal", cur.P[10], cur.P[11], cur.P[14], cur.jx, cur.jy, (int)reset);
    if (mode == 3) {
        ID3D11ShaderResourceView* in[2] = { g_depthFS, g_mvS };
        g_ctx->OMSetRenderTargets(1, &g_vizR, nullptr); g_ctx->PSSetShader(g_psViz, nullptr, 0); g_ctx->PSSetShaderResources(0, 2, in);
        g_ctx->Draw(3, 0);
        g_ctx->OMSetRenderTargets(2, none, nullptr); g_ctx->PSSetShaderResources(0, 2, nos);
        g_evals++; return g_viz;
    }
    g_stage = "ngx evaluate";
    NVSDK_NGX_D3D11_DLSS_Eval_Params ep = {};
    ep.Feature.pInColor = src; ep.Feature.pInOutput = g_out[eye]; ep.Feature.InSharpness = 0.f;
    ep.pInDepth = g_depthF; ep.pInMotionVectors = g_mv;
    ep.InRenderSubrectDimensions.Width = sd.Width; ep.InRenderSubrectDimensions.Height = sd.Height;
    ep.InMVScaleX = 1.f; ep.InMVScaleY = 1.f; ep.InReset = reset ? 1 : 0;
    ep.InJitterOffsetX = cur.jx * (float)g_jitSign[0]; ep.InJitterOffsetY = cur.jy * (float)g_jitSign[1];   // 083: same direction as the image shift (082 test: variant 4)   // 082: direction told to NGX, Shift+F8 cycles the four variants
    ep.InPreExposure = 1.f; ep.InExposureScale = 1.f;
    bool fault = false; NVSDK_NGX_Result r = NgxEval(g_ctx, g_feat[eye], g_params, &ep, &fault);
    if (fault || NVSDK_NGX_FAILED(r)) { Dead(fault ? "NGX crashed" : "DLSS evaluate failed", (long)r); return nullptr; }
    QueryPerformanceCounter(&t1); QueryPerformanceFrequency(&qf);
    g_ms += (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / (double)qf.QuadPart;
    if (++g_evals % 3600 == 0) { Log("dlss: %lld eye images, %.3f ms CPU each on the render thread", g_evals, g_ms / 3600.0); g_ms = 0; }
    return g_out[eye];
}

// start-up: reads the settings; w/h come in as the headset image size and go out as the size the game must render at
void DlssConfigure(const wchar_t* ini, int* w, int* h) {
    g_mode = GetPrivateProfileIntW(L"VR", L"Dlss", 0, ini); if (g_mode < 0 || g_mode > 3) g_mode = 0;
    g_quality = GetPrivateProfileIntW(L"VR", L"DlssQuality", 0, ini); if (g_quality < 0 || g_quality > 4) g_quality = 0;
    g_preset = GetPrivateProfileIntW(L"VR", L"DlssPreset", 11, ini);
    g_jitSign[0] = GetPrivateProfileIntW(L"VR", L"DlssJitterFlipX", 0, ini) ? -1 : 1;
    g_jitSign[1] = GetPrivateProfileIntW(L"VR", L"DlssJitterFlipY", 0, ini) ? -1 : 1;
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"DlssLodBias", L"auto", b, 32, ini); if (b[0] != L'a' && b[0] != L'A') { g_lodAuto = false; g_lodBias = (float)_wtof(b); if (g_lodBias > 0.f || g_lodBias < -3.f) g_lodBias = 0.f; } }
    if (!g_mode) g_quality = 0;   // upscaling only when DLSS is on at start (the game's render size is fixed at start)
    if (g_quality && *w > 0 && *h > 0) {
        g_cfgOutW = (UINT)*w; g_cfgOutH = (UINT)*h;
        float sc = kScale[g_quality];
        *w = ((int)((float)*w * sc + 0.5f) + 1) & ~1; *h = ((int)((float)*h * sc + 0.5f) + 1) & ~1;
        g_phases = (int)(8.f / (sc * sc) + 0.999f); if (g_phases > 64) g_phases = 64;
        if (g_lodAuto) g_lodBias = log2f(sc);   // 084: sharper texture levels to match the smaller render size
    }
    Log("dlss: mode %d (0 off, 1 on, 2 on without jitter, 3 input view; Ctrl+F8 cycles), quality %d (%s), game renders %dx%d, headset image %ux%u, %d jitter positions",
        g_mode, g_quality, kQName[g_quality], *w, *h, g_cfgOutW ? g_cfgOutW : (UINT)*w, g_cfgOutH ? g_cfgOutH : (UINT)*h, g_phases);
}
const char* DlssCycle() {
    static char nm[4][64];
    snprintf(nm[0], 64, "DLSS OFF (GAME IMAGE)"); snprintf(nm[1], 64, "%s ON", kQName[g_quality]); snprintf(nm[2], 64, "%s ON, NO JITTER (TEST)", kQName[g_quality]); snprintf(nm[3], 64, "DLSS INPUT VIEW (DEPTH + MOTION)");
    if (g_dead) return "DLSS UNAVAILABLE (SEE SDVR.LOG)";
    int m = (g_mode + 1) % 4; g_mode = m; InterlockedExchange(&g_resetEye[0], 1); InterlockedExchange(&g_resetEye[1], 1);
    return nm[m];
}
// 082: jitter direction test - the projection shift stays, the direction reported to DLSS changes
const char* DlssJitterCycle() {
    static int v = -1; static char t[64];
    if (v < 0) v = (g_jitSign[0] < 0 ? 1 : 0) | (g_jitSign[1] < 0 ? 2 : 0);
    v = (v + 1) & 3; g_jitSign[0] = (v & 1) ? -1 : 1; g_jitSign[1] = (v & 2) ? -1 : 1;
    InterlockedExchange(&g_resetEye[0], 1); InterlockedExchange(&g_resetEye[1], 1);
    snprintf(t, sizeof t, "JITTER VARIANT %d OF 4%s", v + 1, g_mode == 1 ? "" : " (JITTER IS OFF)");
    return t;
}

// 084: texture LOD bias for the upscaling modes. Every sampler the game binds for pixel shaders is swapped for a copy with
// the bias added (comparison samplers = shadow maps are left alone). Copies are made once per sampler and kept.
struct SampE { ID3D11SamplerState* in, *out; };
static SampE g_samp[512]; static volatile LONG g_sampN; static LONG64 g_sampSwaps;
float DlssLodBias() { return (g_lodOn && !g_dead && g_mode && g_mode != 3 && g_quality) ? g_lodBias : 0.f; }
ID3D11SamplerState* DlssSampler(ID3D11SamplerState* s) {
    if (!s) return s;
    UINT h = (UINT)(((uintptr_t)s >> 4) * 2654435761u) & 511;
    for (int k = 0; k < 512; k++) {
        SampE& e = g_samp[(h + k) & 511];
        if (e.in == s) { g_sampSwaps++; return e.out; }
        if (!e.in) {
            if (g_sampN >= 400) return s;
            D3D11_SAMPLER_DESC d; s->GetDesc(&d);
            ID3D11SamplerState* o = s;
            if (!(d.Filter & 0x80) && d.MaxLOD > 0.f) {   // not a comparison sampler, uses mip levels
                ID3D11Device* dev = nullptr; s->GetDevice(&dev);
                d.MipLODBias += g_lodBias; ID3D11SamplerState* n = nullptr;
                if (dev && SUCCEEDED(dev->CreateSamplerState(&d, &n)) && n) o = n;
                if (dev) dev->Release();
            }
            s->AddRef();   // the address stays ours
            e.out = o; e.in = s; LONG c = InterlockedIncrement(&g_sampN);
            if (c <= 3 || c % 16 == 0) Log("dlss: texture LOD bias %.2f: %ld samplers seen (filter 0x%x, aniso %u%s)", g_lodBias, c, (unsigned)d.Filter, d.MaxAnisotropy, o == s ? ", unchanged" : "");
            return o;
        }
    }
    return s;
}
const char* DlssLodCycle() {
    static char t[64]; g_lodOn = !g_lodOn;
    if (!g_quality) return "TEXTURE SHARPENING: NOT USED IN DLAA";
    snprintf(t, sizeof t, g_lodOn ? "TEXTURE LOD BIAS ON (%.2f)" : "TEXTURE LOD BIAS OFF", g_lodBias); return t;
}
// 089: in-game menu
bool DlssDead() { return g_dead; }
int DlssQualityActive() { return g_quality; }
void DlssSetOn(bool on) { if (g_dead) return; int m = on ? 1 : 0; if ((g_mode != 0) != on) { g_mode = m; InterlockedExchange(&g_resetEye[0], 1); InterlockedExchange(&g_resetEye[1], 1); } }
