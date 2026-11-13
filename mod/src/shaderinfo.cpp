// SDVR build 052 - shader reflection. The game's shader bytecode is kept per created shader (CreatePixelShader /
// CreateVertexShader on the device), so a shader seen in a capture can be described: its constant buffers with the
// names and offsets of their variables, and its textures. Used to read the lantern shader's constants per eye pass.
#include <windows.h>
#include <d3d11.h>
#include <d3d11shader.h>
#include <d3dcompiler.h>
#include <stdint.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include "MinHook.h"
#include "log.h"
// IID_ID3D11ShaderReflection (d3dcompiler_47 / _46 interface)
static const GUID kIID_Refl = { 0x8d536ca1, 0x0cca, 0x4956, { 0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84 } };

struct ShEntry { void* sh; void* code; SIZE_T len; };
static ShEntry* g_sh; static int g_nSh, g_capSh; static SRWLOCK g_shLock = SRWLOCK_INIT;
static void ShAdd(void* sh, const void* code, SIZE_T len) {
    if (!sh || !code || !len || len > (1u << 20)) return;
    void* c = malloc(len); if (!c) return; memcpy(c, code, len);
    AcquireSRWLockExclusive(&g_shLock);
    if (g_nSh == g_capSh) { int n = g_capSh ? g_capSh * 2 : 4096; ShEntry* p = (ShEntry*)realloc(g_sh, n * sizeof(ShEntry)); if (p) { g_sh = p; g_capSh = n; } }
    if (g_nSh < g_capSh) g_sh[g_nSh++] = { sh, c, len }; else free(c);
    ReleaseSRWLockExclusive(&g_shLock);
}
static bool ShFind(void* sh, const void** code, SIZE_T* len) {
    bool ok = false;
    AcquireSRWLockShared(&g_shLock);
    for (int i = g_nSh - 1; i >= 0; i--) if (g_sh[i].sh == sh) { *code = g_sh[i].code; *len = g_sh[i].len; ok = true; break; }
    ReleaseSRWLockShared(&g_shLock);
    return ok;
}
typedef HRESULT (STDMETHODCALLTYPE *CPS_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11PixelShader**);
typedef HRESULT (STDMETHODCALLTYPE *CVS_t)(ID3D11Device*, const void*, SIZE_T, ID3D11ClassLinkage*, ID3D11VertexShader**);
static CPS_t oCPS; static CVS_t oCVS;
static HRESULT STDMETHODCALLTYPE hkCPS(ID3D11Device* d, const void* c, SIZE_T n, ID3D11ClassLinkage* l, ID3D11PixelShader** o) {
    HRESULT hr = oCPS(d, c, n, l, o); if (SUCCEEDED(hr) && o && *o) ShAdd(*o, c, n); return hr;
}
static HRESULT STDMETHODCALLTYPE hkCVS(ID3D11Device* d, const void* c, SIZE_T n, ID3D11ClassLinkage* l, ID3D11VertexShader** o) {
    HRESULT hr = oCVS(d, c, n, l, o); if (SUCCEEDED(hr) && o && *o) ShAdd(*o, c, n); return hr;
}
static void HookDevice(ID3D11Device* dev) {
    static bool done = false; if (done || !dev) return; done = true;
    void** vt = *(void***)dev;
    bool a = MH_CreateHook(vt[15], (void*)hkCPS, (void**)&oCPS) == MH_OK && MH_EnableHook(vt[15]) == MH_OK;
    bool b = MH_CreateHook(vt[12], (void*)hkCVS, (void**)&oCVS) == MH_OK && MH_EnableHook(vt[12]) == MH_OK;
    Log("shaders: create hooks %d %d", a, b);
}
typedef HRESULT (WINAPI *CD_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
typedef HRESULT (WINAPI *CDS_t)(IDXGIAdapter*, D3D_DRIVER_TYPE, HMODULE, UINT, const D3D_FEATURE_LEVEL*, UINT, UINT, const DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**, ID3D11Device**, D3D_FEATURE_LEVEL*, ID3D11DeviceContext**);
static CD_t oCD; static CDS_t oCDS;
static HRESULT WINAPI hkCD(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE m, UINT f, const D3D_FEATURE_LEVEL* fl, UINT n, UINT v, ID3D11Device** d, D3D_FEATURE_LEVEL* o, ID3D11DeviceContext** c) {
    HRESULT hr = oCD(a, t, m, f, fl, n, v, d, o, c); if (SUCCEEDED(hr) && d && *d) HookDevice(*d); return hr;
}
static HRESULT WINAPI hkCDS(IDXGIAdapter* a, D3D_DRIVER_TYPE t, HMODULE m, UINT f, const D3D_FEATURE_LEVEL* fl, UINT n, UINT v, const DXGI_SWAP_CHAIN_DESC* sd, IDXGISwapChain** sc, ID3D11Device** d, D3D_FEATURE_LEVEL* o, ID3D11DeviceContext** c) {
    HRESULT hr = oCDS(a, t, m, f, fl, n, v, sd, sc, d, o, c); if (SUCCEEDED(hr) && d && *d) HookDevice(*d); return hr;
}
void ShaderInfoInstall() {
    HMODULE h = LoadLibraryW(L"d3d11.dll"); if (!h) return;
    void* f1 = (void*)GetProcAddress(h, "D3D11CreateDevice"); void* f2 = (void*)GetProcAddress(h, "D3D11CreateDeviceAndSwapChain");
    int ok = 0;
    if (f1 && MH_CreateHook(f1, (void*)hkCD, (void**)&oCD) == MH_OK && MH_EnableHook(f1) == MH_OK) ok++;
    if (f2 && MH_CreateHook(f2, (void*)hkCDS, (void**)&oCDS) == MH_OK && MH_EnableHook(f2) == MH_OK) ok++;
    Log("shaders: device creation hooks %d of 2", ok);
}
int ShaderCount() { return g_nSh; }
// 120: a stable name for a game shader = FNV-1a of its bytecode (0 = not recorded)
uint64_t ShaderHash(void* sh) {
    const void* code; SIZE_T len; if (!sh || !ShFind(sh, &code, &len)) return 0;
    uint64_t h = 1469598103934665603ull; const uint8_t* p = (const uint8_t*)code; for (SIZE_T i = 0; i < len; i++) { h ^= p[i]; h *= 1099511628211ull; }
    return h ? h : 1;
}

typedef HRESULT (WINAPI *Reflect_t)(LPCVOID, SIZE_T, REFIID, void**);
static Reflect_t Reflector() {
    static Reflect_t f; static bool tried;
    if (!tried) { tried = true; HMODULE h = LoadLibraryW(L"d3dcompiler_47.dll"); if (!h) h = LoadLibraryW(L"d3dcompiler_46.dll"); if (h) f = (Reflect_t)GetProcAddress(h, "D3DReflect"); }
    return f;
}
// Describe a shader; vals[slot] = captured buffers for pass 0 and pass 1 (floats), n[slot] floats in each
void ShaderDescribe(const char* tag, void* sh, const float* const* v0, const float* const* v1, const int* n0, const int* n1, int slots, bool diffOnly) {
    const void* code; SIZE_T len;
    if (!ShFind(sh, &code, &len)) { Log("refl: %s %p - bytecode not recorded (created before the hook?)", tag, sh); return; }
    Reflect_t R = Reflector(); if (!R) { Log("refl: D3DReflect not available"); return; }
    ID3D11ShaderReflection* r = nullptr;
    if (FAILED(R(code, len, kIID_Refl, (void**)&r)) || !r) { Log("refl: %s %p - reflection failed", tag, sh); return; }
    D3D11_SHADER_DESC sd; r->GetDesc(&sd);
    if (!diffOnly) Log("refl: %s %p - %u bytes, %u constant buffers, %u bound resources, %u instructions", tag, sh, (unsigned)len, sd.ConstantBuffers, sd.BoundResources, sd.InstructionCount);
    else Log("refl: %s %p (only the values that differ between the passes)", tag, sh);
    for (UINT i = 0; i < sd.BoundResources; i++) {
        D3D11_SHADER_INPUT_BIND_DESC b; r->GetResourceBindingDesc(i, &b);
        if (!diffOnly) Log("refl:   bind %s type %d slot %u", b.Name, (int)b.Type, b.BindPoint);
        if (b.Type != D3D_SIT_CBUFFER) continue;
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName(b.Name); D3D11_SHADER_BUFFER_DESC cd; if (FAILED(cb->GetDesc(&cd))) continue;
        int s = (int)b.BindPoint;
        for (UINT k = 0; k < cd.Variables; k++) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(k); D3D11_SHADER_VARIABLE_DESC vd; var->GetDesc(&vd);
            char a[300], c[300]; int oa = 0, oc = 0; bool diff = false;
            int o = vd.StartOffset / 4, cnt = vd.Size / 4; if (cnt > 16) cnt = 16;
            for (int j = 0; j < cnt; j++) {
                float x = (s < slots && v0[s] && o + j < n0[s]) ? v0[s][o + j] : NAN, y = (s < slots && v1[s] && o + j < n1[s]) ? v1[s][o + j] : NAN;
                if (memcmp(&x, &y, 4)) diff = true;
                oa += snprintf(a + oa, sizeof a - oa, " %.4g", x); oc += snprintf(c + oc, sizeof c - oc, " %.4g", y);
            }
            if (diffOnly && !diff) continue;
            Log("refl:     cb%d %s +%u (%u bytes)%s pass0:%s%s%s", s, vd.Name, vd.StartOffset, vd.Size, (vd.uFlags & D3D_SVF_USED) ? "" : " [unused]", a, diff ? " | pass1:" : "", diff ? c : "");
        }
    }
    r->Release();
}

// build 059: names of the used variables that differ between two captures of a shader's constant buffers
// (comma list into out), and the bind slot of a named resource (cached per shader)
int ShaderDiffVars(void* sh, const float* const* v0, const float* const* v1, const int* n0, const int* n1, int slots, char* out, int cap) {
    out[0] = 0; const void* code; SIZE_T len; if (!ShFind(sh, &code, &len)) return -1;
    Reflect_t R = Reflector(); if (!R) return -1; ID3D11ShaderReflection* r = nullptr;
    if (FAILED(R(code, len, kIID_Refl, (void**)&r)) || !r) return -1;
    D3D11_SHADER_DESC sd; r->GetDesc(&sd); int o = 0, nd = 0;
    for (UINT i = 0; i < sd.BoundResources; i++) {
        D3D11_SHADER_INPUT_BIND_DESC b; r->GetResourceBindingDesc(i, &b); if (b.Type != D3D_SIT_CBUFFER) continue;
        int sl = (int)b.BindPoint; if (sl >= slots) continue;
        ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName(b.Name); D3D11_SHADER_BUFFER_DESC cd; if (FAILED(cb->GetDesc(&cd))) continue;
        for (UINT k = 0; k < cd.Variables; k++) {
            ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(k); D3D11_SHADER_VARIABLE_DESC vd; var->GetDesc(&vd);
            if (!(vd.uFlags & D3D_SVF_USED)) continue;
            int a = vd.StartOffset / 4, cnt = vd.Size / 4; bool diff = false;
            for (int j = 0; j < cnt; j++) {
                bool h0 = v0[sl] && a + j < n0[sl], h1 = v1[sl] && a + j < n1[sl];
                if (h0 != h1 || (h0 && memcmp(&v0[sl][a + j], &v1[sl][a + j], 4))) { diff = true; break; }
            }
            if (diff) { nd++; if (o < cap - 64) o += snprintf(out + o, cap - o, "%s%s", o ? "," : "", vd.Name); }
        }
    }
    r->Release(); return nd;
}
// build 121: the names a shader declares (textures with slot, constant buffers with their used variables) on one line
void ShaderNames(void* sh, char* out, int cap) {
    out[0] = 0; const void* code; SIZE_T len; if (!ShFind(sh, &code, &len)) { snprintf(out, cap, "(bytecode not recorded)"); return; }
    Reflect_t R = Reflector(); ID3D11ShaderReflection* r = nullptr; if (!R || FAILED(R(code, len, kIID_Refl, (void**)&r)) || !r) return;
    D3D11_SHADER_DESC sd; r->GetDesc(&sd); int o = 0;
    for (UINT i = 0; i < sd.BoundResources && o < cap - 80; i++) { D3D11_SHADER_INPUT_BIND_DESC b; r->GetResourceBindingDesc(i, &b);
        if (b.Type == D3D_SIT_TEXTURE) o += snprintf(out + o, cap - o, " t%u=%s", b.BindPoint, b.Name);
        else if (b.Type == D3D_SIT_CBUFFER) { o += snprintf(out + o, cap - o, " cb%u=%s{", b.BindPoint, b.Name);
            ID3D11ShaderReflectionConstantBuffer* cb = r->GetConstantBufferByName(b.Name); D3D11_SHADER_BUFFER_DESC cd;
            if (SUCCEEDED(cb->GetDesc(&cd))) for (UINT k = 0; k < cd.Variables && o < cap - 80; k++) { ID3D11ShaderReflectionVariable* var = cb->GetVariableByIndex(k); D3D11_SHADER_VARIABLE_DESC vd; var->GetDesc(&vd);
                if (vd.uFlags & D3D_SVF_USED) o += snprintf(out + o, cap - o, "%s@%u ", vd.Name, vd.StartOffset / 4); }
            o += snprintf(out + o, cap - o, "}"); } }
    r->Release();
}
struct ResCache { void* sh; int slot; };
static ResCache g_rc[1024];
int ShaderFindRes(void* sh, const char* name) {
    unsigned nh = 0; for (const char* q = name; *q; q++) nh = nh * 31 + (unsigned char)*q;
    sh = (void*)((uintptr_t)sh ^ ((uintptr_t)nh << 40)); void* real = (void*)((uintptr_t)sh ^ ((uintptr_t)nh << 40));
    unsigned h = (unsigned)((((uintptr_t)real >> 4) + nh) * 2654435761u) & 1023;
    for (int i = 0; i < 1024; i++) { ResCache& e = g_rc[(h + i) & 1023]; if (e.sh == sh) return e.slot; if (!e.sh) break; }
    int slot = -1; const void* code; SIZE_T len;
    if (ShFind(real, &code, &len)) { Reflect_t R = Reflector(); ID3D11ShaderReflection* r = nullptr;
        if (R && SUCCEEDED(R(code, len, kIID_Refl, (void**)&r)) && r) { D3D11_SHADER_INPUT_BIND_DESC b; if (SUCCEEDED(r->GetResourceBindingDescByName(name, &b))) slot = (int)b.BindPoint; r->Release(); } }
    for (int i = 0; i < 1024; i++) { ResCache& e = g_rc[(h + i) & 1023]; if (!e.sh) { e.sh = sh; e.slot = slot; break; } }
    return slot;
}

// build 076: bit mask of the constant buffer slots (< 16) a shader declares, leaving out the one named `exclude`
int ShaderCbMask(void* sh, const char* exclude) {
    const void* code; SIZE_T len; if (!ShFind(sh, &code, &len)) return 0;
    Reflect_t R = Reflector(); ID3D11ShaderReflection* r = nullptr; if (!R || FAILED(R(code, len, kIID_Refl, (void**)&r)) || !r) return 0;
    D3D11_SHADER_DESC sd; r->GetDesc(&sd); int mask = 0;
    for (UINT i = 0; i < sd.BoundResources; i++) { D3D11_SHADER_INPUT_BIND_DESC b; r->GetResourceBindingDesc(i, &b);
        if (b.Type == D3D_SIT_CBUFFER && b.BindPoint < 16 && strcmp(b.Name, exclude)) mask |= 1 << b.BindPoint; }
    r->Release(); return mask;
}
