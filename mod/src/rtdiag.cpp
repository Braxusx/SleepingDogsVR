// SDVR build 025 - render target census: while armed, every OMSetRenderTargets on the immediate context that binds one of
// the eye textures is logged once per unique call stack (game RVAs), to find what draws into the eye images besides the
// eye copy and the UI (the minimap content).
#include <windows.h>
#include <d3d11.h>
#include <stdint.h>
#include <stdio.h>
#include "MinHook.h"
#include "log.h"
#include <string.h>
#include <stdlib.h>
#include <math.h>
extern uintptr_t g_base;
extern volatile int g_stateFix; extern volatile LONG g_stateGen;
typedef void (STDMETHODCALLTYPE *OMSet_t)(ID3D11DeviceContext*, UINT, ID3D11RenderTargetView* const*, ID3D11DepthStencilView*);
static OMSet_t oOMSet;
static volatile LONG g_armed = 0; static LONG g_bindIdx = 0;
void RtDiagNewFrame() { g_bindIdx = 0; }
static ID3D11Resource* g_eyes[2];
struct Site { uintptr_t s[8]; LONG n; LONG order; LONG inUi; };
static Site g_sites[64]; static LONG g_nSites; static SRWLOCK g_lock = SRWLOCK_INIT;
// build 026: during the UI render, Scaleform binds the eye texture again itself (its render-target stack, e.g. after the
// minimap's masked / filtered pass): those binds are redirected to the mod's HUD target.
static ID3D11RenderTargetView* volatile g_uiRtv; static volatile LONG g_uiRedirects;
void RtUiRedirect(ID3D11RenderTargetView* hud) { g_uiRtv = hud; }
LONG RtUiRedirectCount() { return g_uiRedirects; }

// build 043: additive / screen blending on the transparent HUD target. Glow sprites (e.g. the full Face/heat meter) are
// drawn with color += src (DestBlend ONE). On the flat screen the alpha they write is irrelevant; on the HUD target it
// made each glow's whole square sprite opaque (dark squares in the headset). While the HUD target is bound, such blend
// states get an alpha path that keeps the target's alpha; with premultiplied compositing of the HUD layer the glow's
// colour is then simply added over the world, as on the flat screen.
typedef void (STDMETHODCALLTYPE *Blend_t)(ID3D11DeviceContext*, ID3D11BlendState*, const FLOAT[4], UINT);
static Blend_t oBlend;
static bool g_haveBlend; static ID3D11BlendState* g_curBlend; static FLOAT g_curFactor[4] = {1, 1, 1, 1}; static UINT g_curMask = 0xffffffff;
static ID3D11RenderTargetView* volatile g_hudRtvPtr; static bool g_rtIsHud; static volatile int g_glowFix = 1;
static ID3D11BlendState* g_bsFrom[64]; static ID3D11BlendState* g_bsTo[64]; static int g_nBs; static volatile LONG g_glowFixed;
void RtSetHudRtv(ID3D11RenderTargetView* v, int glowFix) { g_hudRtvPtr = v; g_glowFix = glowFix; }
LONG RtGlowFixCount() { return g_glowFixed; }
static ID3D11BlendState* HudBlend(ID3D11DeviceContext* c, ID3D11BlendState* bs) {
    if (!bs || !g_glowFix) return bs;
    for (int i = 0; i < g_nBs; i++) if (g_bsFrom[i] == bs) return g_bsTo[i];
    D3D11_BLEND_DESC d; bs->GetDesc(&d);
    D3D11_RENDER_TARGET_BLEND_DESC& r = d.RenderTarget[0];
    ID3D11BlendState* out = bs;
    bool additive = r.BlendEnable && r.BlendOp == D3D11_BLEND_OP_ADD && (r.DestBlend == D3D11_BLEND_ONE || r.DestBlend == D3D11_BLEND_INV_SRC_COLOR);
    if (additive && !(r.SrcBlendAlpha == D3D11_BLEND_ZERO && r.DestBlendAlpha == D3D11_BLEND_ONE)) {
        Log("glow: HUD blend src %d dst %d op %d | alpha src %d dst %d op %d, mask 0x%x -> alpha kept", r.SrcBlend, r.DestBlend, r.BlendOp, r.SrcBlendAlpha, r.DestBlendAlpha, r.BlendOpAlpha, r.RenderTargetWriteMask);
        for (int i = 0; i < 8; i++) if (d.RenderTarget[i].BlendEnable || i == 0) { d.RenderTarget[i].SrcBlendAlpha = D3D11_BLEND_ZERO; d.RenderTarget[i].DestBlendAlpha = D3D11_BLEND_ONE; d.RenderTarget[i].BlendOpAlpha = D3D11_BLEND_OP_ADD; }
        ID3D11Device* dev = nullptr; c->GetDevice(&dev);
        ID3D11BlendState* nb = nullptr;
        if (dev && SUCCEEDED(dev->CreateBlendState(&d, &nb)) && nb) out = nb;
        if (dev) dev->Release();
    }
    if (g_nBs < 64) { g_bsFrom[g_nBs] = bs; g_bsTo[g_nBs] = out; bs->AddRef(); g_nBs++; }
    return out;
}
static void STDMETHODCALLTYPE hkBlend(ID3D11DeviceContext* c, ID3D11BlendState* bs, const FLOAT f[4], UINT mask) {
    g_haveBlend = true; g_curBlend = bs; if (f) memcpy(g_curFactor, f, sizeof g_curFactor); g_curMask = mask;
    ID3D11BlendState* use = g_rtIsHud ? HudBlend(c, bs) : bs;
    if (use != bs) InterlockedIncrement(&g_glowFixed);
    oBlend(c, use, f, mask);
}
static void TrackHudBind(ID3D11DeviceContext* c, ID3D11RenderTargetView* rt0) {
    bool hud = rt0 && rt0 == g_hudRtvPtr;
    if (hud == g_rtIsHud) return;
    g_rtIsHud = hud;
    if (!oBlend || !g_haveBlend) return;
    oBlend(c, hud ? HudBlend(c, g_curBlend) : g_curBlend, g_curFactor, g_curMask);   // re-apply the current state for the new target
}
static void OlRt(UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
static void DumpOnBind(ID3D11DeviceContext* c, ID3D11RenderTargetView* next, UINT n, ID3D11DepthStencilView* dsv, ID3D11RenderTargetView* const* rtv);
static void DumpAfter(ID3D11DeviceContext* c, UINT cnt);
static void GbReport();
// build 037: minimap. After the UI render the game's next pass binds the eye texture again and draws the minimap map
// (4 draws) with a full-target viewport, using the 16:9 stage coordinates of the UI as if they filled the whole target
// (an oval, too low). While armed: that eye bind goes to the HUD target and its viewport becomes the 16:9 stage area the
// UI is shown in; the first other bind after it ends the window and hands the HUD to the headset.
static ID3D11RenderTargetView* volatile g_mmHud; static bool g_mmIn; static void (*g_mmFlush)(); static float g_mmAspect = 16.f / 9.f;
static volatile LONG g_mmRedirects; static volatile int g_mmFitOn = 1;
void RtMinimapFit(int on) { g_mmFitOn = on; }
void RtMinimapSetup(void (*flush)(), float aspect) { g_mmFlush = flush; if (aspect > 0.5f && aspect < 4.f) g_mmAspect = aspect; }
void RtMinimapArm(ID3D11RenderTargetView* hud) { g_mmHud = hud; g_mmIn = false; }
LONG RtMinimapRedirects() { return g_mmRedirects; }
static ID3D11RenderTargetView* g_rt0;
extern bool g_rtBypass;
float DlssLodBias(); ID3D11SamplerState* DlssSampler(ID3D11SamplerState* s);
typedef void (STDMETHODCALLTYPE *PSSamp_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11SamplerState* const*);
static PSSamp_t oPSSamp;
static void STDMETHODCALLTYPE hkPSSamp(ID3D11DeviceContext* c, UINT slot, UINT n, ID3D11SamplerState* const* s) {
    if (g_rtBypass || !s || n == 0 || n > 16 || DlssLodBias() == 0.f) { oPSSamp(c, slot, n, s); return; }
    ID3D11SamplerState* r[16]; for (UINT i = 0; i < n; i++) r[i] = DlssSampler(s[i]);
    oPSSamp(c, slot, n, r);
}
extern bool g_rtBypass; void DlssNoteTargets(UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv);
static void STDMETHODCALLTYPE hkOMSet(ID3D11DeviceContext* c, UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (g_rtBypass) { oOMSet(c, n, rtv, dsv); return; }   // 079: the mod's own DLSS passes
    DlssNoteTargets(n, rtv, dsv);
    if (g_stateFix == 2) InterlockedIncrement(&g_stateGen);
    OlRt(n, rtv, dsv);
    DumpOnBind(c, n && rtv ? rtv[0] : nullptr, n, dsv, rtv);
    g_rt0 = n && rtv ? rtv[0] : nullptr;   // 046 (the redirected target is recorded as given by the game)
    if (g_mmHud) {
        ID3D11Resource* r = nullptr; if (n >= 1 && n <= 8 && rtv && rtv[0]) rtv[0]->GetResource(&r);
        bool eye = r && (r == g_eyes[0] || r == g_eyes[1]);
        if (r) r->Release();
        if (eye) {
            ID3D11RenderTargetView* a[8]; memcpy(a, rtv, n * sizeof(void*)); a[0] = g_mmHud;
            g_mmIn = true; InterlockedIncrement(&g_mmRedirects);
            oOMSet(c, n, a, dsv); TrackHudBind(c, a[0]); return;
        }
        if (r || g_mmIn) { g_mmHud = nullptr; g_mmIn = false; oOMSet(c, n, rtv, dsv); TrackHudBind(c, n && rtv ? rtv[0] : nullptr); if (g_mmFlush) g_mmFlush(); return; }
    }
    if (g_uiRtv && n >= 1 && n <= 8 && rtv && rtv[0]) {
        ID3D11Resource* r = nullptr; rtv[0]->GetResource(&r);
        bool eye = r && (r == g_eyes[0] || r == g_eyes[1]);
        if (r) r->Release();
        if (eye) {
            ID3D11RenderTargetView* a[8]; memcpy(a, rtv, n * sizeof(void*)); a[0] = g_uiRtv;
            InterlockedIncrement(&g_uiRedirects);
            oOMSet(c, n, a, dsv); TrackHudBind(c, a[0]); return;
        }
    }
    if (g_armed && n && rtv && rtv[0]) {
        ID3D11Resource* r = nullptr; rtv[0]->GetResource(&r);
        if (r) {
            if (r == g_eyes[0] || r == g_eyes[1]) {
                void* fr[24]; USHORT k = RtlCaptureStackBackTrace(1, 24, fr, nullptr);
                uintptr_t s[8] = {}; int m = 0;
                for (USHORT i = 0; i < k && m < 8; i++) { uintptr_t a = (uintptr_t)fr[i]; if (a >= g_base && a < g_base + 0x3000000) s[m++] = a - g_base; }
                AcquireSRWLockExclusive(&g_lock);
                LONG j = 0; for (; j < g_nSites; j++) if (!memcmp(g_sites[j].s, s, sizeof s)) break;
                if (j == g_nSites && g_nSites < 64) { memcpy(g_sites[j].s, s, sizeof s); g_sites[j].n = 0; g_sites[j].order = g_bindIdx; g_sites[j].inUi = g_uiRtv != nullptr; g_nSites++; }
                g_bindIdx++;
                if (j < 64) g_sites[j].n++;
                ReleaseSRWLockExclusive(&g_lock);
            }
            r->Release();
        }
    }
    oOMSet(c, n, rtv, dsv);
    TrackHudBind(c, n && rtv ? rtv[0] : nullptr);
}
void RtDiagInstall(ID3D11DeviceContext* ctx) {
    static bool done = false; if (done || !ctx) return; done = true;
    void* f = (*(void***)ctx)[33];
    if (MH_CreateHook(f, (void*)hkOMSet, (void**)&oOMSet) == MH_OK && MH_EnableHook(f) == MH_OK) Log("rtdiag: OMSetRenderTargets hooked");
    else Log("rtdiag: hook failed");
    void* fb = (*(void***)ctx)[35];
    if (MH_CreateHook(fb, (void*)hkBlend, (void**)&oBlend) == MH_OK && MH_EnableHook(fb) == MH_OK) Log("glow: OMSetBlendState hooked");
    else Log("glow: OMSetBlendState hook failed");
}
static float g_eyeW, g_eyeH;
void RtDiagSetEyes(ID3D11Resource* a, ID3D11Resource* b) {
    g_eyes[0] = a; g_eyes[1] = b;
    ID3D11Texture2D* t = nullptr;
    if (a && SUCCEEDED(a->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); g_eyeW = (float)d.Width; g_eyeH = (float)d.Height; t->Release(); }
}
void RtDiagArm(bool on) {
    if (on) { g_nSites = 0; InterlockedExchange(&g_armed, 1); return; }
    InterlockedExchange(&g_armed, 0);
    Log("rtdiag: %ld call stacks bound an eye texture (count | first seen as bind # in its frame | inside the UI render | game RVAs, innermost first)", g_nSites);
    for (LONG j = 0; j < g_nSites; j++) {
        char b[256]; int o = 0;
        for (int k = 0; k < 8 && g_sites[j].s[k]; k++) o += snprintf(b + o, sizeof b - o, " +0x%llx", (unsigned long long)g_sites[j].s[k]);
        b[o] = 0; Log("  %ld | #%ld | %ld |%s", g_sites[j].n, g_sites[j].order, g_sites[j].inUi, b);
    }
}

// build 036: one-frame outline (F10). Every render-target bind, viewport, scissor, copy and clear on the immediate context
// in one frame, with the number of draws done in between and the game call site, plus markers from the mod (eye passes,
// eye copy, UI render, present). Used to find the pass that puts the minimap into the eye images.
typedef void (STDMETHODCALLTYPE *VP_t)(ID3D11DeviceContext*, UINT, const D3D11_VIEWPORT*);
typedef void (STDMETHODCALLTYPE *SC_t)(ID3D11DeviceContext*, UINT, const D3D11_RECT*);
typedef void (STDMETHODCALLTYPE *DI_t)(ID3D11DeviceContext*, UINT, UINT, INT);
typedef void (STDMETHODCALLTYPE *D_t)(ID3D11DeviceContext*, UINT, UINT);
typedef void (STDMETHODCALLTYPE *DII_t)(ID3D11DeviceContext*, UINT, UINT, UINT, INT, UINT);
typedef void (STDMETHODCALLTYPE *DIn_t)(ID3D11DeviceContext*, UINT, UINT, UINT, UINT);
typedef void (STDMETHODCALLTYPE *CSR_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, UINT, UINT, UINT, ID3D11Resource*, UINT, const D3D11_BOX*);
typedef void (STDMETHODCALLTYPE *CR_t)(ID3D11DeviceContext*, ID3D11Resource*, ID3D11Resource*);
typedef void (STDMETHODCALLTYPE *CL_t)(ID3D11DeviceContext*, ID3D11RenderTargetView*, const FLOAT[4]);
static VP_t oVP; static SC_t oSC; static DI_t oDI; static D_t oD; static DII_t oDII; static DIn_t oDIn; static CSR_t oCSR; static CR_t oCR; static CL_t oCL;
static volatile LONG g_ol = 0;   // 1 = armed for the next frame, 2 = recording
static LONG g_olDraws;
struct OlE { char k; int a, b, c, d; void* r0; void* r1; uintptr_t s[4]; LONG draws; char m[24]; };
static OlE g_olE[3000]; static int g_olN;
static void* g_res[4096]; static int g_resW[4096], g_resH[4096], g_resF[4096], g_nRes;   // 046: 4096 entries, hashed
static short g_resHash[8192];
static int ResId(void* r) {
    if (!r) return -1;
    if (g_nRes == 0) memset(g_resHash, 0xff, sizeof g_resHash);
    unsigned h = (unsigned)(((uintptr_t)r >> 4) * 2654435761u) & 8191;
    while (g_resHash[h] >= 0) { if (g_res[g_resHash[h]] == r) return g_resHash[h]; h = (h + 1) & 8191; }
    if (g_nRes >= 4095) return 4095;
    int i = g_nRes++; g_res[i] = r; g_resHash[h] = (short)i; g_resW[i] = g_resH[i] = g_resF[i] = 0;
    ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(((ID3D11Resource*)r)->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t)) && t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); g_resW[i] = d.Width; g_resH[i] = d.Height; g_resF[i] = d.Format; t->Release(); }
    return i;
}
static OlE* OlAdd(char k) {
    if (g_ol != 2 || g_olN >= 3000) return nullptr;
    OlE& e = g_olE[g_olN++]; memset(&e, 0, sizeof e); e.k = k; e.draws = g_olDraws; g_olDraws = 0;
    void* fr[16]; USHORT n = RtlCaptureStackBackTrace(2, 16, fr, nullptr); int m = 0;
    for (USHORT i = 0; i < n && m < 4; i++) { uintptr_t a = (uintptr_t)fr[i]; if (a >= g_base && a < g_base + 0x3000000) e.s[m++] = a - g_base; }
    return &e;
}

// build 046: eye pass diff (F10). For every draw in the recorded frame: pixel shader, render target and the textures
// bound to pixel shader slots 0-15. Afterwards the first eye pass is compared draw by draw with the second, to find a
// texture that one pass samples and the other does not (the lanterns / signs / vending machine are dark in pass 0).
typedef void (STDMETHODCALLTYPE *PSSRV_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11ShaderResourceView* const*);
typedef void (STDMETHODCALLTYPE *PSSh_t)(ID3D11DeviceContext*, ID3D11PixelShader*, ID3D11ClassInstance* const*, UINT);
static PSSRV_t oPSSRV; static PSSh_t oPSSh;
static ID3D11ShaderResourceView* g_srv[16]; static void* g_ps; static void* g_vs;
struct DD { void* ps; short rt; short t[16]; };
static void* g_skipC[32]; static int g_nSkipC; static void* volatile g_skipPs; static int g_skipI = -1;
static DD g_dd[30000]; static int g_nDD; static int g_passAt[8]; static int g_nPassAt; static int g_frameB = -1, g_olFrames, g_frameBMarks;
static void STDMETHODCALLTYPE hkPSSRV(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11ShaderResourceView* const* v) {
    for (UINT i = 0; i < n && st + i < 16; i++) g_srv[st + i] = v ? v[i] : nullptr;
    oPSSRV(c, st, n, v);
}
static void STDMETHODCALLTYPE hkPSSh(ID3D11DeviceContext* c, ID3D11PixelShader* ps, ID3D11ClassInstance* const* ci, UINT n) { g_ps = ps; oPSSh(c, ps, ci, n); }
typedef void (STDMETHODCALLTYPE *VSSh_t)(ID3D11DeviceContext*, ID3D11VertexShader*, ID3D11ClassInstance* const*, UINT);
static VSSh_t oVSSh;
static void STDMETHODCALLTYPE hkVSSh(ID3D11DeviceContext* c, ID3D11VertexShader* vs, ID3D11ClassInstance* const* ci, UINT n) { g_vs = vs; oVSSh(c, vs, ci, n); }
static short ViewRes(ID3D11View* v) { if (!v) return -1; ID3D11Resource* r = nullptr; v->GetResource(&r); short id = (short)ResId(r); if (r) r->Release(); return id; }
static void DiffDraw() {
    if (g_nDD >= 30000) return;
    DD& d = g_dd[g_nDD++]; d.ps = g_ps; d.rt = ViewRes(g_rt0);
    for (int i = 0; i < 16; i++) d.t[i] = ViewRes(g_srv[i]);
}
static void DiffMarkPass() { if (g_nPassAt < 8) g_passAt[g_nPassAt++] = g_nDD; }

// build 050: same pass in two consecutive frames (same view, same draw order). In daylight the first eye's lanterns
// are dark only on every second frame - a resource that alternates per frame. Draw i of pass P in frame A is compared
// with draw i of pass P in frame B; texture slots that differ are listed (most frequent first).
static void DiffFrames(const char* label, int x0, int x1, int y0, int y1) {
    Log("frames: %s - frame A draws %d, frame B draws %d", label, x1 - x0, y1 - y0);
    struct Ag { void* ps; int slot; short r0, r1; int n; int first; };
    static Ag ag[600]; int nag = 0, mism = 0;
    int m = (x1 - x0) < (y1 - y0) ? (x1 - x0) : (y1 - y0);
    for (int j = 0; j < m; j++) {
        DD& a = g_dd[x0 + j]; DD& b = g_dd[y0 + j];
        if (a.ps != b.ps) { if (++mism <= 3) Log("frames:   draw %d: different shader (%p / %p) - order changed from here", j, a.ps, b.ps); if (mism > 40) break; continue; }
        for (int sl = -1; sl < 16; sl++) {
            short r0 = sl < 0 ? a.rt : a.t[sl], r1 = sl < 0 ? b.rt : b.t[sl];
            if (r0 == r1) continue;
            int q = 0; for (; q < nag; q++) if (ag[q].ps == a.ps && ag[q].slot == sl && ag[q].r0 == r0 && ag[q].r1 == r1) break;
            if (q == nag && nag < 600) { ag[q] = { a.ps, sl, r0, r1, 0, j }; nag++; }
            if (q < 600) ag[q].n++;
        }
    }
    auto tx = [](short id, char* b, int n) { if (id < 0) snprintf(b, n, "none"); else snprintf(b, n, "#%d %dx%d f%d", id, g_resW[id], g_resH[id], g_resF[id]); };
    for (int pass = 0; pass < 2; pass++) for (int q = 0; q < nag; q++) {
        bool rtLike = ag[q].r0 >= 0 && g_resW[ag[q].r0] >= 256 && g_resW[ag[q].r0] == g_resW[ag[q].r1 >= 0 ? ag[q].r1 : ag[q].r0];
        if ((pass == 0) != rtLike) continue;   // same-size swaps (ping-pong render targets) first
        char x[64], y[64]; tx(ag[q].r0, x, 64); tx(ag[q].r1, y, 64);
        Log("frames:   shader %p slot %d: A %s | B %s | %d draws from %d", ag[q].ps, ag[q].slot, x, y, ag[q].n, ag[q].first);
    }
    Log("frames: %s - %d differing slot bindings, %d shader mismatches", label, nag, mism);
    // 051: every use of the textures that swap render-target roles between the frames (ping-pong pairs), in frame A
    for (int q = 0; q < nag; q++) {
        if (ag[q].slot != -1) continue;
        for (int k = 0; k < 2; k++) {
            short id = k ? ag[q].r1 : ag[q].r0; if (id < 0) continue;
            bool seen = false; for (int z = 0; z < q; z++) if (ag[z].slot == -1 && (ag[z].r0 == id || ag[z].r1 == id)) seen = true;
            if (seen) continue;
            Log("frames:   uses of #%d %dx%d f%d in frame A (pass 0 = draws 0..%d, pass 1 = ..%d, after = ..%d):", id, g_resW[id], g_resH[id], g_resF[id], g_passAt[0], g_passAt[1], g_frameB);
            int shown = 0;
            for (int i = 0; i < g_frameB && shown < 60; i++) {
                DD& d = g_dd[i]; const char* ps = i < g_passAt[0] ? "pass0" : i < g_passAt[1] ? "pass1" : "after";
                int rel = i < g_passAt[0] ? i : i < g_passAt[1] ? i - g_passAt[0] : i - g_passAt[1];
                if (d.rt == id) { Log("frames:     %s draw %d: WRITE (shader %p)", ps, rel, d.ps); shown++; }
                for (int sl = 0; sl < 16; sl++) if (d.t[sl] == id) { Log("frames:     %s draw %d: read slot %d (shader %p, target #%d %dx%d)", ps, rel, sl, d.ps, d.rt, d.rt >= 0 ? g_resW[d.rt] : 0, d.rt >= 0 ? g_resH[d.rt] : 0); shown++; }
            }
        }
    }
}
static void DiffReport() {
    // passes: [0, eye copy 0) and [eye copy 0 end, eye copy 1); marks: 0 = eye copy 0, 1 = eye copy 1
    g_nSkipC = 0; g_skipI = -1; g_skipPs = nullptr;
    if (g_nPassAt < 2) { Log("diff: pass marks missing (%d)", g_nPassAt); return; }
    int a0 = 0, a1 = g_passAt[0], b0 = g_passAt[0], b1 = g_passAt[1];
    Log("diff: pass 0 = draws %d..%d (%d), pass 1 = draws %d..%d (%d)", a0, a1, a1 - a0, b0, b1, b1 - b0);
    // per pixel shader: i-th draw in pass 0 vs i-th in pass 1
    struct Agg { void* ps; int slot; short r0, r1; int n; int first; };
    static Agg ag[400]; int nag = 0;
    static void* pss[2000]; int nps = 0;
    for (int i = a0; i < b1; i++) { void* p = g_dd[i].ps; int k = 0; for (; k < nps; k++) if (pss[k] == p) break; if (k == nps && nps < 2000) pss[nps++] = p; }
    int only0 = 0, only1 = 0;
    for (int k = 0; k < nps; k++) {
        int ia[3000], ib[3000], na = 0, nb = 0;
        for (int i = a0; i < a1 && na < 3000; i++) if (g_dd[i].ps == pss[k]) ia[na++] = i;
        for (int i = b0; i < b1 && nb < 3000; i++) if (g_dd[i].ps == pss[k]) ib[nb++] = i;
        if (na != nb) {
            Log("diff: shader %p draws pass0 %d, pass1 %d (first at pass-relative %d / %d)", pss[k], na, nb, na ? ia[0] - a0 : -1, nb ? ib[0] - b0 : -1); if (!na) only1++; if (!nb) only0++;
            DD& f = g_dd[na ? ia[0] : ib[0]]; char b[400]; int o = 0;
            o += snprintf(b + o, sizeof b - o, "rt #%d %dx%d f%d |", f.rt, f.rt >= 0 ? g_resW[f.rt] : 0, f.rt >= 0 ? g_resH[f.rt] : 0, f.rt >= 0 ? g_resF[f.rt] : 0);
            for (int sl = 0; sl < 8; sl++) if (f.t[sl] >= 0) o += snprintf(b + o, sizeof b - o, " t%d #%d %dx%d f%d", sl, f.t[sl], g_resW[f.t[sl]], g_resH[f.t[sl]], g_resF[f.t[sl]]);
            Log("        first draw: %s", b);
            if (g_nSkipC < 32 && !(na == 0 && ib[0] - b0 < 40)) g_skipC[g_nSkipC++] = pss[k];   // test candidates (not the UI at the start of pass 1)
        }
        int m = na < nb ? na : nb;
        for (int j = 0; j < m; j++) for (int sl = -1; sl < 16; sl++) {
            short r0 = sl < 0 ? g_dd[ia[j]].rt : g_dd[ia[j]].t[sl], r1 = sl < 0 ? g_dd[ib[j]].rt : g_dd[ib[j]].t[sl];
            if (r0 == r1) continue;
            int q = 0; for (; q < nag; q++) if (ag[q].ps == pss[k] && ag[q].slot == sl && ag[q].r0 == r0 && ag[q].r1 == r1) break;
            if (q == nag && nag < 400) { ag[q].ps = pss[k]; ag[q].slot = sl; ag[q].r0 = r0; ag[q].r1 = r1; ag[q].n = 0; ag[q].first = ia[j] - a0; nag++; }
            if (q < 400) ag[q].n++;
        }
    }
    // textures sampled in a pass before that pass renders into them (they hold older data at that point)
    for (int pass = 0; pass < 2; pass++) {
        int s0 = pass ? b0 : a0, s1 = pass ? b1 : a1;
        static int firstR[4096], firstW[4096]; static void* rps[4096];
        for (int i = 0; i < g_nRes; i++) { firstR[i] = firstW[i] = -1; }
        for (int i = s0; i < s1; i++) {
            DD& d = g_dd[i];
            if (d.rt >= 0 && firstW[d.rt] < 0) firstW[d.rt] = i - s0;
            for (int sl = 0; sl < 16; sl++) if (d.t[sl] >= 0 && firstR[d.t[sl]] < 0) { firstR[d.t[sl]] = i - s0; rps[d.t[sl]] = d.ps; }
        }
        int cnt = 0;
        for (int i = 0; i < g_nRes && cnt < 60; i++) if (firstR[i] >= 0 && firstW[i] > firstR[i]) {
            cnt++; Log("diff: pass %d samples #%d %dx%d f%d at draw %d (shader %p) before rendering into it at draw %d", pass, i, g_resW[i], g_resH[i], g_resF[i], firstR[i], rps[i], firstW[i]);
        }
        Log("diff: pass %d - %d textures sampled before being rendered in the same pass", pass, cnt);
    }
    Log("diff: %d shaders, %d only in pass 0, %d only in pass 1; %d differing bindings (slot -1 = render target):", nps, only0, only1, nag);
    auto tx = [](short id, char* b, int n) { if (id < 0) snprintf(b, n, "none"); else snprintf(b, n, "#%d %dx%d f%d%s", id, g_resW[id], g_resH[id], g_resF[id], g_res[id] == g_eyes[0] ? " LEFTEYE" : g_res[id] == g_eyes[1] ? " RIGHTEYE" : ""); };
    for (int q = 0; q < nag; q++) { char x[64], y[64]; tx(ag[q].r0, x, 64); tx(ag[q].r1, y, 64);
        Log("  shader %p slot %d: pass0 %s | pass1 %s | %d draws, first at pass-relative %d", ag[q].ps, ag[q].slot, x, y, ag[q].n, ag[q].first); }
}

// build 055: render target dump (Shift+F10). For one frame, every render target the game finishes drawing into (when
// it is unbound after at least one draw) is saved as a quarter-size BMP, tagged with the eye pass it belongs to:
// sdvr_dumpNN_p<pass>_<seq>_<id>_<WxH>_f<format>.bmp. Comparing the same target between pass 0 and pass 1 shows at
// which stage the first eye's image starts to differ (dark lanterns, green table).
static volatile LONG g_dump;   // 1 armed, 2 recording
static int g_dumpPass, g_dumpSeq[3], g_dumpN; static LONG g_drawsBound; static ID3D11RenderTargetView* g_dumpRt;
static wchar_t g_dumpDir[MAX_PATH];
void RtDumpInit(const wchar_t* dir) { wcsncpy(g_dumpDir, dir, MAX_PATH - 1); }
void RtDumpArm() { InterlockedExchange(&g_dump, 1); }
static inline float H2F(uint16_t h) { uint32_t s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023; uint32_t f;
    if (e == 0) f = s << 31; else if (e == 31) f = (s << 31) | 0x7f800000; else f = (s << 31) | ((e + 112) << 23) | (m << 13); float r; memcpy(&r, &f, 4); return r; }
static inline uint8_t ToByte(float v) { if (!(v > 0)) return 0; v = v / (1.f + v) * 2.f; return v >= 1.f ? 255 : (uint8_t)(v * 255.f); }
static int g_mode;   // 0 dump, 1 locate (061), 2 state (061)
static volatile int g_sfEye = 0;
static struct { void* buf; uintptr_t s[4]; long seq; } g_psSet[4], g_vsSet[4];
static int g_emN[2], g_emWhite[2], g_emFresh[2], g_emSame[2];
static int g_dumpStepOverride;   // 111: small pictures (every 8th pixel) for the per-draw layer snapshots
static void DumpTarget(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const wchar_t* name = nullptr, bool alpha = false) {
    if (g_mode != 0) return;
    ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) return;
    ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    int f = d.Format; bool ok = (f == 28 || f == 29 || f == 27 || f == 87 || f == 91 || f == 10 || f == 26 || f == 24 || f == 41 || f == 61 || f == 2) && d.Width >= 256 && d.SampleDesc.Count == 1;
    if (!ok || (!name && g_dumpSeq[g_dumpPass] >= 60)) { t->Release(); return; }
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    if (f == 27) sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); ID3D11Texture2D* st = nullptr;
    if (!dev || FAILED(dev->CreateTexture2D(&sd, nullptr, &st)) || !st) { if (dev) dev->Release(); t->Release(); return; }
    dev->Release();
    c->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        int step = g_dumpStepOverride ? g_dumpStepOverride : d.Width >= 2048 ? 4 : d.Width >= 1024 ? 2 : 1;
        UINT w = d.Width / step, h = d.Height / step, row = (w * 3 + 3) & ~3u;
        uint8_t* out = (uint8_t*)malloc((size_t)row * h);
        for (UINT y = 0; y < h; y++) {
            const uint8_t* p = (const uint8_t*)m.pData + (size_t)(y * step) * m.RowPitch; uint8_t* o = out + (size_t)(h - 1 - y) * row;
            for (UINT x = 0; x < w; x++) {
                UINT sx = x * step; uint8_t R = 0, G = 0, B = 0;
                if (f == 28 || f == 29 || f == 27) { const uint8_t* q = p + sx * 4; if (alpha) R = G = B = q[3]; else { R = q[0]; G = q[1]; B = q[2]; } }
                else if (f == 87 || f == 91) { const uint8_t* q = p + sx * 4; R = q[2]; G = q[1]; B = q[0]; }
                else if (f == 10) { const uint16_t* q = (const uint16_t*)(p + sx * 8); R = ToByte(H2F(q[0])); G = ToByte(H2F(q[1])); B = ToByte(H2F(q[2])); }
                else if (f == 2) { const float* q = (const float*)(p + sx * 16); R = ToByte(q[0]); G = ToByte(q[1]); B = ToByte(q[2]); }
                else if (f == 26) { uint32_t u = *(const uint32_t*)(p + sx * 4);
                    auto f11 = [](uint32_t v, int mb) { uint32_t e = v >> mb, mm = v & ((1u << mb) - 1); if (!e) return 0.f; uint32_t bits = ((e + 112) << 23) | (mm << (23 - mb)); float r; memcpy(&r, &bits, 4); return r; };
                    R = ToByte(f11(u & 2047, 6)); G = ToByte(f11((u >> 11) & 2047, 6)); B = ToByte(f11(u >> 22, 5)); }
                else if (f == 24) { uint32_t u = *(const uint32_t*)(p + sx * 4); R = (uint8_t)((u & 1023) >> 2); G = (uint8_t)(((u >> 10) & 1023) >> 2); B = (uint8_t)(((u >> 20) & 1023) >> 2); }
                else if (f == 41) { float v = *(const float*)(p + sx * 4); R = G = B = ToByte(v); }
                else if (f == 61) { R = G = B = p[sx]; }
                o[x * 3 + 0] = B; o[x * 3 + 1] = G; o[x * 3 + 2] = R;
            }
        }
        c->Unmap(st, 0);
        int id = ResId(t);
        wchar_t path[MAX_PATH]; if (name) swprintf(path, MAX_PATH, L"%ls\\%ls", g_dumpDir, name); else swprintf(path, MAX_PATH, L"%ls\\sdvr_dump%02d_p%d_%02d_id%d_%ux%u_f%d.bmp", g_dumpDir, g_dumpN, g_dumpPass, g_dumpSeq[g_dumpPass], id, d.Width, d.Height, f);
        FILE* fp = _wfopen(path, L"wb");
        if (fp) { BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + row * h;
            ih.biSize = sizeof ih; ih.biWidth = (LONG)w; ih.biHeight = (LONG)h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = row * h;
            fwrite(&fh, sizeof fh, 1, fp); fwrite(&ih, sizeof ih, 1, fp); fwrite(out, row, h, fp); fclose(fp); }
        if (!name) { Log("dump: pass %d #%02d target id %d %ux%u fmt %d, %ld draws", g_dumpPass, g_dumpSeq[g_dumpPass], id, d.Width, d.Height, f, g_drawsBound);
        g_dumpSeq[g_dumpPass]++; }
        free(out);
    }
    st->Release(); t->Release();
}
static DWORD g_dumpTid; static UINT g_dumpRtN; static ID3D11DepthStencilView* g_dumpDsv; static ID3D11RenderTargetView* g_dumpMrt[4];
// build 061: emission probe. After the dump frame, pixels where eye 1's emission target (G-buffer rt2) is lit but eye 0's
// is dark (the missing lantern glow) are picked. Frame B ("locate"): those pixels of rt2 are read after every G-buffer
// draw of both passes and the draws that change them are logged. Frame C ("state"): for the draws found, the full state
// (PS/VS constants by name, textures, states) is captured in both passes and compared. All from one Shift+F10.
static uint8_t* g_rt2q[2]; static UINT g_qW, g_qH, g_qStep;
static int g_probeN, g_probeX[8], g_probeY[8]; static uint32_t g_probeV[8]; static ID3D11Texture2D* g_probeSt;
struct ProbeHit { void* ib; void* vb; UINT a, b, d, e; void* ps; void* vs; };
static ProbeHit g_hit[16]; static int g_hitN;
static bool IsEyeGbuf() {
    if (!g_dumpRt || g_dumpRtN < 3) return false;
    ID3D11Resource* r = nullptr; g_dumpRt->GetResource(&r); if (!r) return false; ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return false;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); t->Release(); return d.Width >= 2048;
}
static ID3D11Texture2D* RtTex(ID3D11RenderTargetView* v) { if (!v) return nullptr; ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) return nullptr; ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); return t; }
static void ReadQuarter(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, int pass) {
    ID3D11Texture2D* t = RtTex(v); if (!t) return; D3D11_TEXTURE2D_DESC d; t->GetDesc(&d);
    if (d.Format != 28 || d.SampleDesc.Count != 1) { t->Release(); return; }
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); ID3D11Texture2D* st = nullptr; if (dev) { dev->CreateTexture2D(&sd, nullptr, &st); dev->Release(); }
    if (!st) { t->Release(); return; }
    c->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        g_qStep = 4; g_qW = d.Width / 4; g_qH = d.Height / 4; free(g_rt2q[pass]); g_rt2q[pass] = (uint8_t*)malloc(g_qW * g_qH);
        for (UINT y = 0; y < g_qH; y++) { const uint8_t* p = (const uint8_t*)m.pData + (size_t)(y * 4) * m.RowPitch;
            for (UINT x = 0; x < g_qW; x++) { const uint8_t* q = p + (size_t)x * 16; uint8_t mx = q[0] > q[1] ? q[0] : q[1]; if (q[2] > mx) mx = q[2]; g_rt2q[pass][y * g_qW + x] = mx; } }
        c->Unmap(st, 0);
    }
    st->Release(); t->Release();
}
static void ProbePick() {
    g_probeN = 0; if (!g_rt2q[0] || !g_rt2q[1]) { Log("probe: emission target not captured in both passes"); return; }
    int hist0 = 0; for (UINT i = 0; i < g_qW * g_qH; i++) if (g_rt2q[1][i] >= 24) hist0++;
    for (int pick = 0; pick < 8; pick++) {
        int best = -1, bv = 0;
        for (UINT y = 3; y + 3 < g_qH; y++) for (UINT x = 8; x + 8 < g_qW; x++) {
            int v = g_rt2q[1][y * g_qW + x]; if (v < 24 || v <= bv) continue;
            bool near0 = false; for (int dy = -3; dy <= 3 && !near0; dy++) for (int dx = -8; dx <= 8; dx++) if (g_rt2q[0][(y + dy) * g_qW + x + dx] >= v / 4) { near0 = true; break; }
            if (near0) continue;
            bool close = false; for (int k = 0; k < g_probeN; k++) if (abs(g_probeX[k] / 4 - (int)x) < 10 && abs(g_probeY[k] / 4 - (int)y) < 10) close = true;
            if (close) continue;
            best = (int)(y * g_qW + x); bv = v;
        }
        if (best < 0) break;
        g_probeX[g_probeN] = (best % g_qW) * 4; g_probeY[g_probeN] = (best / g_qW) * 4; g_probeN++;
        Log("probe: pixel %d at %d,%d - eye 1 emission %d, eye 0 dark", g_probeN - 1, (best % g_qW) * 4, (best / g_qW) * 4, bv);
    }
    Log("probe: %d probe pixels (eye 1 lit emission pixels: %d at quarter size)", g_probeN, hist0);
}
static void ProbeRead(ID3D11DeviceContext* c, uint32_t* out) {
    ID3D11Texture2D* t = RtTex(g_dumpMrt[2]); if (!t) return;
    if (!g_probeSt) { D3D11_TEXTURE2D_DESC sd = {}; sd.Width = 8; sd.Height = 1; sd.MipLevels = 1; sd.ArraySize = 1; sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.SampleDesc.Count = 1; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { dev->CreateTexture2D(&sd, nullptr, &g_probeSt); dev->Release(); } }
    if (!g_probeSt) { t->Release(); return; }
    for (int i = 0; i < g_probeN; i++) { D3D11_BOX b = { (UINT)g_probeX[i], (UINT)g_probeY[i], 0, (UINT)g_probeX[i] + 1, (UINT)g_probeY[i] + 1, 1 }; c->CopySubresourceRegion(g_probeSt, 0, i, 0, 0, t, 0, &b); }
    D3D11_MAPPED_SUBRESOURCE m; if (SUCCEEDED(c->Map(g_probeSt, 0, D3D11_MAP_READ, 0, &m))) { memcpy(out, m.pData, 4 * g_probeN); c->Unmap(g_probeSt, 0); }
    t->Release();
}
static void DumpDepth(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, const wchar_t* name);
static void DumpOnBind(ID3D11DeviceContext* c, ID3D11RenderTargetView* next, UINT n, ID3D11DepthStencilView* dsv, ID3D11RenderTargetView* const* rtv) {
    if (g_dump != 2 || GetCurrentThreadId() != g_dumpTid) return;
    if (g_dumpRt == next) { g_dumpRtN = n; g_dumpDsv = dsv; return; }
    if (g_mode == 0 && g_dumpRt && g_dumpRt != next && g_drawsBound > 0 && g_dumpPass < 2 && IsEyeGbuf()) ReadQuarter(c, g_dumpMrt[2], g_dumpPass);
    if (g_mode == 0 && g_dumpRt && g_dumpRt != next && g_drawsBound > 0) {
        int g = g_dumpSeq[g_dumpPass];
        DumpTarget(c, g_dumpRt);
        if (g_dumpDsv && g_dumpPass == 0 && g < 8) {   // 117: the shadow map passes at the start of the first eye pass: save their depth (the shadow map itself)
            ID3D11Resource* r0 = nullptr; g_dumpRt->GetResource(&r0); ID3D11Texture2D* t0 = nullptr; if (r0) { r0->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t0); r0->Release(); }
            if (t0) { D3D11_TEXTURE2D_DESC d0; t0->GetDesc(&d0); t0->Release();
                if (d0.Width == d0.Height && d0.Width >= 512 && d0.Width <= 4096) { wchar_t nm[160]; swprintf(nm, 160, L"sdvr_dump%02d_p0_%02d_shadowmap", g_dumpN, g); DumpDepth(c, g_dumpDsv, nm); } }
        }
        if (g_dumpSeq[g_dumpPass] != g) {   // saved: also the alpha of eye-size RGBA8 targets and the other bound targets (G-buffer)
            for (int i = 0; i < 4; i++) {
                ID3D11RenderTargetView* v = i == 0 ? g_dumpRt : g_dumpMrt[i]; if (!v) continue;
                ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) continue;
                ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) continue;
                D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); int id = ResId(t); t->Release();
                wchar_t nm[160];
                if (i > 0) { swprintf(nm, 160, L"sdvr_dump%02d_p%d_%02d_rt%d_id%d.bmp", g_dumpN, g_dumpPass, g, i, id); DumpTarget(c, v, nm); }
                if (d.Format == 28 && d.Width >= 2048) { swprintf(nm, 160, L"sdvr_dump%02d_p%d_%02d_rt%d_id%d_alpha.bmp", g_dumpN, g_dumpPass, g, i, id); DumpTarget(c, v, nm, true); }
            }
        }
    }
    g_drawsBound = 0;
    g_dumpRt = next; g_dumpRtN = n; g_dumpDsv = dsv;
    for (int i = 1; i < 4; i++) { if (g_dumpMrt[i]) g_dumpMrt[i]->Release(); g_dumpMrt[i] = (rtv && (UINT)i < n) ? rtv[i] : nullptr; if (g_dumpMrt[i]) g_dumpMrt[i]->AddRef(); }
    if (g_mode == 1 && g_dumpPass < 2 && IsEyeGbuf()) { ProbeRead(c, g_probeV); Log("probe: pass %d G-buffer bound, probe start values %08x %08x %08x %08x", g_dumpPass, g_probeV[0], g_probeV[1], g_probeV[2], g_probeV[3]); }
}
static long g_dseq;
static void DumpDraw() { if (g_dump == 2 && GetCurrentThreadId() == g_dumpTid) { g_drawsBound++; g_dseq++; } }
void RtDumpMark() { if (g_dump == 2 && g_dumpPass < 2) g_dumpPass++; }
static void GbStateReport(); static void ProvReset(); static void ProvLog(ID3D11DeviceContext* c);
void RtDumpFrame() {
    g_sfEye = 0;
    if (g_dump == 1) { g_dumpTid = GetCurrentThreadId(); g_dumpPass = 0; g_dseq = 0; ProvReset(); memset(g_dumpSeq, 0, sizeof g_dumpSeq); g_dumpRt = nullptr; g_drawsBound = 0; InterlockedExchange(&g_dump, 2); Log("dump: recording render targets of one frame"); return; }
    if (g_dump == 2) { for (int i = 1; i < 4; i++) if (g_dumpMrt[i]) { g_dumpMrt[i]->Release(); g_dumpMrt[i] = nullptr; } InterlockedExchange(&g_dump, 0);
        if (g_mode == 0) { Log("dump: done - pass 0 %d targets, pass 1 %d, after %d", g_dumpSeq[0], g_dumpSeq[1], g_dumpSeq[2]); GbReport(); g_dumpN++;
            ProbePick(); g_mode = 1; g_hitN = 0; InterlockedExchange(&g_dump, 1); Log("probe: locating the draws that write the probe pixels (next frame)"); }
        else if (g_mode == 1) { Log("probe: locate done, %d draws found", g_hitN); g_mode = 2; memset(g_emN, 0, sizeof g_emN); memset(g_emWhite, 0, sizeof g_emWhite); memset(g_emFresh, 0, sizeof g_emFresh); memset(g_emSame, 0, sizeof g_emSame); InterlockedExchange(&g_dump, 1); Log("probe: capturing the state of those draws (next frame)"); }
        else { for (int q = 0; q < 2; q++) Log("emis: pass %d - %d emissive cbSceneryInstance draws: white %d, slot bound for the draw %d, slot shares a buffer with another slot %d", q, g_emN[q], g_emWhite[q], g_emFresh[q], g_emSame[q]); GbStateReport(); g_mode = 0; } }
}
void RtOutlineMark(const char* m) { if (!strncmp(m, "eye copy end", 12)) { RtDumpMark(); g_sfEye = g_sfEye + 1; if (g_stateFix) InterlockedIncrement(&g_stateGen); } if (g_ol == 2 && !strncmp(m, "eye copy (", 10)) DiffMarkPass(); if (OlE* e = OlAdd('M')) { strncpy(e->m, m, 23); } }

// build 051: constant buffer dump. If a shader is selected with Ctrl+F10 (hidden), F10 records it instead of hiding it:
// the pixel and vertex shader constant buffers of its first 4 draws in each eye pass of both recorded frames, then the
// values that differ between the eye passes are logged (the dark lanterns: emissive / light intensity per pass?).
typedef void (STDMETHODCALLTYPE *PSGetCB_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer**);
static float g_cb[2][2][4][2][6][256]; static void* g_cbVs; static int g_cbN[2][2]; static int g_cbSz[2][2][4][2][6];
static int CbFrame() { return g_olFrames ? 1 : 0; }
static int CbPass() { return g_nPassAt == 0 || (g_olFrames && g_nPassAt == 2) ? 0 : 1; }
static void CbRead(ID3D11DeviceContext* c, ID3D11Buffer* b, float* out, int* sz) {
    *sz = 0; if (!b) return;
    D3D11_BUFFER_DESC d; b->GetDesc(&d); UINT n = d.ByteWidth < 1024 ? d.ByteWidth : 1024;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return;
    D3D11_BUFFER_DESC sd = {}; sd.ByteWidth = d.ByteWidth; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr;
    if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st)) && st) {
        c->CopyResource(st, b);
        D3D11_MAPPED_SUBRESOURCE m; if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) { memcpy(out, m.pData, n); *sz = n / 4; c->Unmap(st, 0); }
        st->Release();
    }
    dev->Release();
}
// 053: every draw of the selected shader, per pass: object position (VS cb1 row 3), PS cb1 / cb2 (16 floats each),
// pixel shader textures 0-2 - matched by position between the eye passes (the same lantern in both eyes)
struct Obj { float pos[3]; float c1[16], c2[16]; short t[3]; };
static Obj g_obj[2][2][400]; static int g_nObj[2][2];
static void ObjCapture(ID3D11DeviceContext* c, int f, int p) {
    if (g_nObj[f][p] >= 400) return;
    Obj& o = g_obj[f][p][g_nObj[f][p]++]; memset(&o, 0, sizeof o);
    float tmp[256]; int n;
    ID3D11Buffer* v = nullptr; c->VSGetConstantBuffers(1, 1, &v); CbRead(c, v, tmp, &n); if (v) v->Release(); if (n >= 15) memcpy(o.pos, tmp + 12, 12);
    ID3D11Buffer* b[2] = {}; c->PSGetConstantBuffers(1, 2, b);
    CbRead(c, b[0], tmp, &n); if (n >= 16) memcpy(o.c1, tmp, 64);
    CbRead(c, b[1], tmp, &n); if (n >= 16) memcpy(o.c2, tmp, 64);
    for (int i = 0; i < 2; i++) if (b[i]) b[i]->Release();
    for (int i = 0; i < 3; i++) o.t[i] = ViewRes(g_srv[i]);
}
static void ObjReport() {
    for (int f = 0; f < 2; f++) {
        int na = g_nObj[f][0], nb = g_nObj[f][1], matched = 0, same = 0;
        Log("objects: frame %c - selected shader drew %d objects in pass 0, %d in pass 1", 'A' + f, na, nb);
        static bool used[400]; memset(used, 0, sizeof used);
        for (int i = 0; i < na; i++) {
            Obj& a = g_obj[f][0][i]; int j = 0;
            for (; j < nb; j++) { Obj& b = g_obj[f][1][j]; if (!used[j] && fabsf(a.pos[0] - b.pos[0]) < 0.01f && fabsf(a.pos[1] - b.pos[1]) < 0.01f && fabsf(a.pos[2] - b.pos[2]) < 0.01f && !memcmp(a.t, b.t, sizeof a.t)) break; }
            if (j == nb) { Log("objects:   only in pass 0: at %.2f %.2f %.2f, textures #%d #%d #%d, cb1 %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g", a.pos[0], a.pos[1], a.pos[2], a.t[0], a.t[1], a.t[2], a.c1[0], a.c1[1], a.c1[2], a.c1[3], a.c1[4], a.c1[5], a.c1[6], a.c1[7]); continue; }
            used[j] = true; matched++;
            Obj& b = g_obj[f][1][j];
            if (!memcmp(a.c1, b.c1, 64) && !memcmp(a.c2, b.c2, 64)) { same++; continue; }
            char x[400]; int o = 0;
            for (int k = 0; k < 16; k++) if (memcmp(&a.c1[k], &b.c1[k], 4)) o += snprintf(x + o, sizeof x - o, " cb1[%d] %.4g/%.4g", k, a.c1[k], b.c1[k]);
            for (int k = 0; k < 16; k++) if (memcmp(&a.c2[k], &b.c2[k], 4)) o += snprintf(x + o, sizeof x - o, " cb2[%d] %.4g/%.4g", k, a.c2[k], b.c2[k]);
            Log("objects:   DIFFERS at %.2f %.2f %.2f (textures #%d #%d #%d):%s", a.pos[0], a.pos[1], a.pos[2], a.t[0], a.t[1], a.t[2], x);
        }
        for (int j = 0; j < nb; j++) if (!used[j]) { Obj& b = g_obj[f][1][j]; Log("objects:   only in pass 1: at %.2f %.2f %.2f, textures #%d #%d #%d, cb1 %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g", b.pos[0], b.pos[1], b.pos[2], b.t[0], b.t[1], b.t[2], b.c1[0], b.c1[1], b.c1[2], b.c1[3], b.c1[4], b.c1[5], b.c1[6], b.c1[7]); }
        Log("objects: frame %c - %d matched (%d identical)", 'A' + f, matched, same);
    }
    // full values of the first few objects (to read the instance constants)
    for (int i = 0; i < g_nObj[0][0] && i < 6; i++) { Obj& a = g_obj[0][0][i];
        Log("objects:   pass 0 object %d at %.1f %.1f %.1f: cb1 %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g | %.3g %.3g %.3g %.3g", i, a.pos[0], a.pos[1], a.pos[2],
            a.c1[0], a.c1[1], a.c1[2], a.c1[3], a.c1[4], a.c1[5], a.c1[6], a.c1[7], a.c1[8], a.c1[9], a.c1[10], a.c1[11], a.c1[12], a.c1[13], a.c1[14], a.c1[15]); }
}
static void CbDump(ID3D11DeviceContext* c) {
    if (!g_skipPs || g_ps != g_skipPs) return;
    int f = CbFrame(), p = CbPass(); if (f > 1 || g_nPassAt >= (f ? 4 : 2)) return;
    ObjCapture(c, f, p);
    int k = g_cbN[f][p]; if (k >= 4) return; g_cbN[f][p]++; g_cbVs = g_vs;
    for (int st = 0; st < 2; st++) {
        ID3D11Buffer* bs[6] = {};
        if (st == 0) c->PSGetConstantBuffers(0, 6, bs); else c->VSGetConstantBuffers(0, 6, bs);
        for (int i = 0; i < 6; i++) { CbRead(c, bs[i], g_cb[f][p][k][st][i], &g_cbSz[f][p][k][st][i]); if (bs[i]) bs[i]->Release(); }
    }
}
// build 056: step dump. In the dump frame, for every eye-size single-target colour group (the composite / forward
// passes), the target is saved after each of its first 12 draws, with the draw's state logged: shaders, textures,
// depth buffer and depth/stencil state, blend, and the first PS constants. The depth/stencil buffer is saved once per
// group (depth stretched to grey, stencil values as a colour image). Finds which draw adds the lantern glow in eye 1
// and why the same draw adds nothing in eye 0.
static void DumpDepth(ID3D11DeviceContext* c, ID3D11DepthStencilView* v, const wchar_t* name) {
    ID3D11Resource* r = nullptr; v->GetResource(&r); if (!r) return;
    ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); int f = d.Format;
    bool d24 = f == 44 || f == 45 || f == 46, d32 = f == 39 || f == 40 || f == 41, d32s = f == 19 || f == 20 || f == 21;
    if ((!d24 && !d32 && !d32s) || d.SampleDesc.Count != 1) { Log("step:   depth format %d samples %u - not saved", f, d.SampleDesc.Count); t->Release(); return; }
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0; sd.MipLevels = 1; sd.ArraySize = 1;
    sd.Format = d24 ? DXGI_FORMAT_R24G8_TYPELESS : d32 ? DXGI_FORMAT_R32_TYPELESS : DXGI_FORMAT_R32G8X24_TYPELESS;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); ID3D11Texture2D* st = nullptr;
    HRESULT hr = dev ? dev->CreateTexture2D(&sd, nullptr, &st) : E_FAIL; if (dev) dev->Release();
    if (FAILED(hr) || !st) { Log("step:   depth staging failed %08x", (unsigned)hr); t->Release(); return; }
    c->CopySubresourceRegion(st, 0, 0, 0, 0, t, 0, nullptr);
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
        int step = d.Width >= 2048 ? 4 : d.Width >= 1024 ? 2 : 1; UINT w = d.Width / step, h = d.Height / step, row = (w * 3 + 3) & ~3u;
        float* dv = (float*)malloc(sizeof(float) * w * h); uint8_t* sv = (uint8_t*)malloc(w * h); float lo = 1e30f, hi = -1e30f; int hist[256] = {};
        for (UINT y = 0; y < h; y++) { const uint8_t* p = (const uint8_t*)m.pData + (size_t)(y * step) * m.RowPitch;
            for (UINT x = 0; x < w; x++) { size_t sx = (size_t)x * step; float z; uint8_t sten = 0;
                if (d24) { uint32_t u = *(const uint32_t*)(p + sx * 4); z = (u & 0xffffff) / 16777215.f; sten = (uint8_t)(u >> 24); }
                else if (d32) z = *(const float*)(p + sx * 4); else { z = *(const float*)(p + sx * 8); sten = p[sx * 8 + 4]; }
                dv[y * w + x] = z; sv[y * w + x] = sten; hist[sten]++;
                if (z > 0.f && z < 1.f) { if (z < lo) lo = z; if (z > hi) hi = z; } } }
        c->Unmap(st, 0);
        uint8_t* out = (uint8_t*)malloc((size_t)row * h);
        for (int pass = 0; pass < 2; pass++) {
            for (UINT y = 0; y < h; y++) { uint8_t* o = out + (size_t)(h - 1 - y) * row;
                for (UINT x = 0; x < w; x++) { uint8_t R, G, B;
                    if (pass == 0) { float z = dv[y * w + x]; float q = hi > lo ? (z - lo) / (hi - lo) : 0.f; if (q < 0) q = 0; if (q > 1) q = 1; R = G = B = (uint8_t)(q * 255.f); if (z <= 0.f) { R = 0; G = 0; B = 255; } else if (z >= 1.f) { R = 255; G = 0; B = 255; } }
                    else { uint8_t v8 = sv[y * w + x]; R = (uint8_t)((v8 & 7) * 36); G = (uint8_t)(((v8 >> 3) & 7) * 36); B = (uint8_t)((v8 >> 6) * 85); }
                    o[x * 3 + 0] = B; o[x * 3 + 1] = G; o[x * 3 + 2] = R; } }
            wchar_t path[MAX_PATH]; swprintf(path, MAX_PATH, L"%ls\\%ls_%ls.bmp", g_dumpDir, name, pass ? L"stencil" : L"depth");
            FILE* fp = _wfopen(path, L"wb");
            if (fp) { BITMAPFILEHEADER fh = {}; BITMAPINFOHEADER ih = {}; fh.bfType = 0x4D42; fh.bfOffBits = sizeof fh + sizeof ih; fh.bfSize = fh.bfOffBits + row * h;
                ih.biSize = sizeof ih; ih.biWidth = (LONG)w; ih.biHeight = (LONG)h; ih.biPlanes = 1; ih.biBitCount = 24; ih.biSizeImage = row * h;
                fwrite(&fh, sizeof fh, 1, fp); fwrite(&ih, sizeof ih, 1, fp); fwrite(out, row, h, fp); fclose(fp); }
        }
        char hs[400]; int o = 0; for (int i = 0; i < 256 && o < 380; i++) if (hist[i]) o += snprintf(hs + o, sizeof hs - o, " %d:%d", i, hist[i]);
        Log("step:   depth buffer #%d %ux%u fmt %d, depth range %.6f..%.6f, stencil values%s", ResId(t), d.Width, d.Height, f, lo, hi, hs);
        free(out); free(dv); free(sv);
    }
    st->Release(); t->Release();
}
uint64_t ShaderHash(void* sh);
void ShaderNames(void* sh, char* out, int cap);
void ShaderDescribe(const char* tag, void* sh, const float* const* v0, const float* const* v1, const int* n0, const int* n1, int slots, bool diffOnly);
// 102: read a byte range of any buffer (through a small staging copy)
static UINT g_startInst;
static bool BufRange(ID3D11DeviceContext* c, ID3D11Buffer* b, UINT off, UINT n, uint8_t* out) {
    D3D11_BUFFER_DESC d; b->GetDesc(&d); if (off >= d.ByteWidth) return false; if (off + n > d.ByteWidth) return false;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return false;
    D3D11_BUFFER_DESC sd = {}; sd.ByteWidth = n; sd.Usage = D3D11_USAGE_STAGING; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Buffer* st = nullptr; bool ok = false;
    if (SUCCEEDED(dev->CreateBuffer(&sd, nullptr, &st)) && st) {
        D3D11_BOX bx = { off, 0, 0, off + n, 1, 1 }; c->CopySubresourceRegion(st, 0, 0, 0, 0, b, 0, &bx);
        D3D11_MAPPED_SUBRESOURCE m; if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) { memcpy(out, m.pData, n); ok = true; c->Unmap(st, 0); }
        st->Release();
    }
    dev->Release(); return ok;
}
static uint32_t Sum32(const void* p, size_t n) { uint32_t h = 2166136261u; const uint8_t* b = (const uint8_t*)p; for (size_t i = 0; i < n; i++) { h ^= b[i]; h *= 16777619u; } return h; }
// 100: one short line per draw of the half-size groups (particles / mist) in the dump frame, to find a draw that exists in
// one eye pass only. "pdraw:" lines; compare pass 0 with pass 1.
static void ParticleLine(ID3D11DeviceContext* c, UINT cnt) {
    if (g_mode != 0 || g_dump != 2 || GetCurrentThreadId() != g_dumpTid || !g_dumpRt || g_drawsBound > 700 || g_dumpPass > 1) return;
    ID3D11Resource* r = nullptr; g_dumpRt->GetResource(&r); if (!r) return;
    ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); int rid = ResId(t); t->Release();
    if (d.Width < 1024 || d.Width >= 2048 || d.Height < 1024 || d.Format != 28) return;
    char tx[200]; int o = 0; tx[0] = 0;
    ID3D11ShaderResourceView* sr[4] = {}; c->PSGetShaderResources(0, 4, sr);
    for (int i = 0; i < 4; i++) { if (!sr[i]) continue; short id = ViewRes(sr[i]); o += snprintf(tx + o, sizeof tx - o, " t%d=#%d(%dx%d f%d)", i, id, id >= 0 ? g_resW[id] : 0, id >= 0 ? g_resH[id] : 0, id >= 0 ? g_resF[id] : 0); sr[i]->Release(); }
    ID3D11Buffer* vb = nullptr; UINT stride = 0, off = 0; c->IAGetVertexBuffers(0, 1, &vb, &stride, &off);
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ifm; UINT ioff = 0; c->IAGetIndexBuffer(&ib, &ifm, &ioff);
    UINT vbSize = 0; if (vb) { D3D11_BUFFER_DESC bd; vb->GetDesc(&bd); vbSize = bd.ByteWidth; }
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm); int bsrc = -1, bdst = -1;
    if (bs) { D3D11_BLEND_DESC q; bs->GetDesc(&q); bsrc = q.RenderTarget[0].SrcBlend; bdst = q.RenderTarget[0].DestBlend; bs->Release(); }
    float w[4096]; int wn = 0; float px = 0, py = 0, pz = 0;
    ID3D11Buffer* cb = nullptr; c->VSGetConstantBuffers(0, 1, &cb); if (cb) { CbRead(c, cb, w, &wn); cb->Release(); if (wn >= 16) { px = w[12]; py = w[13]; pz = w[14]; } }
    Log("pdraw: pass %d group %02d target #%d draw %ld count %u ps %p vs %p vb %p (%u bytes, stride %u) ib %p blend %d/%d vs-cb0 row3 %.1f %.1f %.1f |%s",
        g_dumpPass, g_dumpSeq[g_dumpPass], rid, (long)g_drawsBound, cnt, g_ps, g_vs, (void*)vb, vbSize, stride, (void*)ib, bsrc, bdst, px, py, pz, tx);
    if (cnt < 100 && g_drawsBound <= 3) {   // 101: the odd small draw at the start of the layer - what are its shaders and constants?
        ID3D11PixelShader* ps = nullptr; c->PSGetShader(&ps, nullptr, nullptr); ID3D11VertexShader* vs = nullptr; c->VSGetShader(&vs, nullptr, nullptr);
        ID3D11GeometryShader* gs = nullptr; c->GSGetShader(&gs, nullptr, nullptr);
        D3D11_PRIMITIVE_TOPOLOGY tp; c->IAGetPrimitiveTopology(&tp);
        Log("pdraw:   topology %d, geometry shader %p", (int)tp, (void*)gs);
        if (gs) gs->Release();
        static float pv[6][4096], vv[6][4096]; const float* pp[6]; const float* vp[6]; int pn[6], vn[6];
        for (int i = 0; i < 6; i++) {
            pn[i] = vn[i] = 0; pp[i] = pv[i]; vp[i] = vv[i];
            ID3D11Buffer* b = nullptr; c->PSGetConstantBuffers(i, 1, &b); if (b) { CbRead(c, b, pv[i], &pn[i]); b->Release(); }
            b = nullptr; c->VSGetConstantBuffers(i, 1, &b); if (b) { CbRead(c, b, vv[i], &vn[i]); b->Release(); }
        }
        if (ps) { ShaderDescribe("pdraw PS", ps, pp, pp, pn, pn, 6, false); ps->Release(); }
        if (vs) { ShaderDescribe("pdraw VS", vs, vp, vp, vn, vn, 6, false); vs->Release(); }
    }
    if (cnt >= 100000) {   // 102: instanced particle draw - instance data, constants and depth set-up, to compare between the eye passes
        UINT inst = cnt % 100000; ID3D11Buffer* nb = nullptr; UINT ns = 0, no = 0; c->IAGetVertexBuffers(1, 1, &nb, &ns, &no);
        char first[260] = ""; uint32_t isum = 0; UINT ibytes = 0;
        if (nb && ns && ns <= 256 && inst) {
            ibytes = inst * ns; if (ibytes > 16384) ibytes = 16384; static uint8_t ibuf[16384];
            if (BufRange(c, nb, no + g_startInst * ns, ibytes, ibuf)) { isum = Sum32(ibuf, ibytes); int o3 = 0; const float* f = (const float*)ibuf; for (UINT k = 0; k < ns / 4 && k < 16 && o3 < 240; k++) o3 += snprintf(first + o3, sizeof first - o3, " %.4g", f[k]); }
            else snprintf(first, sizeof first, " (not readable)");
        }
        uint32_t cs[2][4] = {}; int cn[2][4] = {};
        for (int st = 0; st < 2; st++) for (int i = 0; i < 4; i++) { ID3D11Buffer* b = nullptr; if (st == 0) c->PSGetConstantBuffers(i, 1, &b); else c->VSGetConstantBuffers(i, 1, &b); if (!b) continue;
            static float v[4096]; int n = 0; CbRead(c, b, v, &n); b->Release(); cn[st][i] = n; cs[st][i] = Sum32(v, (size_t)n * 4); }
        ID3D11DepthStencilState* ds = nullptr; UINT ref = 0; c->OMGetDepthStencilState(&ds, &ref); int de = -1, dw = -1, df = -1; char sten[120] = ""; if (ds) { D3D11_DEPTH_STENCIL_DESC q; ds->GetDesc(&q); de = q.DepthEnable; dw = q.DepthWriteMask; df = q.DepthFunc; snprintf(sten, sizeof sten, " stencil %d rm %02x wm %02x ref %u ffunc %d fpass %d ffail %d fzfail %d bfunc %d", q.StencilEnable, q.StencilReadMask, q.StencilWriteMask, ref, q.FrontFace.StencilFunc, q.FrontFace.StencilPassOp, q.FrontFace.StencilFailOp, q.FrontFace.StencilDepthFailOp, q.BackFace.StencilFunc); ds->Release(); }
        ID3D11RenderTargetView* rv = nullptr; ID3D11DepthStencilView* dv = nullptr; c->OMGetRenderTargets(1, &rv, &dv); short did = dv ? ViewRes(dv) : -1; if (rv) rv->Release(); if (dv) dv->Release();
        Log("pinst: pass %d draw %ld inst %u start %u ibuf %p stride %u off %u sum %08x first:%s | PS cb %08x/%d %08x/%d %08x/%d %08x/%d VS cb %08x/%d %08x/%d %08x/%d %08x/%d | dsv #%d depth %d write %d func %d%s",
            g_dumpPass, (long)g_drawsBound, inst, g_startInst, (void*)nb, ns, no, isum, first, cs[0][0], cn[0][0], cs[0][1], cn[0][1], cs[0][2], cn[0][2], cs[0][3], cn[0][3], cs[1][0], cn[1][0], cs[1][1], cn[1][1], cs[1][2], cn[1][2], cs[1][3], cn[1][3], did, de, dw, df, sten);
        if (nb) nb->Release();
        {   // 110: viewport, scissor and rasterizer state of the draw
            D3D11_VIEWPORT vp[1]; UINT nv = 1; c->RSGetViewports(&nv, vp); D3D11_RECT sr[1] = {}; UINT nr = 1; c->RSGetScissorRects(&nr, sr);
            ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs); int sc = -1, cl = -1, fc = -1, dcl = -1; if (rs) { D3D11_RASTERIZER_DESC q; rs->GetDesc(&q); sc = q.ScissorEnable; cl = q.CullMode; fc = q.FrontCounterClockwise; dcl = q.DepthClipEnable; rs->Release(); }
            ID3D11BlendState* bs2 = nullptr; FLOAT bf2[4]; UINT sm2 = 0; c->OMGetBlendState(&bs2, bf2, &sm2); int be = -1, wm = -1, sa = -1, da = -1; if (bs2) { D3D11_BLEND_DESC q; bs2->GetDesc(&q); be = q.RenderTarget[0].BlendEnable; wm = q.RenderTarget[0].RenderTargetWriteMask; sa = q.RenderTarget[0].SrcBlendAlpha; da = q.RenderTarget[0].DestBlendAlpha; bs2->Release(); }
            Log("prs: pass %d draw %ld viewport %.0f %.0f %.0f %.0f z %.2f-%.2f (n %u) scissor on %d rect %ld %ld %ld %ld (n %u) cull %d ccw %d depthclip %d | blend on %d mask %x alpha %d/%d samplemask %x",
                g_dumpPass, (long)g_drawsBound, nv ? vp[0].TopLeftX : -1.f, nv ? vp[0].TopLeftY : -1.f, nv ? vp[0].Width : -1.f, nv ? vp[0].Height : -1.f, nv ? vp[0].MinDepth : -1.f, nv ? vp[0].MaxDepth : -1.f, nv, sc, sr[0].left, sr[0].top, sr[0].right, sr[0].bottom, nr, cl, fc, dcl, be, wm, sa, da, sm2);
        }
        static int dsvDone[2] = { -1, -1 };
        if (g_drawsBound >= 20 && dsvDone[g_dumpPass] != g_dumpN && (dsvDone[g_dumpPass] = g_dumpN, true)) {   // 110: once per pass: what the bound depth-stencil buffer holds (depth + stencil values)
            ID3D11RenderTargetView* rv2 = nullptr; ID3D11DepthStencilView* dv2 = nullptr; c->OMGetRenderTargets(1, &rv2, &dv2); if (rv2) rv2->Release();
            if (dv2) { ID3D11Resource* rr = nullptr; dv2->GetResource(&rr); ID3D11Texture2D* tt = nullptr; if (rr) { rr->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tt); rr->Release(); }
                if (tt) { D3D11_TEXTURE2D_DESC td; tt->GetDesc(&td);
                    D3D11_TEXTURE2D_DESC sd2 = td; sd2.Usage = D3D11_USAGE_STAGING; sd2.BindFlags = 0; sd2.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd2.MiscFlags = 0;
                    ID3D11Device* dev = nullptr; c->GetDevice(&dev); ID3D11Texture2D* st2 = nullptr;
                    if (dev && (td.Format == 44 || td.Format == 45) && SUCCEEDED(dev->CreateTexture2D(&sd2, nullptr, &st2)) && st2) {
                        c->CopyResource(st2, tt); D3D11_MAPPED_SUBRESOURCE m2;
                        if (SUCCEEDED(c->Map(st2, 0, D3D11_MAP_READ, 0, &m2))) {
                            static UINT hist[256]; memset(hist, 0, sizeof hist); double sum = 0; UINT n = 0; uint32_t h = 2166136261u;
                            for (UINT y = 0; y < td.Height; y += 2) { const uint32_t* r4 = (const uint32_t*)((const uint8_t*)m2.pData + (size_t)y * m2.RowPitch);
                                for (UINT x = 0; x < td.Width; x += 2) { uint32_t v = r4[x]; hist[v >> 24]++; sum += (double)(v & 0xFFFFFF) / 16777215.0; n++; h ^= v; h *= 16777619u; } }
                            char hs[500]; int ho = 0; for (int q = 0; q < 256 && ho < 470; q++) if (hist[q]) ho += snprintf(hs + ho, sizeof hs - ho, " %d:%u", q, hist[q]);
                            Log("pdsv: pass %d draw %ld bound depth-stencil #%d %ux%u fmt %d: depth mean %.6f, sum %08x, stencil values (value:count of %u):%s", g_dumpPass, (long)g_drawsBound, ResId(tt), td.Width, td.Height, (int)td.Format, n ? sum / n : 0.0, h, n, hs);
                            c->Unmap(st2, 0);
                        }
                        st2->Release();
                    }
                    if (dev) dev->Release(); tt->Release(); }
                dv2->Release(); }
        }
        for (int i = 0; i < 8; i++) {   // 109: the full vertex shader constant blocks of every instanced draw (up to 64 floats each)
            ID3D11Buffer* b = nullptr; c->VSGetConstantBuffers(i, 1, &b); if (!b) continue;
            static float v[4096]; int n = 0; CbRead(c, b, v, &n); b->Release(); if (!n) continue;
            char ln[1100]; int o5 = 0; for (int k = 0; k < n && k < 64 && o5 < 1050; k++) o5 += snprintf(ln + o5, sizeof ln - o5, " %.5g", v[k]);
            Log("pvs: pass %d draw %ld slot %d (%d floats):%s", g_dumpPass, (long)g_drawsBound, i, n, ln);
        }
        for (UINT sl = 2; sl < 8; sl++) {   // 108: any further vertex streams of the draw (per-particle position / colour data?)
            ID3D11Buffer* xb = nullptr; UINT xs = 0, xo = 0; c->IAGetVertexBuffers(sl, 1, &xb, &xs, &xo); if (!xb) continue;
            char fx[300] = ""; uint32_t xsum = 0; UINT xbytes = inst * xs; if (xbytes > 16384) xbytes = 16384; static uint8_t xbuf[16384];
            D3D11_BUFFER_DESC xd; xb->GetDesc(&xd);
            if (xs && xs <= 256 && inst && BufRange(c, xb, xo + g_startInst * xs, xbytes, xbuf)) { xsum = Sum32(xbuf, xbytes); int o4 = 0; const float* f = (const float*)xbuf; for (UINT k = 0; k < xs / 4 && k < 20 && o4 < 280; k++) o4 += snprintf(fx + o4, sizeof fx - o4, " %.4g", f[k]); }
            Log("pinst:   pass %d draw %ld stream %u buf %p (%u bytes) stride %u off %u sum %08x first:%s", g_dumpPass, (long)g_drawsBound, sl, (void*)xb, xd.ByteWidth, xs, xo, xsum, fx);
            xb->Release();
        }
        {   // input layout is not readable through D3D, but stream 0 and the index buffer are: sums of both
            ID3D11Buffer* v0 = nullptr; UINT s0 = 0, o0 = 0; c->IAGetVertexBuffers(0, 1, &v0, &s0, &o0);
            if (v0) { static uint8_t q[256]; D3D11_BUFFER_DESC vd; v0->GetDesc(&vd); UINT nb0 = vd.ByteWidth < 256 ? vd.ByteWidth : 256; uint32_t vs0 = BufRange(c, v0, 0, nb0, q) ? Sum32(q, nb0) : 0;
                Log("pinst:   pass %d draw %ld stream 0 buf %p stride %u off %u sum %08x", g_dumpPass, (long)g_drawsBound, (void*)v0, s0, o0, vs0); v0->Release(); }
        }
    }
    {   // 104: each distinct pixel shader of the layer is described once per pass (constant buffer names, sizes, values)
        static void* seen[2][48]; static int nSeen[2]; static int forDump = -1;
        if (forDump != g_dumpN) { forDump = g_dumpN; nSeen[0] = nSeen[1] = 0; }
        int ps = g_dumpPass; bool have = false; for (int i = 0; i < nSeen[ps]; i++) if (seen[ps][i] == g_ps) have = true;
        if (!have && nSeen[ps] < 48 && d.Width > 1024) {
            seen[ps][nSeen[ps]++] = g_ps;
            static float pv[6][4096]; const float* pp[6]; int pn[6];
            for (int i = 0; i < 6; i++) { pn[i] = 0; pp[i] = pv[i]; ID3D11Buffer* b = nullptr; c->PSGetConstantBuffers(i, 1, &b); if (b) { CbRead(c, b, pv[i], &pn[i]); b->Release(); } }
            char tag[64]; snprintf(tag, sizeof tag, "player PS pass %d draw %ld", ps, (long)g_drawsBound);
            ID3D11PixelShader* sh = nullptr; c->PSGetShader(&sh, nullptr, nullptr);
            if (sh) { ShaderDescribe(tag, sh, pp, pp, pn, pn, 6, false); sh->Release(); }
        }
    }
    {   // 105: the same for each distinct vertex shader, plus its textures; and what the half-size depth texture the particles fade against holds
        static void* seenV[2][48]; static int nV[2]; static int forDumpV = -1; static bool depthDone[2];
        if (forDumpV != g_dumpN) { forDumpV = g_dumpN; nV[0] = nV[1] = 0; depthDone[0] = depthDone[1] = false; }
        int ps = g_dumpPass; bool have = false; for (int i = 0; i < nV[ps]; i++) if (seenV[ps][i] == g_vs) have = true;
        if (!have && nV[ps] < 48 && d.Width > 1024) {
            seenV[ps][nV[ps]++] = g_vs;
            static float vv[6][4096]; const float* vp[6]; int vn[6];
            for (int i = 0; i < 6; i++) { vn[i] = 0; vp[i] = vv[i]; ID3D11Buffer* b = nullptr; c->VSGetConstantBuffers(i, 1, &b); if (b) { CbRead(c, b, vv[i], &vn[i]); b->Release(); } }
            char tag[64]; snprintf(tag, sizeof tag, "player VS pass %d draw %ld", ps, (long)g_drawsBound);
            ID3D11VertexShader* sh = nullptr; c->VSGetShader(&sh, nullptr, nullptr);
            if (sh) { ShaderDescribe(tag, sh, vp, vp, vn, vn, 6, false); sh->Release(); }
            ID3D11ShaderResourceView* vr[16] = {}; c->VSGetShaderResources(0, 16, vr); char vt[400] = ""; int vo = 0;
            for (int i = 0; i < 16; i++) if (vr[i]) { short id = ViewRes(vr[i]); vo += snprintf(vt + vo, sizeof vt - vo, " t%d=#%d(%dx%d f%d)", i, id, id >= 0 ? g_resW[id] : 0, id >= 0 ? g_resH[id] : 0, id >= 0 ? g_resF[id] : 0); vr[i]->Release(); }
            Log("pdraw:   VS textures:%s", vo ? vt : " none");
        }
        if (!depthDone[ps] && d.Width > 1024) {
            ID3D11ShaderResourceView* pr[4] = {}; c->PSGetShaderResources(0, 4, pr);
            for (int i = 0; i < 4; i++) { if (!pr[i]) continue;
                ID3D11Resource* rr = nullptr; pr[i]->GetResource(&rr); ID3D11Texture2D* tt = nullptr; if (rr) { rr->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&tt); rr->Release(); }
                if (tt) { D3D11_TEXTURE2D_DESC td; tt->GetDesc(&td);
                    if (!depthDone[ps] && (td.Format == 44 || td.Format == 19 || td.Format == 39) && td.Width >= 1024) {
                        depthDone[ps] = true;
                        D3D11_TEXTURE2D_DESC sd = td; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
                        ID3D11Device* dev = nullptr; c->GetDevice(&dev); ID3D11Texture2D* st = nullptr;
                        if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
                            c->CopyResource(st, tt); D3D11_MAPPED_SUBRESOURCE m;
                            if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
                                double sum = 0; UINT n0 = 0, n1 = 0, n = 0; uint32_t h = 2166136261u; float mn = 2, mx = -1; float row[8] = {};
                                for (UINT y = 0; y < td.Height; y += 4) { const uint32_t* r4 = (const uint32_t*)((const uint8_t*)m.pData + (size_t)y * m.RowPitch);
                                    for (UINT x = 0; x < td.Width; x += 4) { uint32_t v = r4[x] & 0xFFFFFF; float z = (float)v / 16777215.f; sum += z; n++; if (v == 0) n0++; if (v == 0xFFFFFF) n1++; if (z < mn) mn = z; if (z > mx) mx = z; h ^= v; h *= 16777619u;
                                        if (y == (td.Height / 2 & ~3u) && x % (td.Width / 8 & ~3u) == 0 && x / (td.Width / 8 & ~3u) < 8) row[x / (td.Width / 8 & ~3u)] = z; } }
                                Log("pdraw:   pass %d particle depth texture #%d %ux%u fmt %d (PS slot %d): mean %.6f min %.6f max %.6f, zero %u, one %u of %u samples, sum %08x; middle row: %.5f %.5f %.5f %.5f %.5f %.5f %.5f %.5f",
                                    ps, ResId(tt), td.Width, td.Height, (int)td.Format, i, n ? sum / n : 0.0, mn, mx, n0, n1, n, h, row[0], row[1], row[2], row[3], row[4], row[5], row[6], row[7]);
                                c->Unmap(st, 0);
                            }
                            st->Release();
                        }
                        if (dev) dev->Release();
                    }
                    tt->Release(); }
                pr[i]->Release(); }
        }
    }
    if (d.Width > 1024) {   // 111: a small picture of the layer after EVERY draw, and the fixed-function state of the non-instanced draws
        wchar_t nm[160]; swprintf(nm, 160, L"sdvr_pd%02d_p%d_d%03ld.bmp", g_dumpN, g_dumpPass, (long)g_drawsBound);
        g_dumpStepOverride = 8; DumpTarget(c, g_dumpRt, nm); g_dumpStepOverride = 0;
        if (cnt < 100000) {
            ID3D11DepthStencilState* ds = nullptr; UINT ref = 0; c->OMGetDepthStencilState(&ds, &ref); char dz[160] = "no depth state";
            if (ds) { D3D11_DEPTH_STENCIL_DESC q; ds->GetDesc(&q); snprintf(dz, sizeof dz, "depth %d write %d func %d stencil %d ref %u ffunc %d", q.DepthEnable, q.DepthWriteMask, q.DepthFunc, q.StencilEnable, ref, q.FrontFace.StencilFunc); ds->Release(); }
            ID3D11BlendState* bs2 = nullptr; FLOAT bf2[4]; UINT sm2 = 0; c->OMGetBlendState(&bs2, bf2, &sm2); int be = -1, wm = -1, sb = -1, db = -1; if (bs2) { D3D11_BLEND_DESC q; bs2->GetDesc(&q); be = q.RenderTarget[0].BlendEnable; wm = q.RenderTarget[0].RenderTargetWriteMask; sb = q.RenderTarget[0].SrcBlend; db = q.RenderTarget[0].DestBlend; bs2->Release(); }
            D3D11_VIEWPORT vp[1]; UINT nv = 1; c->RSGetViewports(&nv, vp);
            uint32_t cs[2][4] = {}; int cn[2][4] = {};
            for (int st = 0; st < 2; st++) for (int i = 0; i < 4; i++) { ID3D11Buffer* b = nullptr; if (st == 0) c->PSGetConstantBuffers(i, 1, &b); else c->VSGetConstantBuffers(i, 1, &b); if (!b) continue;
                static float v[4096]; int n = 0; CbRead(c, b, v, &n); b->Release(); cn[st][i] = n; cs[st][i] = Sum32(v, (size_t)n * 4); }
            Log("pst: pass %d draw %ld count %u ps %p vs %p | %s | blend on %d %d/%d mask %x | viewport %.0f %.0f %.0f %.0f | PS cb %08x/%d %08x/%d %08x/%d %08x/%d VS cb %08x/%d %08x/%d %08x/%d %08x/%d",
                g_dumpPass, (long)g_drawsBound, cnt, g_ps, g_vs, dz, be, sb, db, wm, nv ? vp[0].TopLeftX : -1.f, nv ? vp[0].TopLeftY : -1.f, nv ? vp[0].Width : -1.f, nv ? vp[0].Height : -1.f,
                cs[0][0], cn[0][0], cs[0][1], cn[0][1], cs[0][2], cn[0][2], cs[0][3], cn[0][3], cs[1][0], cn[1][0], cs[1][1], cn[1][1], cs[1][2], cn[1][2], cs[1][3], cn[1][3]);
        }
    }
    if (g_drawsBound % 12 == 0) {   // 103: picture of the layer every 12 draws, to see at which draws the mist appears in each eye
        wchar_t nm[160]; swprintf(nm, 160, L"sdvr_pl%02d_p%d_g%02d_d%03ld.bmp", g_dumpN, g_dumpPass, g_dumpSeq[g_dumpPass], (long)g_drawsBound);
        DumpTarget(c, g_dumpRt, nm);
    }
    if (vb) vb->Release(); if (ib) ib->Release();
}
static void DumpAfter(ID3D11DeviceContext* c, UINT cnt) {
    ParticleLine(c, cnt);
    if (g_mode != 0 || g_dump != 2 || GetCurrentThreadId() != g_dumpTid || !g_dumpRt || g_drawsBound > (g_dumpPass == 2 ? 60 : 12) || g_dumpPass > 2) return;   // 121: the HUD pass too (60 draws)
    ID3D11Resource* r = nullptr; g_dumpRt->GetResource(&r); if (!r) return;
    ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return;
    D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); int rid = ResId(t); t->Release();
    if (d.Width < 2048 || d.Format != 28) return;
    int g = g_dumpSeq[g_dumpPass]; LONG k = g_drawsBound;
    // state of this draw
    char tx[400]; int o = 0;
    ID3D11ShaderResourceView* sr[8] = {}; c->PSGetShaderResources(0, 8, sr);
    for (int i = 0; i < 8; i++) { if (!sr[i]) continue; short id = ViewRes(sr[i]); o += snprintf(tx + o, sizeof tx - o, " t%d=#%d(%dx%d f%d)", i, id, id >= 0 ? g_resW[id] : 0, id >= 0 ? g_resH[id] : 0, id >= 0 ? g_resF[id] : 0); sr[i]->Release(); }
    ID3D11DepthStencilState* ds = nullptr; UINT ref = 0; c->OMGetDepthStencilState(&ds, &ref); char dz[200] = "no depth state";
    if (ds) { D3D11_DEPTH_STENCIL_DESC q; ds->GetDesc(&q); snprintf(dz, sizeof dz, "depth %d write %d func %d | stencil %d rm %02x wm %02x ref %u front func %d pass %d fail %d zfail %d back func %d",
        q.DepthEnable, q.DepthWriteMask, q.DepthFunc, q.StencilEnable, q.StencilReadMask, q.StencilWriteMask, ref, q.FrontFace.StencilFunc, q.FrontFace.StencilPassOp, q.FrontFace.StencilFailOp, q.FrontFace.StencilDepthFailOp, q.BackFace.StencilFunc); ds->Release(); }
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm = 0; c->OMGetBlendState(&bs, bf, &sm); char bl[100] = "no blend";
    if (bs) { D3D11_BLEND_DESC q; bs->GetDesc(&q); snprintf(bl, sizeof bl, "blend %d src %d dst %d op %d mask %x", q.RenderTarget[0].BlendEnable, q.RenderTarget[0].SrcBlend, q.RenderTarget[0].DestBlend, q.RenderTarget[0].BlendOp, q.RenderTarget[0].RenderTargetWriteMask); bs->Release(); }
    ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs); int cull = -1; if (rs) { D3D11_RASTERIZER_DESC q; rs->GetDesc(&q); cull = q.CullMode; rs->Release(); }
    short did = -1; if (g_dumpDsv) did = ViewRes(g_dumpDsv);
    ID3D11RenderTargetView* rv[8] = {}; c->OMGetRenderTargets(8, rv, nullptr); char rts[160]; int ro = 0;
    for (int i = 0; i < 8; i++) if (rv[i]) { short id = ViewRes(rv[i]); ro += snprintf(rts + ro, sizeof rts - ro, " rt%d=#%d(%dx%d f%d)", i, id, id >= 0 ? g_resW[id] : 0, id >= 0 ? g_resH[id] : 0, id >= 0 ? g_resF[id] : 0); rv[i]->Release(); }
    if (!ro) rts[0] = 0;
    Log("step: pass %d group %02d (target #%d, bound %u:%s) draw %ld count %u: ps %p (%016llx) vs %p | dsv #%d | %s | %s | cull %d |%s", g_dumpPass, g, rid, g_dumpRtN, rts, k, cnt, g_ps, (unsigned long long)ShaderHash(g_ps), g_vs, did, dz, bl, cull, tx);
    { char nmz[900]; ShaderNames(g_ps, nmz, sizeof nmz); Log("step:   PS names:%s", nmz); }
    if (g_dumpPass == 2) { wchar_t nm[160]; swprintf(nm, 160, L"sdvr_hd%02d_g%02d_d%03ld.bmp", g_dumpN, g, (long)k); g_dumpStepOverride = 4; DumpTarget(c, g_dumpRt, nm); g_dumpStepOverride = 0; }   // 121: HUD after each draw
    for (int st = 0; st < 2; st++) for (int i = 0; i < 4; i++) {   // constants: PS cb0-3, VS cb0-3 (first 24 floats)
        ID3D11Buffer* b = nullptr; if (st == 0) c->PSGetConstantBuffers(i, 1, &b); else c->VSGetConstantBuffers(i, 1, &b); if (!b) continue;
        static float v[4096]; int n = 0; CbRead(c, b, v, &n); b->Release(); if (!n) continue;
        char line[700]; int q = 0; for (int j = 0; j < n && j < 24; j++) q += snprintf(line + q, sizeof line - q, " %.4g", v[j]);
        Log("step:   %s cb%d (%d floats):%s", st ? "VS" : "PS", i, n, line);
    }
    return;   // 058: step images off (the composite draw is known), state log kept
    wchar_t name[160];
    if (k == 1 && g_dumpDsv) { swprintf(name, 160, L"sdvr_step%02d_p%d_g%02d", g_dumpN, g_dumpPass, g); DumpDepth(c, g_dumpDsv, name); }
    swprintf(name, 160, L"sdvr_step%02d_p%d_g%02d_d%02ld.bmp", g_dumpN, g_dumpPass, g, k);
    DumpTarget(c, g_dumpRt, name);
}
void ShaderDescribe(const char* tag, void* sh, const float* const* v0, const float* const* v1, const int* n0, const int* n1, int slots, bool diffOnly = false);
int ShaderDiffVars(void* sh, const float* const* v0, const float* const* v1, const int* n0, const int* n1, int slots, char* out, int cap);
int ShaderFindRes(void* sh, const char* name); int ShaderCbMask(void* sh, const char* exclude);
static void CbReport() {
    if (!g_skipPs) return;
    for (int f = 0; f < 2; f++) if (g_cbN[f][0] && g_cbN[f][1]) for (int st = 0; st < 2; st++) {
        const float* a[6]; const float* b[6]; int na[6], nb[6];
        for (int i = 0; i < 6; i++) { a[i] = g_cb[f][0][0][st][i]; b[i] = g_cb[f][1][0][st][i]; na[i] = g_cbSz[f][0][0][st][i]; nb[i] = g_cbSz[f][1][0][st][i]; }
        char tag[48]; snprintf(tag, sizeof tag, "frame %c draw 0 %s", 'A' + f, st ? "VS" : "PS");
        ShaderDescribe(tag, st ? g_cbVs : g_skipPs, a, b, na, nb, 6);
    }
    Log("cbuf: shader %p - draws recorded: frame A pass0 %d pass1 %d, frame B pass0 %d pass1 %d", g_skipPs, g_cbN[0][0], g_cbN[0][1], g_cbN[1][0], g_cbN[1][1]);
    for (int f = 0; f < 2; f++) for (int k = 0; k < 4; k++) {
        if (k >= g_cbN[f][0] || k >= g_cbN[f][1]) continue;
        for (int st = 0; st < 2; st++) for (int i = 0; i < 6; i++) {
            int n = g_cbSz[f][0][k][st][i] < g_cbSz[f][1][k][st][i] ? g_cbSz[f][0][k][st][i] : g_cbSz[f][1][k][st][i];
            if (!n) continue;
            char line[1600]; int o = 0, nd = 0;
            for (int j = 0; j < n && o < 1500; j++) { float a = g_cb[f][0][k][st][i][j], b = g_cb[f][1][k][st][i][j]; if (a != b) { nd++; o += snprintf(line + o, sizeof line - o, " [%d] %.4g/%.4g", j, a, b); } }
            Log("cbuf: frame %c draw %d %s cb%d (%d floats): %d differ (pass0/pass1)%s", 'A' + f, k, st ? "VS" : "PS", i, n, nd, nd ? ":" : "");
            if (nd) Log("cbuf:   %s", line);
            if (f == 0 && k == 0 && st == 0) {   // full PS buffers of the first draw, pass 0
                o = 0; for (int j = 0; j < n && o < 1500; j++) o += snprintf(line + o, sizeof line - o, " %.4g", g_cb[0][0][0][0][i][j]);
                Log("cbuf:   pass0 PS cb%d:%s", i, line);
            }
        }
    }
}
void RtOutlineArm() { memset(g_cbN, 0, sizeof g_cbN); memset(g_nObj, 0, sizeof g_nObj); memset(g_cbSz, 0, sizeof g_cbSz); InterlockedExchange(&g_ol, 1); }
void RtMinimapFitViewport(D3D11_VIEWPORT* v) {
    float a = v->Width / v->Height;
    if (a < g_mmAspect) { float h = v->Width / g_mmAspect; v->TopLeftY += (v->Height - h) * 0.5f; v->Height = h; }
    else { float w = v->Height * g_mmAspect; v->TopLeftX += (v->Width - w) * 0.5f; v->Width = w; }
}
static volatile bool g_olTrace;
void RtOutlineTrace(bool on) { g_olTrace = on; }
static void DiffDraw();
static void OlDraw() { DiffDraw(); if (g_olTrace) if (OlE* e = OlAdd('D')) { (void)e; } }
static void STDMETHODCALLTYPE hkVP(ID3D11DeviceContext* c, UINT n, const D3D11_VIEWPORT* v) {
    D3D11_VIEWPORT mv;
    if (g_mmHud && g_mmFitOn && n == 1 && v && v[0].TopLeftX == 0 && v[0].TopLeftY == 0 && v[0].Width == g_eyeW && v[0].Height == g_eyeH && g_eyeW > 0) {   // the minimap pass: fit the UI stage
        mv = v[0]; float a = mv.Width / mv.Height;
        if (a < g_mmAspect) { float h = mv.Width / g_mmAspect; mv.TopLeftY = (mv.Height - h) * 0.5f; mv.Height = h; }
        else { float w = mv.Height * g_mmAspect; mv.TopLeftX = (mv.Width - w) * 0.5f; mv.Width = w; }
        v = &mv;
    }
    if (g_ol == 2 && n && v) if (OlE* e = OlAdd('V')) { e->a = (int)v[0].TopLeftX; e->b = (int)v[0].TopLeftY; e->c = (int)v[0].Width; e->d = (int)v[0].Height; }
    oVP(c, n, v);
}
static void STDMETHODCALLTYPE hkSC(ID3D11DeviceContext* c, UINT n, const D3D11_RECT* r) {
    if (g_ol == 2 && n && r) if (OlE* e = OlAdd('S')) { e->a = r[0].left; e->b = r[0].top; e->c = r[0].right - r[0].left; e->d = r[0].bottom - r[0].top; }
    oSC(c, n, r);
}
static void CbDump(ID3D11DeviceContext* c);
// build 059: G-buffer draw comparison. In the dump frame every draw into the G-buffer (3+ targets bound) is recorded in
// both eye passes, keyed by its geometry (index/vertex buffer, counts, offsets). Draws whose pixel shader has an
// emission texture keep their PS/VS constants (slots 0-3). At the end the passes are matched and reported: draws
// whose shaders, textures or states differ, draws only one pass has, and per emissive shader the constants that differ.
struct GbD { void* ps; void* vs; void* ib; void* vb; UINT a, b, d, e; void* bs; void* ds; void* rs; UINT ref; short srv[16]; short vsrv[8]; float* cb[8]; int n[8]; bool used, em; };
static GbD* g_gb[2]; static int g_gbN[2];
typedef HRESULT (STDMETHODCALLTYPE *Map_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
typedef void (STDMETHODCALLTYPE *USR_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, const D3D11_BOX*, const void*, UINT, UINT);
typedef void (STDMETHODCALLTYPE *SCB_t)(ID3D11DeviceContext*, UINT, UINT, ID3D11Buffer* const*);
static Map_t oMap; static USR_t oUSR; static SCB_t oPSCB, oVSCB;
// build 069: SceneryFix. In the first eye pass some emissive scenery (lanterns, signs, ...) is drawn while the pixel
// shader's cbSceneryInstance slot still holds another material's static "look" buffer: the game did not bind an instance
// block for the draw, so the colours/emission scale are another material's values (068 capture: eye-0 lanterns had a
// non-dynamic 64-byte buffer there, every correct draw has the shader's dynamic instance buffer). Fix at the D3D level:
// for a draw whose PS declares cbSceneryInstance and whose slot holds a buffer that is not DYNAMIC, bind a buffer with
// the game's default instance block (as built at +0x57eaf: colour 0x14203bcf8 twice, 0x14235b2a0 twice) for that draw.
volatile int g_sceneryFix = 1; volatile LONG g_sfFixed[3], g_sfSeen[3], g_sfNull[3];
static struct { void* ps; signed char slot; unsigned short others; } g_sfCache[8192]; static unsigned short g_sfOthers; static ID3D11Buffer* g_sfBuf;
static void* g_wrSet[16384]; static unsigned g_wrCnt[16384];
struct SfUse { unsigned w; void* vb; void* ib; bool fixed; }; static SfUse g_sfUse[16384];
static int WrFind(void* b) { unsigned h = (unsigned)(((uintptr_t)b >> 4) * 2654435761u) & 16383; for (int i = 0; i < 32; i++) { int k = (h + i) & 16383; void* e = g_wrSet[k]; if (e == b) return k; if (!e) return -1; } return -1; }
static unsigned short g_wrFull[16384];   // 112: full byte size of the buffer (capped)
static unsigned short g_wrSize[16384]; static float g_wrData[16384][16];   // 073: first 64 bytes last written to each buffer
static int WrAdd(ID3D11Resource* r) {
    if (!r) return -1; unsigned h = (unsigned)(((uintptr_t)r >> 4) * 2654435761u) & 16383;
    for (int i = 0; i < 32; i++) { int k = (h + i) & 16383; void*& e = g_wrSet[k]; if (e == r) { g_wrCnt[k]++; return k; }
        if (!e) { D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim); if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return -1;
            D3D11_BUFFER_DESC bd; ((ID3D11Buffer*)r)->GetDesc(&bd); if (!(bd.BindFlags & D3D11_BIND_CONSTANT_BUFFER)) return -1;
            e = r; g_wrCnt[k] = 1; g_wrSize[k] = (unsigned short)(bd.ByteWidth > 64 ? 64 : bd.ByteWidth); g_wrFull[k] = (unsigned short)(bd.ByteWidth > 65535 ? 65535 : bd.ByteWidth); return k; } }
    return -1;
}
static struct { ID3D11Resource* r; const void* p; int k; } g_mapPend[8];
// 071: is the instance block in this slot stale for this draw? Never written = a material's look buffer (070). Written,
// but not since the last draw that used it, and that draw had different geometry = another object's block left over
// (mode 2). A draw with the same geometry as the last user follows what was decided for that one.
static bool SfStale(ID3D11DeviceContext* c, ID3D11Buffer* cur) {
    int k = WrFind(cur); if (k < 0) return true;
    if (g_sceneryFix != 2) return false;
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT f; UINT o = 0; c->IAGetIndexBuffer(&ib, &f, &o); if (ib) ib->Release();
    ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0; c->IAGetVertexBuffers(0, 1, &vb, &st, &of); if (vb) vb->Release();
    SfUse& u = g_sfUse[k]; bool fixed;
    if (u.w != g_wrCnt[k]) fixed = false;                         // written for this draw
    else if (u.vb == vb && u.ib == ib) fixed = u.fixed;           // another part / copy of the last object
    else fixed = true;                                            // a different object and nothing written for it
    u.w = g_wrCnt[k]; u.vb = vb; u.ib = ib; u.fixed = fixed;
    return fixed;
}
// 073: block memory. A draw with a real instance block records it under the object's identity (vertex/index buffer,
// index count, world position from the VS cbLocalTransform); a draw left without a block gets the block last recorded
// for the same object (normally from the other eye of the previous frame), or plain white if there is none.
struct SfMem { uint64_t key; float v[16]; }; static SfMem g_sfMem[8192]; static volatile LONG g_sfHit, g_sfMiss, g_sfRec;
static struct { void* vs; signed char slot; } g_sfVs[8192];
static int SfVsSlot(void* vs) {
    unsigned h = (unsigned)(((uintptr_t)vs >> 4) * 2654435761u) & 8191;
    for (int i = 0; i < 64; i++) { auto& e = g_sfVs[(h + i) & 8191]; if (e.vs == vs) return e.slot; if (!e.vs) { int sl = ShaderFindRes(vs, "cbLocalTransform"); e.slot = (signed char)(sl >= 0 && sl < 14 ? sl : -1); e.vs = vs; return e.slot; } }
    return -1;
}
static float g_sfLastPos[3];
static uint64_t SfPos(ID3D11DeviceContext* c) {   // the object's world position (VS cbLocalTransform row 3), 0 if unknown
    int vsl = g_vs ? SfVsSlot(g_vs) : -1; if (vsl < 0) return 0;
    ID3D11Buffer* tb = nullptr; c->VSGetConstantBuffers(vsl, 1, &tb); if (!tb) return 0; int i = WrFind(tb); tb->Release();
    g_sfLastPos[0] = g_sfLastPos[1] = g_sfLastPos[2] = 0;
    if (i < 0 || g_wrSize[i] < 64) return 0; const float* m = g_wrData[i];
    g_sfLastPos[0] = m[12]; g_sfLastPos[1] = m[13]; g_sfLastPos[2] = m[14];
    int q[3] = { (int)floorf(m[12] * 8.f + .5f), (int)floorf(m[13] * 8.f + .5f), (int)floorf(m[14] * 8.f + .5f) };
    uint64_t k = (uint64_t)(uint32_t)q[0] * 0x9E3779B1u ^ ((uint64_t)(uint32_t)q[1] * 0x85EBCA77u << 16) ^ ((uint64_t)(uint32_t)q[2] * 0xC2B2AE3Du << 32);
    return k ? k : 1;
}
static uint64_t SfKey(ID3D11DeviceContext* c, UINT count, uint64_t pos) {
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT f; UINT o = 0; c->IAGetIndexBuffer(&ib, &f, &o); if (ib) ib->Release();
    ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0; c->IAGetVertexBuffers(0, 1, &vb, &st, &of); if (vb) vb->Release();
    uint64_t k = (uintptr_t)vb * 0x9E3779B97F4A7C15ull ^ (uintptr_t)ib * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)count * 0x165667B19E3779F9ull ^ pos;
    return k ? k : 1;
}
static uint64_t g_sfObj[16384]; static unsigned g_sfW[16384]; static unsigned char g_sfSt[16384]; static volatile LONG g_sfLeft, g_sfKept;
static SfMem* SfMemGet(uint64_t key, bool add) {
    unsigned h = (unsigned)(key ^ (key >> 29)) & 8191;
    for (int i = 0; i < 16; i++) { SfMem& e = g_sfMem[(h + i) & 8191]; if (e.key == key) return &e; if (!e.key) { if (!add) return nullptr; e.key = key; return &e; } }
    if (add) { SfMem& e = g_sfMem[h]; e.key = key; return &e; }   // full neighbourhood: overwrite
    return nullptr;
}
static int SfSlot(void* ps) {
    unsigned h = (unsigned)(((uintptr_t)ps >> 4) * 2654435761u) & 8191;
    for (int i = 0; i < 64; i++) { auto& e = g_sfCache[(h + i) & 8191]; if (e.ps == ps) { g_sfOthers = e.others; return e.slot; }
        if (!e.ps) { int sl = ShaderFindRes(ps, "cbSceneryInstance"); e.slot = (signed char)(sl >= 0 && sl < 14 ? sl : -1); e.others = e.slot >= 0 ? (unsigned short)ShaderCbMask(ps, "cbSceneryInstance") : 0; e.ps = ps; g_sfOthers = e.others; return e.slot; } }
    return -1;
}
// 076: what each written buffer is used for - votes from draws: bound in a slot the shader declares for something else
// (environment settings, material look, ...) vs holding a freshly written block in the instance slot.
static unsigned char g_sfVoteOther[16384], g_sfVoteInst[16384]; static volatile LONG g_sfRole;
static UINT g_sfCount;
static ID3D11Buffer* SceneryFixBegin(ID3D11DeviceContext* c, int* slotOut) {
    if (!g_sceneryFix || !g_ps) return nullptr;
    int sl = SfSlot(g_ps); if (sl < 0) return nullptr;
    int ctx = g_sfEye < 0 || g_sfEye > 1 ? 2 : g_sfEye;
    ID3D11Buffer* cur = nullptr; c->PSGetConstantBuffers(sl, 1, &cur);
    InterlockedIncrement(&g_sfSeen[ctx]);
    if (!cur) InterlockedIncrement(&g_sfNull[ctx]);
    const float* give = nullptr; static const float kWhite[16] = { 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 };
    bool bad = false;
    if (g_sceneryFix == 1) {
        // 074: fresh = the game wrote the block since the buffer's last use. Not written but the same object (same world
        // position) as the last user = another part of it. Otherwise the block belongs to another object ("left over").
        int k = cur ? WrFind(cur) : -1; uint64_t pos = SfPos(c); bool never = k < 0;
        for (int sl2 = 0, m2 = g_sfOthers; m2; sl2++, m2 >>= 1) if ((m2 & 1) && sl2 != sl) { ID3D11Buffer* ob = nullptr; c->PSGetConstantBuffers(sl2, 1, &ob); if (ob) { int j = WrFind(ob); ob->Release(); if (j >= 0 && g_sfVoteOther[j] < 250) g_sfVoteOther[j]++; } }
        if (!never && g_sfVoteOther[k] > g_sfVoteInst[k]) { never = true; InterlockedIncrement(&g_sfRole); k = -1; }   // some other kind of buffer sits in the instance slot
        bool stale = never;
        if (!never) {
            if (g_sfW[k] != g_wrCnt[k]) stale = false; else if (pos && g_sfObj[k] == pos) stale = g_sfSt[k] != 0; else stale = true;
            g_sfW[k] = g_wrCnt[k]; g_sfObj[k] = pos; g_sfSt[k] = stale;
        }
        uint64_t key = SfKey(c, g_sfCount, pos);
        if (!stale && g_sfVoteInst[k] < 250) g_sfVoteInst[k] += 2;
        if (!stale) { if (g_wrSize[k] >= 32) { SfMem* e = SfMemGet(key, true); if (e) { memcpy(e->v, g_wrData[k], 64); InterlockedIncrement(&g_sfRec); } } }
        else { SfMem* e = SfMemGet(key, false);
            if (e) { give = e->v; InterlockedIncrement(&g_sfHit); if (!never) InterlockedIncrement(&g_sfLeft); }
            else if (never) { give = kWhite; InterlockedIncrement(&g_sfMiss); }
            else InterlockedIncrement(&g_sfKept);   // left-over block and no record of this object: leave it (may be a shared tint)
        }
        bad = give != nullptr;
        if (g_dump == 2 && g_mode == 2 && GetCurrentThreadId() == g_dumpTid && g_dumpPass < 2 && IsEyeGbuf()) {   // 075: decision log (Shift+F10)
            static int cnt[2]; static long lastN = -1; if (lastN != g_dumpN) { lastN = g_dumpN; cnt[0] = cnt[1] = 0; }
            if (cnt[g_dumpPass]++ < 1500) { const float* v = k >= 0 ? g_wrData[k] : kWhite; const float* gv = give ? give : v;
                Log("inst: pass %d seq %ld count %u ps %p buf %p writes %u pos %.2f %.2f %.2f | %s | has %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g | %s %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g / %.3g %.3g %.3g %.3g",
                    g_dumpPass, g_dseq, g_sfCount, g_ps, cur, k >= 0 ? g_wrCnt[k] : 0, g_sfLastPos[0], g_sfLastPos[1], g_sfLastPos[2],
                    never ? "never written" : !stale ? "fresh/same object" : "left over", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11],
                    give ? (give == kWhite ? "GIVEN white" : "GIVEN record") : "kept", gv[0], gv[1], gv[2], gv[3], gv[4], gv[5], gv[6], gv[7], gv[8], gv[9], gv[10], gv[11]); }
        }
    } else {
        if (cur) bad = SfStale(c, cur); else bad = true;
    }
    if (!bad) { if (cur) cur->Release(); return nullptr; }
    if (!g_sfBuf) { ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (dev) { D3D11_BUFFER_DESC bd = {}; bd.ByteWidth = 64; bd.Usage = D3D11_USAGE_DYNAMIC; bd.BindFlags = D3D11_BIND_CONSTANT_BUFFER; bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE; dev->CreateBuffer(&bd, nullptr, &g_sfBuf); dev->Release(); } }
    if (!g_sfBuf) { if (cur) cur->Release(); return nullptr; }
    D3D11_MAPPED_SUBRESOURCE m;
    if (SUCCEEDED(oMap ? oMap(c, g_sfBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m) : c->Map(g_sfBuf, 0, D3D11_MAP_WRITE_DISCARD, 0, &m))) {
        float* f = (float*)m.pData; const float* col = (const float*)(g_base + 0x203bcf8); const float* g = (const float*)(g_base + 0x235b2a0);
        if (give) memcpy(f, give, 64);
        else if (g_sceneryFix == 3) { memcpy(f, col, 16); memcpy(f + 4, col, 16); memcpy(f + 8, g, 16); memcpy(f + 12, g, 16); }   // the block built at +0x57eaf
        else for (int i = 0; i < 16; i++) f[i] = 1.f;
        c->Unmap(g_sfBuf, 0);
    }
    if (oPSCB) oPSCB(c, sl, 1, &g_sfBuf); else c->PSSetConstantBuffers(sl, 1, &g_sfBuf);
    InterlockedIncrement(&g_sfFixed[ctx]);
    *slotOut = sl; return cur ? cur : (ID3D11Buffer*)1;
}
static void SceneryFixEnd(ID3D11DeviceContext* c, ID3D11Buffer* old, int sl) {
    if (!old) return; ID3D11Buffer* o = old == (ID3D11Buffer*)1 ? nullptr : old;
    if (oPSCB) oPSCB(c, sl, 1, &o); else c->PSSetConstantBuffers(sl, 1, &o);
    if (o) o->Release();
}
void RtSceneryFixStats(char* b, int n) { const float* col = (const float*)(g_base + 0x203bcf8); const float* g = (const float*)(g_base + 0x235b2a0); int o = snprintf(b, n, "game default block: %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g; ", col[0], col[1], col[2], col[3], g[0], g[1], g[2], g[3]); b += o; n -= o; o = snprintf(b, n, "block memory: recorded %ld, reused %ld (of them left-over blocks replaced %ld), no record: white %ld, left as is %ld, wrong kind of buffer in the slot %ld; ", (long)g_sfRec, (long)g_sfHit, (long)g_sfLeft, (long)g_sfMiss, (long)g_sfKept, (long)g_sfRole); b += o; n -= o; snprintf(b, n, "scenery fix %d: draws with an instance slot eye0 %ld eye1 %ld other %ld; given the default block eye0 %ld eye1 %ld other %ld; empty slot %ld/%ld/%ld", g_sceneryFix, g_sfSeen[0], g_sfSeen[1], g_sfSeen[2], g_sfFixed[0], g_sfFixed[1], g_sfFixed[2], g_sfNull[0], g_sfNull[1], g_sfNull[2]); }
static void GbDraw(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    if (g_dump != 2 || GetCurrentThreadId() != g_dumpTid || g_dumpRtN < 3 || g_dumpPass > 1 || g_mode == 1) return;
    if (!g_dumpRt) return;
    void* kib = nullptr; void* kvb = nullptr;
    if (g_mode == 2) {   // only the draws the locate frame found
        ID3D11Buffer* ib = nullptr; DXGI_FORMAT ifm; UINT io = 0; c->IAGetIndexBuffer(&ib, &ifm, &io); kib = ib; if (ib) ib->Release();
        ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0; c->IAGetVertexBuffers(0, 1, &vb, &st, &of); kvb = vb; if (vb) vb->Release();
        int si = g_ps ? ShaderFindRes(g_ps, "cbSceneryInstance") : -1;
        if (si >= 0 && si < 4 && ShaderFindRes(g_ps, "texEmission") >= 0) {   // 068: every emissive draw that uses cbSceneryInstance
            static int cnt[2]; static long lastN = -1; if (lastN != g_dumpN) { lastN = g_dumpN; cnt[0] = cnt[1] = 0; }
            int pp = g_dumpPass;
            ID3D11Buffer* cb[3] = {}; c->PSGetConstantBuffers(0, 3, cb);
            float v[4096]; int n = 0; if (cb[si]) CbRead(c, cb[si], v, &n);
            bool white = n >= 8 && v[0] == 1.f && v[1] == 1.f && v[2] == 1.f && v[4] == 1.f;
            g_emN[pp]++; if (white) g_emWhite[pp]++; if (g_psSet[si].buf == cb[si] && g_psSet[si].seq == g_dseq - 1) g_emFresh[pp]++; if (cb[si] && (cb[si] == cb[(si + 1) % 3] || cb[si] == cb[(si + 2) % 3])) g_emSame[pp]++;
            if (cnt[pp]++ < 30 || (!white && cnt[pp] < 200)) Log("emis: pass %d seq %ld count %u ps %p slot %d: cb0 %p cb1 %p cb2 %p | slot bound at seq %ld (%s) | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g | %.4g %.4g %.4g %.4g",
                pp, g_dseq, a, g_ps, si, cb[0], cb[1], cb[2], g_psSet[si].buf == cb[si] ? g_psSet[si].seq : -1L, g_psSet[si].buf == cb[si] ? (g_psSet[si].seq == g_dseq - 1 ? "for this draw" : "earlier") : "before the frame",
                n > 0 ? v[0] : 0, n > 1 ? v[1] : 0, n > 2 ? v[2] : 0, n > 3 ? v[3] : 0, n > 4 ? v[4] : 0, n > 5 ? v[5] : 0, n > 6 ? v[6] : 0, n > 7 ? v[7] : 0, n > 8 ? v[8] : 0, n > 9 ? v[9] : 0, n > 10 ? v[10] : 0, n > 11 ? v[11] : 0, n > 12 ? v[12] : 0, n > 13 ? v[13] : 0, n > 14 ? v[14] : 0, n > 15 ? v[15] : 0);
            for (int i = 0; i < 3; i++) if (cb[i]) cb[i]->Release();
        }
        bool want = false; for (int i = 0; i < g_hitN; i++) if (g_hit[i].ib == kib && g_hit[i].vb == kvb && g_hit[i].a == a && g_hit[i].b == b && g_hit[i].d == d && g_hit[i].ps == g_ps) want = true;
        if (!want) return;
    }
    { ID3D11Resource* r = nullptr; g_dumpRt->GetResource(&r); if (!r) return; ID3D11Texture2D* t = nullptr; r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); if (!t) return;
      D3D11_TEXTURE2D_DESC dd; t->GetDesc(&dd); t->Release(); if (dd.Width < 2048) return; }   // the eye's G-buffer only, not the first-eye extra renders
    int p = g_dumpPass; if (!g_gb[p]) g_gb[p] = (GbD*)calloc(6000, sizeof(GbD)); if (!g_gb[p] || g_gbN[p] >= 6000) return;
    GbD& x = g_gb[p][g_gbN[p]++]; memset(&x, 0, sizeof x);
    x.ps = g_ps; x.vs = g_vs; x.a = a; x.b = b; x.d = d; x.e = e;
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ifm; UINT io = 0; c->IAGetIndexBuffer(&ib, &ifm, &io); x.ib = ib; if (ib) ib->Release();
    ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0; c->IAGetVertexBuffers(0, 1, &vb, &st, &of); x.vb = vb; if (vb) vb->Release();
    ID3D11BlendState* bs = nullptr; FLOAT bf[4]; UINT sm; c->OMGetBlendState(&bs, bf, &sm); x.bs = bs; if (bs) bs->Release();
    ID3D11DepthStencilState* ds = nullptr; c->OMGetDepthStencilState(&ds, &x.ref); x.ds = ds; if (ds) ds->Release();
    ID3D11RasterizerState* rs = nullptr; c->RSGetState(&rs); x.rs = rs; if (rs) rs->Release();
    ID3D11ShaderResourceView* sr[16] = {}; c->PSGetShaderResources(0, 16, sr);
    for (int i = 0; i < 16; i++) { x.srv[i] = sr[i] ? ViewRes(sr[i]) : -1; if (sr[i]) sr[i]->Release(); }
    ID3D11ShaderResourceView* vr[8] = {}; c->VSGetShaderResources(0, 8, vr);
    for (int i = 0; i < 8; i++) { x.vsrv[i] = vr[i] ? ViewRes(vr[i]) : -1; if (vr[i]) vr[i]->Release(); }
    x.em = g_mode == 2 || (g_ps && ShaderFindRes(g_ps, "texEmission") >= 0);
    if (g_mode == 2) { Log("prov: pass %d recorded draw %d (count %u start %u) ps %p", g_dumpPass, g_gbN[p] - 1, a, b, g_ps); ProvLog(c); }
    if (x.em) for (int k = 0; k < 8; k++) {
        ID3D11Buffer* cbb = nullptr; if (k < 4) c->PSGetConstantBuffers(k, 1, &cbb); else c->VSGetConstantBuffers(k - 4, 1, &cbb); if (!cbb) continue;
        static float v[4096]; int n = 0; CbRead(c, cbb, v, &n); cbb->Release(); if (!n) continue;
        x.cb[k] = (float*)malloc(n * 4); if (x.cb[k]) { memcpy(x.cb[k], v, n * 4); x.n[k] = n; }
    }
}
static void GbReport() {
    if (!g_gb[0] || !g_gb[1]) { Log("gbuf: no G-buffer draws recorded (%d / %d)", g_gbN[0], g_gbN[1]); return; }
    Log("gbuf: G-buffer draws pass 0 %d, pass 1 %d (emissive-capable: pass 0 %d)", g_gbN[0], g_gbN[1], [] { int k = 0; for (int i = 0; i < g_gbN[0]; i++) k += g_gb[0][i].em; return k; }());
    struct Agg { void* ps; int draws, srvDiff, stDiff, cbDiff, only0, only1, shDiff; char vars[400]; int described; };
    static Agg ag[256]; int na = 0;
    auto A = [&](void* ps) -> Agg& { for (int i = 0; i < na; i++) if (ag[i].ps == ps) return ag[i]; Agg& g = ag[na < 255 ? na++ : 255]; memset(&g, 0, sizeof g); g.ps = ps; return g; };
    int matched = 0, shaderDiff = 0;
    for (int i = 0; i < g_gbN[0]; i++) {
        GbD& x = g_gb[0][i]; GbD* y = nullptr;
        for (int j = 0; j < g_gbN[1]; j++) { GbD& z = g_gb[1][j]; if (!z.used && z.ib == x.ib && z.vb == x.vb && z.a == x.a && z.b == x.b && z.d == x.d && z.e == x.e) { y = &z; break; } }
        if (!y) { if (x.em) A(x.ps).only0++; continue; }
        y->used = true; matched++;
        if (y->ps != x.ps || y->vs != x.vs) { shaderDiff++; if (shaderDiff <= 40) Log("gbuf: draw %d (count %u start %u base %d): pass 0 ps %p vs %p (emission %d) | pass 1 ps %p vs %p (emission %d)", i, x.a, x.b, (int)x.d, x.ps, x.vs, x.em, y->ps, y->vs, y->em); if (x.em || y->em) A(x.em ? x.ps : y->ps).shDiff++; continue; }
        if (!x.em) continue;
        Agg& g = A(x.ps); g.draws++;
        if (memcmp(x.srv, y->srv, sizeof x.srv)) { g.srvDiff++; if (g.srvDiff <= 3) { char t[200]; int o = 0; for (int k = 0; k < 16; k++) if (x.srv[k] != y->srv[k]) o += snprintf(t + o, sizeof t - o, " t%d #%d/#%d", k, x.srv[k], y->srv[k]); Log("gbuf: ps %p draw %d textures differ (pass 0/pass 1):%s", x.ps, i, t); } }
        if (x.bs != y->bs || x.ds != y->ds || x.rs != y->rs || x.ref != y->ref) { g.stDiff++; if (g.stDiff <= 3) Log("gbuf: ps %p draw %d states differ: blend %p/%p depth %p/%p raster %p/%p ref %u/%u", x.ps, i, x.bs, y->bs, x.ds, y->ds, x.rs, y->rs, x.ref, y->ref); }
        const float* v0[4] = { x.cb[0], x.cb[1], x.cb[2], x.cb[3] }; const float* v1[4] = { y->cb[0], y->cb[1], y->cb[2], y->cb[3] };
        int n0[4] = { x.n[0], x.n[1], x.n[2], x.n[3] }, n1[4] = { y->n[0], y->n[1], y->n[2], y->n[3] };
        char names[400]; int nd = ShaderDiffVars(x.ps, v0, v1, n0, n1, 4, names, sizeof names);
        if (nd > 0) { g.cbDiff++;
            // remember new variable names; describe the draw when it brings a name not seen for this shader (up to 6 per shader)
            bool fresh = false;
            char* tok = names; while (tok && *tok) { char* cm = strchr(tok, ','); if (cm) *cm = 0;
                if (!strstr(g.vars, tok) && strlen(g.vars) + strlen(tok) + 2 < sizeof g.vars) { if (g.vars[0]) strcat(g.vars, ","); strcat(g.vars, tok); fresh = true; }
                tok = cm ? cm + 1 : nullptr; }
            if (fresh && g.described < 6) { g.described++; char tag[64]; snprintf(tag, sizeof tag, "gbuf draw %d PS", i); ShaderDescribe(tag, x.ps, v0, v1, n0, n1, 4, true);
                const float* w0[4] = { x.cb[4], x.cb[5], x.cb[6], x.cb[7] }; const float* w1[4] = { y->cb[4], y->cb[5], y->cb[6], y->cb[7] };
                int m0[4] = { x.n[4], x.n[5], x.n[6], x.n[7] }, m1[4] = { y->n[4], y->n[5], y->n[6], y->n[7] };
                snprintf(tag, sizeof tag, "gbuf draw %d VS", i); ShaderDescribe(tag, x.vs, w0, w1, m0, m1, 4, true); }
        }
    }
    for (int j = 0; j < g_gbN[1]; j++) if (!g_gb[1][j].used && g_gb[1][j].em) A(g_gb[1][j].ps).only1++;
    Log("gbuf: matched %d draws by geometry, %d of them with different shaders", matched, shaderDiff);
    for (int i = 0; i < na; i++) Log("gbuf: emissive ps %p: matched %d, textures differ %d, states differ %d, constants differ %d (%s), shader differs %d, only in pass 0 %d, only in pass 1 %d",
        ag[i].ps, ag[i].draws, ag[i].srvDiff, ag[i].stDiff, ag[i].cbDiff, ag[i].vars, ag[i].shDiff, ag[i].only0, ag[i].only1);
    for (int p = 0; p < 2; p++) { for (int i = 0; i < g_gbN[p]; i++) for (int k = 0; k < 8; k++) free(g_gb[p][i].cb[k]); g_gbN[p] = 0; }
}
// build 062: constant buffer provenance in the state frame - who bound each PS/VS constant buffer and who last wrote it
// (Map / UpdateSubresource), with game call stacks, so the eye-0 lantern draw's wrong cbSceneryInstance can be traced.
struct CbProv { void* buf; uintptr_t w[4]; int wPass; long wSeq; int wCount; };
static CbProv g_prov[2048];
static void Stack4(uintptr_t* o) { void* fr[16]; USHORT k = RtlCaptureStackBackTrace(2, 16, fr, nullptr); int m = 0; for (USHORT i = 0; i < k && m < 4; i++) { uintptr_t a = (uintptr_t)fr[i]; if (a >= g_base && a < g_base + 0x3000000) o[m++] = a - g_base; } while (m < 4) o[m++] = 0; }
static bool ProvOn() { return g_mode == 2 && g_dump == 2 && GetCurrentThreadId() == g_dumpTid; }
static CbProv* ProvGet(void* b, bool add) {
    unsigned h = (unsigned)(((uintptr_t)b >> 4) * 2654435761u) & 2047;
    for (int i = 0; i < 2048; i++) { CbProv& e = g_prov[(h + i) & 2047]; if (e.buf == b) return &e; if (!e.buf) { if (!add) return nullptr; e.buf = b; return &e; } }
    return nullptr;
}
static void ProvWrite(ID3D11Resource* r) {
    if (!ProvOn() || !r) return; D3D11_RESOURCE_DIMENSION dim; r->GetType(&dim); if (dim != D3D11_RESOURCE_DIMENSION_BUFFER) return;
    CbProv* e = ProvGet(r, true); if (!e) return; Stack4(e->w); e->wPass = g_dumpPass; e->wSeq = g_dseq; e->wCount++;
}
// ---------------------------------------------------------------------------------------------------------------
// ---------------------------------------------------------------------------------------------------------------
// 112: RainFix - the vehicle mist (and other effects using the rain block) missing in the second eye.
// Found with per-draw layer pictures (build 111): one instanced draw adds the mist in the first eye pass and nothing in the
// second. Its vertex shader reads cbRainSettings (320 bytes = 80 floats). Floats 0-31 (two matrices) are the same in both
// passes; floats 32-79 are all zero in the first eye pass and hold other matrices in the second, and with those the
// particles are not drawn. Fix, per draw: first eye pass -> remember floats 32-79 of the bound block; later passes -> if
// they differ, bind a 320-byte buffer of the mod's own with the game's first 32 floats and the remembered rest, and put
// the game's buffer back after the draw. The block contents come from write tracking (no GPU read-back).
volatile int g_rainFix = 1; volatile LONG g_rnFixed, g_rnSeen;
// 113: what actually differs for the mist draw (the block's tail is stray memory in BOTH passes - build 112 disproved it):
// its vertex shader samples a texture, texHeightmap (512x512 depth map, VS slot 0). In the first eye pass it is bound; in
// the second eye pass NOTHING is bound in that slot, so every particle gets height 0 and ends up out of sight.
// HeightmapFix: at an instanced draw whose vertex shader declares texHeightmap - bound -> remember that view; not bound ->
// bind the remembered one. Toggled together with g_rainFix (Alt+F5); the block patch of 112 is no longer applied.
volatile LONG g_hmFixed, g_hmSeen; static ID3D11ShaderResourceView* g_hmSrv;
static void HeightmapFix(ID3D11DeviceContext* c) {
    static void* lastVs; static int lastSlot = -1;
    if (g_vs != lastVs) { lastVs = g_vs; lastSlot = g_vs ? ShaderFindRes(g_vs, "texHeightmap") : -1; }
    if (lastSlot < 0 || lastSlot > 15) return;
    InterlockedIncrement(&g_hmSeen);
    ID3D11ShaderResourceView* v = nullptr; c->VSGetShaderResources((UINT)lastSlot, 1, &v);
    if (v) { if (v != g_hmSrv) { if (g_hmSrv) g_hmSrv->Release(); g_hmSrv = v; LOG_FIRST(3, "heightmap: view %p remembered (vertex shader %p, slot %d)", (void*)v, g_vs, lastSlot); } else v->Release(); return; }
    if (!g_hmSrv) return;
    c->VSSetShaderResources((UINT)lastSlot, 1, &g_hmSrv);
    LOG_FIRST(3, "heightmap: nothing bound in VS slot %d for vertex shader %p (eye pass %d) -> remembered view %p bound", lastSlot, g_vs, (int)g_sfEye, (void*)g_hmSrv);
    InterlockedIncrement(&g_hmFixed);
}
int ShaderFindRes(void* sh, const char* name);
typedef HRESULT (STDMETHODCALLTYPE *MapFn_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT, D3D11_MAP, UINT, D3D11_MAPPED_SUBRESOURCE*);
static HRESULT STDMETHODCALLTYPE hkMap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, D3D11_MAP t, UINT f, D3D11_MAPPED_SUBRESOURCE* m) {
    int k = -1; if (t != D3D11_MAP_READ) { k = WrAdd(r); ProvWrite(r); }
    HRESULT hr = oMap(c, r, s, t, f, m);
    if (k >= 0 && SUCCEEDED(hr) && m && m->pData) for (auto& e : g_mapPend) if (!e.r || e.r == r) { e.r = r; e.p = m->pData; e.k = k; break; }
    return hr;
}
typedef void (STDMETHODCALLTYPE *Unmap_t)(ID3D11DeviceContext*, ID3D11Resource*, UINT); static Unmap_t oUnmap;
static void STDMETHODCALLTYPE hkUnmap(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s) {
    for (auto& e : g_mapPend) if (e.r == r) { if (g_wrSet[e.k] == r) { memcpy(g_wrData[e.k], e.p, g_wrSize[e.k]); } e.r = nullptr; break; }
    oUnmap(c, r, s);
}
static void STDMETHODCALLTYPE hkUSR(ID3D11DeviceContext* c, ID3D11Resource* r, UINT s, const D3D11_BOX* b, const void* d, UINT rp, UINT dp) { int k = WrAdd(r); if (k >= 0 && d && !b) { memcpy(g_wrData[k], d, g_wrSize[k]); } ProvWrite(r); oUSR(c, r, s, b, d, rp, dp); }
static void STDMETHODCALLTYPE hkPSCB(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* b) {
    if (ProvOn()) for (UINT i = 0; i < n; i++) if (st + i < 4) { g_psSet[st + i].buf = b ? b[i] : nullptr; g_psSet[st + i].seq = g_dseq; }
    oPSCB(c, st, n, b);
}
static void STDMETHODCALLTYPE hkVSCB(ID3D11DeviceContext* c, UINT st, UINT n, ID3D11Buffer* const* b) {
    if (ProvOn()) for (UINT i = 0; i < n; i++) if (st + i < 4) { g_vsSet[st + i].buf = b ? b[i] : nullptr; Stack4(g_vsSet[st + i].s); }
    oVSCB(c, st, n, b);
}
static void ProvLog(ID3D11DeviceContext* c) {
    for (int k = 0; k < 8; k++) {
        ID3D11Buffer* b = nullptr; if (k < 4) c->PSGetConstantBuffers(k, 1, &b); else c->VSGetConstantBuffers(k - 4, 1, &b);
        auto& set = k < 4 ? g_psSet[k] : g_vsSet[k - 4];
        CbProv* e = b ? ProvGet(b, false) : nullptr; UINT bw = 0; if (b) { D3D11_BUFFER_DESC bd; b->GetDesc(&bd); bw = bd.ByteWidth; }
        Log("prov:   pass %d seq %ld %s cb%d buffer %p (%u bytes, usage dyn?) bound by +%llx +%llx +%llx +%llx%s | written %d times this frame, last pass %d seq %ld by +%llx +%llx +%llx +%llx",
            g_dumpPass, g_dseq, k < 4 ? "PS" : "VS", k & 3, b, bw,
            (unsigned long long)set.s[0], (unsigned long long)set.s[1], (unsigned long long)set.s[2], (unsigned long long)set.s[3], set.buf == b ? "" : " (bound before this frame's recording)",
            e ? e->wCount : 0, e ? e->wPass : -1, e ? e->wSeq : -1L,
            (unsigned long long)(e ? e->w[0] : 0), (unsigned long long)(e ? e->w[1] : 0), (unsigned long long)(e ? e->w[2] : 0), (unsigned long long)(e ? e->w[3] : 0));
        if (b) b->Release();
    }
}
static void ProvReset() { memset(g_prov, 0, sizeof g_prov); memset(g_psSet, 0, sizeof g_psSet); memset(g_vsSet, 0, sizeof g_vsSet); }
void RtProvInstall(ID3D11DeviceContext* ctx) {
    static bool done = false; if (done || !ctx) return; done = true;
    void** vt = *(void***)ctx; int ok = 0;
    ok += MH_CreateHook(vt[14], (void*)hkMap, (void**)&oMap) == MH_OK && MH_EnableHook(vt[14]) == MH_OK;
    ok += MH_CreateHook(vt[48], (void*)hkUSR, (void**)&oUSR) == MH_OK && MH_EnableHook(vt[48]) == MH_OK;
    ok += MH_CreateHook(vt[15], (void*)hkUnmap, (void**)&oUnmap) == MH_OK && MH_EnableHook(vt[15]) == MH_OK;
    ok += MH_CreateHook(vt[10], (void*)hkPSSamp, (void**)&oPSSamp) == MH_OK && MH_EnableHook(vt[10]) == MH_OK;   // 084
    ok += MH_CreateHook(vt[16], (void*)hkPSCB, (void**)&oPSCB) == MH_OK && MH_EnableHook(vt[16]) == MH_OK;
    ok += MH_CreateHook(vt[7], (void*)hkVSCB, (void**)&oVSCB) == MH_OK && MH_EnableHook(vt[7]) == MH_OK;
    Log("prov: %d of 6 context hooks installed", ok);
}
static void ProbeAfter(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) {
    if (g_mode != 1 || g_dump != 2 || GetCurrentThreadId() != g_dumpTid || g_dumpPass > 1 || !IsEyeGbuf()) return;
    uint32_t v[8]; memcpy(v, g_probeV, sizeof v); ProbeRead(c, v);
    if (!memcmp(v, g_probeV, 4 * g_probeN)) return;
    ID3D11Buffer* ib = nullptr; DXGI_FORMAT ifm; UINT io = 0; c->IAGetIndexBuffer(&ib, &ifm, &io); if (ib) ib->Release();
    ID3D11Buffer* vb = nullptr; UINT st = 0, of = 0; c->IAGetVertexBuffers(0, 1, &vb, &st, &of); if (vb) vb->Release();
    char ch[200]; int o = 0; for (int i = 0; i < g_probeN; i++) if (v[i] != g_probeV[i]) o += snprintf(ch + o, sizeof ch - o, " p%d %08x>%08x", i, g_probeV[i], v[i]);
    Log("probe: pass %d G-buffer draw %ld (count %u start %u base %d) ps %p vs %p (emission tex %d) changed:%s", g_dumpPass, g_drawsBound, a, b, (int)d, g_ps, g_vs, g_ps ? ShaderFindRes(g_ps, "texEmission") : -2, ch);
    if (g_dumpPass == 1 && g_hitN < 16) { bool dup = false; for (int i = 0; i < g_hitN; i++) if (g_hit[i].ib == ib && g_hit[i].vb == vb && g_hit[i].a == a && g_hit[i].b == b && g_hit[i].d == d && g_hit[i].ps == g_ps) dup = true;
        if (!dup) g_hit[g_hitN++] = { ib, vb, a, b, d, e, g_ps, g_vs }; }
    memcpy(g_probeV, v, sizeof v);
}
static void GbStateReport() {
    Log("state: pass 0 %d draws, pass 1 %d draws of the found geometry", g_gbN[0], g_gbN[1]);
    int shown = 0;
    for (int j = 0; j < g_gbN[1] && shown < 16; j++) {
        GbD& y = g_gb[1][j]; GbD* x = nullptr; int bestd = 1 << 30;
        for (int i = 0; i < g_gbN[0]; i++) { GbD& z = g_gb[0][i]; if (z.ib != y.ib || z.vb != y.vb || z.a != y.a || z.b != y.b || z.d != y.d || z.ps != y.ps) continue;
            int dif = 0; for (int k = 4; k < 8; k++) { int n = z.n[k] < y.n[k] ? z.n[k] : y.n[k]; for (int q = 0; q < n; q++) if (memcmp(&z.cb[k][q], &y.cb[k][q], 4)) dif++; dif += abs(z.n[k] - y.n[k]); }
            if (dif < bestd) { bestd = dif; x = &z; } }
        shown++;
        char t1[400], t0[400]; int o1 = 0, o0 = 0;
        for (int k = 0; k < 16; k++) { o1 += snprintf(t1 + o1, sizeof t1 - o1, " %d", y.srv[k]); if (x) o0 += snprintf(t0 + o0, sizeof t0 - o0, " %d", x->srv[k]); }
        Log("state: draw (count %u start %u base %d) ps %p vs %p - eye 0 instance %s (VS constants differing %d)", y.a, y.b, (int)y.d, y.ps, y.vs, x ? "found" : "NOT FOUND", x ? bestd : -1);
        Log("state:   PS textures eye1:%s", t1); if (x) Log("state:   PS textures eye0:%s", t0);
        o1 = o0 = 0; for (int k = 0; k < 8; k++) { o1 += snprintf(t1 + o1, sizeof t1 - o1, " %d", y.vsrv[k]); if (x) o0 += snprintf(t0 + o0, sizeof t0 - o0, " %d", x->vsrv[k]); }
        Log("state:   VS textures eye1:%s", t1); if (x) Log("state:   VS textures eye0:%s", t0);
        if (x) Log("state:   states eye0 blend %p depth %p raster %p ref %u | eye1 blend %p depth %p raster %p ref %u", x->bs, x->ds, x->rs, x->ref, y.bs, y.ds, y.rs, y.ref);
        GbD& z = x ? *x : y;
        const float* v0[4] = { z.cb[0], z.cb[1], z.cb[2], z.cb[3] }; const float* v1[4] = { y.cb[0], y.cb[1], y.cb[2], y.cb[3] };
        int n0[4] = { z.n[0], z.n[1], z.n[2], z.n[3] }, n1[4] = { y.n[0], y.n[1], y.n[2], y.n[3] };
        ShaderDescribe("state PS (pass0 = eye 0, pass1 = eye 1)", y.ps, v0, v1, n0, n1, 4, false);
        const float* w0[4] = { z.cb[4], z.cb[5], z.cb[6], z.cb[7] }; const float* w1[4] = { y.cb[4], y.cb[5], y.cb[6], y.cb[7] };
        int m0[4] = { z.n[4], z.n[5], z.n[6], z.n[7] }, m1[4] = { y.n[4], y.n[5], y.n[6], y.n[7] };
        ShaderDescribe("state VS (pass0 = eye 0, pass1 = eye 1)", y.vs, w0, w1, m0, m1, 4, false);
    }
    for (int p = 0; p < 2; p++) { for (int i = 0; i < g_gbN[p]; i++) for (int k = 0; k < 8; k++) free(g_gb[p][i].cb[k]); g_gbN[p] = 0; }
}
// build 120-122: things the flat game puts over the finished picture that do not belong in a headset. A pixel shader is
// named by the hash of its bytecode (ShaderHash), judged once and then remembered by pointer.
//  1 water drops "on the lens" (boats, rain): draws after the final composite, shader ba368a837e5c06cc -> left out
//  2 the low-health red edge vignette: a UI (Scaleform) draw with the plain textured shader f5a14d889ede8a42 and a
//    128x128 BC3 texture; the UI shares that shader, so the texture decides (red, judged once per texture) -> left out
//  3 depth of field / speed blur: the combine pass 588261c8a73938c3 mixes the sharp picture (t0 texDiffuse) with a
//    blurred half-size copy (t1 texDiffuse2) -> the sharp picture is bound in both slots, so the mix changes nothing
volatile int g_hideDrops = 1, g_hideHurt = 1, g_hideBlur = 1;
//  4 bloom: the final composite bc57b9e0e6db743c adds the glow picture (t1 texHDRBloom) to the scene (t0 texDiffuse) -> a
//    black 1x1 texture is bound in its place (126; off by default = the game's look)
volatile int g_hideBloom = 0;
static ID3D11ShaderResourceView* BlackSrv(ID3D11DeviceContext* c) {
    static ID3D11ShaderResourceView* v = nullptr; static bool tried = false; if (tried) return v; tried = true;
    ID3D11Device* dev = nullptr; c->GetDevice(&dev); if (!dev) return nullptr;
    D3D11_TEXTURE2D_DESC d = {}; d.Width = d.Height = 1; d.MipLevels = 1; d.ArraySize = 1; d.Format = DXGI_FORMAT_R8G8B8A8_UNORM; d.SampleDesc.Count = 1; d.Usage = D3D11_USAGE_IMMUTABLE; d.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const uint32_t px = 0; D3D11_SUBRESOURCE_DATA sd = { &px, 4, 4 }; ID3D11Texture2D* t = nullptr;
    if (SUCCEEDED(dev->CreateTexture2D(&d, &sd, &t)) && t) { dev->CreateShaderResourceView(t, nullptr, &v); t->Release(); }
    dev->Release(); Log("overlay: black texture for the bloom input %s", v ? "ready" : "FAILED");
    return v;
}
static struct OvE { void* k; int v; } g_ovl[4096], g_ovlTex[256];
// pointer -> verdict, open addressing (123: the first table let two shaders share a slot and judged them again on every draw)
static OvE* OvFind(OvE* tab, unsigned mask, void* k) {
    unsigned h = (unsigned)(((uintptr_t)k >> 4) * 2654435761u);
    for (unsigned i = 0; i < 16; i++) { OvE* e = &tab[(h + i) & mask]; if (e->k == k || !e->k) return e; }
    OvE* e = &tab[h & mask]; e->k = nullptr; return e;      // full neighbourhood: reuse the home slot
}
static int UiTexRed(ID3D11DeviceContext* c, ID3D11Texture2D* t, const D3D11_TEXTURE2D_DESC& d) {
    OvE& e = *OvFind(g_ovlTex, 255, t); if (e.k == t) return e.v;
    int red = 0; ID3D11Device* dev = nullptr; c->GetDevice(&dev); ID3D11Texture2D* st = nullptr;
    D3D11_TEXTURE2D_DESC sd = d; sd.Usage = D3D11_USAGE_STAGING; sd.BindFlags = 0; sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ; sd.MiscFlags = 0;
    if (dev && SUCCEEDED(dev->CreateTexture2D(&sd, nullptr, &st)) && st) {
        c->CopyResource(st, t); D3D11_MAPPED_SUBRESOURCE m;
        if (SUCCEEDED(c->Map(st, 0, D3D11_MAP_READ, 0, &m))) {
            double R = 0, G = 0, B = 0; int n = 0; uint64_t h = 1469598103934665603ull;
            for (UINT y = 0; y < d.Height / 4; y++) { const uint8_t* p = (const uint8_t*)m.pData + (size_t)y * m.RowPitch;
                for (UINT x = 0; x < d.Width / 4; x++, p += 16) {          // BC3 block: alpha ends 0-1, colour ends (565) 8-11
                    for (int k = 0; k < 16; k++) { h ^= p[k]; h *= 1099511628211ull; }
                    if (p[0] < 8 && p[1] < 8) continue;                    // fully transparent
                    for (int q = 0; q < 2; q++) { unsigned v = p[8 + q * 2] | (p[9 + q * 2] << 8); R += ((v >> 11) & 31) * 8.2; G += ((v >> 5) & 63) * 4.05; B += (v & 31) * 8.2; n++; } } }
            c->Unmap(st, 0);
            if (n) { R /= n; G /= n; B /= n; }
            red = h == 0xe8255263b4204b2dull || (n > 64 && R > 50 && R > 3 * G && R > 3 * B);   // 123: the vignette texture is black with an alpha gradient, tinted red by the UI - known by its content
            Log("overlay: UI texture %p 128x128 hash %016llx: %d opaque block ends, mean colour %.0f %.0f %.0f -> %s", t, (unsigned long long)h, n, R, G, B, red ? "hurt vignette, left out" : "kept");
        }
        st->Release();
    }
    if (dev) dev->Release();
    e.k = t; e.v = red; return red;
}
static bool OverlaySkip(ID3D11DeviceContext* c, UINT count) {
    void* ps = g_ps; if (!ps) return false;
    OvE& e = *OvFind(g_ovl, 4095, ps);
    if (e.k != ps) {
        uint64_t h = ShaderHash(ps); int v = h == 0xba368a837e5c06ccull ? 1 : h == 0xf5a14d889ede8a42ull ? 2 : h == 0x588261c8a73938c3ull ? 3 : h == 0xbc57b9e0e6db743cull ? 4 : 0;
        if (v) Log("overlay: shader %p hash %016llx = %s", ps, (unsigned long long)h, v == 1 ? "screen water drops" : v == 2 ? "UI textured (hurt vignette check)" : v == 3 ? "depth of field / blur combine" : "final composite (bloom input)");
        e.k = ps; e.v = v;
    }
    if (!e.v) return false;
    if (e.v == 1) return g_hideDrops != 0;
    if (e.v == 2) {
        if (!g_hideHurt || count < 24) return false;
        ID3D11ShaderResourceView* sr = nullptr; c->PSGetShaderResources(0, 1, &sr); if (!sr) return false;
        bool skip = false; ID3D11Resource* r = nullptr; sr->GetResource(&r); sr->Release(); ID3D11Texture2D* t = nullptr; if (r) { r->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&t); r->Release(); }
        if (t) { D3D11_TEXTURE2D_DESC d; t->GetDesc(&d); if (d.Width == 128 && d.Height == 128 && d.Format >= 76 && d.Format <= 78) skip = UiTexRed(c, t, d) != 0; t->Release(); }
        return skip;
    }
    if (e.v == 4 && g_hideBloom) { ID3D11ShaderResourceView* b = BlackSrv(c); if (b) c->PSSetShaderResources(1, 1, &b); return false; }
    if (e.v == 3 && g_hideBlur) { ID3D11ShaderResourceView* sr = nullptr; c->PSGetShaderResources(0, 1, &sr); if (sr) { c->PSSetShaderResources(1, 1, &sr); sr->Release(); } }
    return false;
}
static void STDMETHODCALLTYPE hkDI(ID3D11DeviceContext* c, UINT a, UINT b, INT d) { if (OverlaySkip(c, a)) return; if (g_skipPs && g_ps == g_skipPs && g_ol != 2) return; if (g_ol == 2) { g_olDraws++; OlDraw(); CbDump(c); } DumpDraw(); GbDraw(c, a, b, (UINT)d, 0); { int sfs = 0; g_sfCount = a; ID3D11Buffer* sfo = SceneryFixBegin(c, &sfs); oDI(c, a, b, d); SceneryFixEnd(c, sfo, sfs); } DumpAfter(c, a); ProbeAfter(c, a, b, (UINT)d, 0); }
static void STDMETHODCALLTYPE hkD(ID3D11DeviceContext* c, UINT a, UINT b) { if (OverlaySkip(c, a)) return; if (g_skipPs && g_ps == g_skipPs && g_ol != 2) return; if (g_ol == 2) { g_olDraws++; OlDraw(); CbDump(c); } DumpDraw(); GbDraw(c, a, b, 0, 0); { int sfs = 0; g_sfCount = a; ID3D11Buffer* sfo = SceneryFixBegin(c, &sfs); oD(c, a, b); SceneryFixEnd(c, sfo, sfs); } DumpAfter(c, a); ProbeAfter(c, a, b, 0, 0); }
static void STDMETHODCALLTYPE hkDII(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, INT e, UINT f) { if (OverlaySkip(c, a * b)) return; g_startInst = f; if (g_skipPs && g_ps == g_skipPs && g_ol != 2) return; if (g_ol == 2) { g_olDraws++; OlDraw(); CbDump(c); } DumpDraw(); GbDraw(c, a * 100000 + b, d, (UINT)e, f); { int sfs = 0; g_sfCount = a; ID3D11Buffer* sfo = SceneryFixBegin(c, &sfs); if (g_rainFix) HeightmapFix(c); oDII(c, a, b, d, e, f); SceneryFixEnd(c, sfo, sfs); } DumpAfter(c, a * 100000 + b); ProbeAfter(c, a * 100000 + b, d, (UINT)e, f); }
static void STDMETHODCALLTYPE hkDIn(ID3D11DeviceContext* c, UINT a, UINT b, UINT d, UINT e) { g_startInst = e; if (g_skipPs && g_ps == g_skipPs && g_ol != 2) return; if (g_ol == 2) { g_olDraws++; OlDraw(); CbDump(c); } DumpDraw(); GbDraw(c, a * 100000 + b, d, e, 0); { int sfs = 0; g_sfCount = a; ID3D11Buffer* sfo = SceneryFixBegin(c, &sfs); oDIn(c, a, b, d, e); SceneryFixEnd(c, sfo, sfs); } DumpAfter(c, a * 100000 + b); ProbeAfter(c, a * 100000 + b, d, e, 0); }
static void STDMETHODCALLTYPE hkCSR(ID3D11DeviceContext* c, ID3D11Resource* dst, UINT ds, UINT x, UINT y, UINT z, ID3D11Resource* src, UINT ss, const D3D11_BOX* box) {
    if (g_ol == 2) if (OlE* e = OlAdd('C')) { e->r0 = dst; e->r1 = src; ResId(dst); ResId(src); e->a = x; e->b = y; if (box) { e->c = box->right - box->left; e->d = box->bottom - box->top; } else e->c = e->d = -1; }
    oCSR(c, dst, ds, x, y, z, src, ss, box);
}
static void STDMETHODCALLTYPE hkCR(ID3D11DeviceContext* c, ID3D11Resource* dst, ID3D11Resource* src) {
    if (g_ol == 2) if (OlE* e = OlAdd('R')) { e->r0 = dst; e->r1 = src; ResId(dst); ResId(src); }
    oCR(c, dst, src);
}
static void STDMETHODCALLTYPE hkCL(ID3D11DeviceContext* c, ID3D11RenderTargetView* v, const FLOAT col[4]) {
    if (g_ol == 2 && v) if (OlE* e = OlAdd('L')) { ID3D11Resource* r = nullptr; v->GetResource(&r); e->r0 = r; ResId(r); if (r) r->Release(); e->a = (int)(col[3] * 100); }
    oCL(c, v, col);
}
static void OlRt(UINT n, ID3D11RenderTargetView* const* rtv, ID3D11DepthStencilView* dsv) {
    if (g_ol != 2) return;
    if (OlE* e = OlAdd('T')) {
        ID3D11Resource* r = nullptr; if (n && rtv && rtv[0]) { rtv[0]->GetResource(&r); if (r) r->Release(); }
        e->r0 = r; ResId(r); e->a = n;
        ID3D11Resource* d = nullptr; if (dsv) { dsv->GetResource(&d); if (d) d->Release(); } e->r1 = d; ResId(d);
    }
}
void RtOutlineFrame() {   // called at present: start recording an armed frame, or report a recorded one
    if (g_ol == 1) { g_olN = 0; g_olDraws = 0; g_nRes = 0; g_nDD = 0; g_nPassAt = 0; g_olFrames = 0; g_frameB = -1; InterlockedExchange(&g_ol, 2); return; }
    if (g_ol != 2) return;
    if (g_olFrames == 0) { g_olFrames = 1; g_frameB = g_nDD; g_frameBMarks = g_nPassAt; return; }   // 050: keep recording a second frame
    InterlockedExchange(&g_ol, 0);
    if (g_nPassAt >= 4 && g_frameBMarks == 2) {
        DiffFrames("first eye pass (pass 0)", 0, g_passAt[0], g_frameB, g_passAt[2]);
        DiffFrames("second eye pass (pass 1)", g_passAt[0], g_passAt[1], g_passAt[2], g_passAt[3]);
        CbReport(); ObjReport();
    } else Log("frames: pass marks %d (frame B from mark %d) - no two-frame compare", g_nPassAt, g_frameBMarks);
    Log("outline: one frame, %d entries (draws = draws since the previous entry; T = render target, V = viewport, S = scissor, C/R = copy dst<-src, L = clear, M = mod marker; textures as #id WxH fmt)", g_olN);
    auto tx = [](void* r, char* b, int n) { int i = ResId(r); if (i < 0) snprintf(b, n, "-"); else snprintf(b, n, "#%d %dx%d f%d%s", i, g_resW[i], g_resH[i], g_resF[i], r == g_eyes[0] ? " LEFTEYE" : r == g_eyes[1] ? " RIGHTEYE" : ""); };
    for (int i = 0; i < g_olN; i++) {
        OlE& e = g_olE[i]; char a[64], b[64], s[64]; int o = 0; s[0] = 0;
        for (int k = 0; k < 4 && e.s[k]; k++) o += snprintf(s + o, sizeof s - o, " +%llx", (unsigned long long)e.s[k]);
        switch (e.k) {
        case 'T': tx(e.r0, a, 64); tx(e.r1, b, 64); Log("  %4d d%-4ld T %s (n %d) depth %s |%s", i, e.draws, a, e.a, b, s); break;
        case 'V': Log("  %4d d%-4ld V %d,%d %dx%d |%s", i, e.draws, e.a, e.b, e.c, e.d, s); break;
        case 'S': Log("  %4d d%-4ld S %d,%d %dx%d |%s", i, e.draws, e.a, e.b, e.c, e.d, s); break;
        case 'C': tx(e.r0, a, 64); tx(e.r1, b, 64); Log("  %4d d%-4ld C %s at %d,%d <- %s box %dx%d |%s", i, e.draws, a, e.a, e.b, b, e.c, e.d, s); break;
        case 'R': tx(e.r0, a, 64); tx(e.r1, b, 64); Log("  %4d d%-4ld R %s <- %s |%s", i, e.draws, a, b, s); break;
        case 'L': tx(e.r0, a, 64); Log("  %4d d%-4ld L %s alpha %d |%s", i, e.draws, a, e.a, s); break;
        case 'M': Log("  %4d d%-4ld M %s", i, e.draws, e.m); break;
        case 'D': Log("  %4d d%-4ld D draw |%s", i, e.draws, s); break;
        }
    }
    Log("outline: %ld draws after the last entry", g_olDraws);
    DiffReport();
}
void RtProvInstall(ID3D11DeviceContext* ctx);
void RtOutlineInstall(ID3D11DeviceContext* ctx) {
    static bool done = false; if (done || !ctx) return; done = true;
    void** vt = *(void***)ctx; int ok = 0;
    ok += MH_CreateHook(vt[44], (void*)hkVP, (void**)&oVP) == MH_OK; ok += MH_CreateHook(vt[45], (void*)hkSC, (void**)&oSC) == MH_OK;
    ok += MH_CreateHook(vt[12], (void*)hkDI, (void**)&oDI) == MH_OK; ok += MH_CreateHook(vt[13], (void*)hkD, (void**)&oD) == MH_OK;
    ok += MH_CreateHook(vt[20], (void*)hkDII, (void**)&oDII) == MH_OK; ok += MH_CreateHook(vt[21], (void*)hkDIn, (void**)&oDIn) == MH_OK;
    ok += MH_CreateHook(vt[46], (void*)hkCSR, (void**)&oCSR) == MH_OK; ok += MH_CreateHook(vt[47], (void*)hkCR, (void**)&oCR) == MH_OK;
    ok += MH_CreateHook(vt[50], (void*)hkCL, (void**)&oCL) == MH_OK;
    ok += MH_CreateHook(vt[8], (void*)hkPSSRV, (void**)&oPSSRV) == MH_OK; ok += MH_CreateHook(vt[9], (void*)hkPSSh, (void**)&oPSSh) == MH_OK;
    ok += MH_CreateHook(vt[11], (void*)hkVSSh, (void**)&oVSSh) == MH_OK;
    int idx[] = {44, 45, 12, 13, 20, 21, 46, 47, 50, 8, 9, 11}; for (int i : idx) MH_EnableHook(vt[i]);
    Log("outline: %d of 11 context hooks installed", ok);
    RtProvInstall(ctx);
}

// 047: Ctrl+F10 steps through the shaders whose draw counts differ between the eye passes and hides that shader's draws
int RtSkipNext(char* label, int n) {
    if (!g_nSkipC) { snprintf(label, n, "NO SHADER LIST - PRESS F10 FIRST"); return -1; }
    g_skipI = g_skipI + 1 >= g_nSkipC ? -1 : g_skipI + 1;
    g_skipPs = g_skipI < 0 ? nullptr : g_skipC[g_skipI];
    if (g_skipI < 0) snprintf(label, n, "SHADER TEST OFF"); else snprintf(label, n, "SHADER TEST %d OF %d: HIDDEN", g_skipI + 1, g_nSkipC);
    Log("shader test: %s (shader %p)", label, g_skipPs);
    return g_skipI;
}

// build 049: occlusion queries. Light glows / emissive fades are often driven by occlusion query results; with two eye
// passes per frame the first pass may read a result that is not ready yet (treated as hidden). QueryTest (Ctrl+F9):
// 0 = off, 1 = occlusion queries whose result is not ready or zero report "visible" (test).
typedef HRESULT (STDMETHODCALLTYPE *GetData_t)(ID3D11DeviceContext*, ID3D11Asynchronous*, void*, UINT, UINT);
static GetData_t oGetData; static volatile int g_qTest; static volatile LONG g_qCalls, g_qNotReady, g_qZero, g_qForced;
static HRESULT STDMETHODCALLTYPE hkGetData(ID3D11DeviceContext* c, ID3D11Asynchronous* a, void* d, UINT n, UINT f) {
    HRESULT hr = oGetData(c, a, d, n, f);
    if (!g_qTest) return hr;   // 088: the occlusion query census only runs during the query test
    ID3D11Query* q = nullptr;
    if (a && SUCCEEDED(a->QueryInterface(__uuidof(ID3D11Query), (void**)&q)) && q) {
        D3D11_QUERY_DESC qd; q->GetDesc(&qd); q->Release();
        if (qd.Query == D3D11_QUERY_OCCLUSION || qd.Query == D3D11_QUERY_OCCLUSION_PREDICATE) {
            InterlockedIncrement(&g_qCalls);
            bool zero = hr == S_OK && d && (qd.Query == D3D11_QUERY_OCCLUSION ? (n >= 8 && *(UINT64*)d == 0) : (n >= 4 && *(BOOL*)d == 0));
            if (hr == S_FALSE) InterlockedIncrement(&g_qNotReady); else if (zero) InterlockedIncrement(&g_qZero);
            if (g_qTest && d && (hr == S_FALSE || zero)) {
                if (qd.Query == D3D11_QUERY_OCCLUSION && n >= 8) *(UINT64*)d = 100000; else if (n >= 4) *(BOOL*)d = TRUE;
                InterlockedIncrement(&g_qForced); hr = S_OK;
            }
        }
    }
    return hr;
}
void RtQueryTest(int on) { g_qTest = on; }
void RtQueryStats(char* b, int n) { snprintf(b, n, "occlusion query reads %ld (not ready %ld, zero %ld, forced visible %ld)", (long)g_qCalls, (long)g_qNotReady, (long)g_qZero, (long)g_qForced); }
void RtQueryInstall(ID3D11DeviceContext* ctx) {
    static bool done = false; if (done || !ctx) return; done = true;
    void* f = (*(void***)ctx)[29];
    if (MH_CreateHook(f, (void*)hkGetData, (void**)&oGetData) == MH_OK && MH_EnableHook(f) == MH_OK) Log("query: GetData hooked"); else Log("query: GetData hook failed");
}
