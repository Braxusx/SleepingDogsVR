// SDVR - keys held back from the game while the VR menu is open (build 091)
// The menu uses the arrow keys; in the game Up raises the phone and Down draws a weapon. While the menu is open the four
// arrow keys are hidden from the game on every route it could read them by, and given back when the menu closes:
//   - DirectInput keyboard devices (GetDeviceState / GetDeviceData)
//   - raw input (GetRawInputData)
//   - key state calls (GetAsyncKeyState / GetKeyState / GetKeyboardState) - the mod's own key thread still sees the keys
//   - window messages (PeekMessage / GetMessage: WM_KEYDOWN / WM_KEYUP)
#define DIRECTINPUT_VERSION 0x0800
#include <windows.h>
#include <stdint.h>
#include <dinput.h>
#include <MinHook.h>
#include "log.h"

static volatile LONG g_block; static DWORD g_ownTid;
static volatile LONG g_hits[5];
void InputBlockSet(bool on) { InterlockedExchange(&g_block, on ? 1 : 0); }
void InputBlockOwnThread(DWORD tid) { g_ownTid = tid; }
static inline bool VkBlocked(UINT vk) { return vk == VK_UP || vk == VK_DOWN || vk == VK_LEFT || vk == VK_RIGHT; }
static inline bool DikBlocked(DWORD k) { return k == DIK_UP || k == DIK_DOWN || k == DIK_LEFT || k == DIK_RIGHT; }
static inline bool On() { return g_block != 0 && GetCurrentThreadId() != g_ownTid; }

// --- DirectInput -----------------------------------------------------------------------------------------------
static void* g_kb[8]; static int g_nKb;
static bool IsKb(void* d) { for (int i = 0; i < g_nKb; i++) if (g_kb[i] == d) return true; return false; }
typedef HRESULT (STDMETHODCALLTYPE *GetState_t)(void*, DWORD, LPVOID);
typedef HRESULT (STDMETHODCALLTYPE *GetData_t)(void*, DWORD, LPDIDEVICEOBJECTDATA, LPDWORD, DWORD);
typedef HRESULT (STDMETHODCALLTYPE *CreateDev_t)(void*, REFGUID, void**, LPUNKNOWN);
static GetState_t oGetState[2]; static GetData_t oGetData[2]; static void* g_devVt[2]; static CreateDev_t oCreateDev[2]; static void* g_diVt[2];
template <int N> static HRESULT STDMETHODCALLTYPE hkGetState(void* dev, DWORD cb, LPVOID data) {
    HRESULT hr = oGetState[N](dev, cb, data);
    if (SUCCEEDED(hr) && data && cb == 256 && On() && IsKb(dev)) {
        uint8_t* k = (uint8_t*)data; if (k[DIK_UP] | k[DIK_DOWN] | k[DIK_LEFT] | k[DIK_RIGHT]) InterlockedIncrement(&g_hits[0]);
        k[DIK_UP] = k[DIK_DOWN] = k[DIK_LEFT] = k[DIK_RIGHT] = 0;
    }
    return hr;
}
template <int N> static HRESULT STDMETHODCALLTYPE hkGetData(void* dev, DWORD cb, LPDIDEVICEOBJECTDATA d, LPDWORD n, DWORD fl) {
    HRESULT hr = oGetData[N](dev, cb, d, n, fl);
    if (SUCCEEDED(hr) && d && n && *n && cb >= 8 && On() && IsKb(dev)) {
        DWORD w = 0; uint8_t* base = (uint8_t*)d;
        for (DWORD i = 0; i < *n; i++) {
            DIDEVICEOBJECTDATA* e = (DIDEVICEOBJECTDATA*)(base + (size_t)i * cb);
            if (DikBlocked(e->dwOfs)) { InterlockedIncrement(&g_hits[0]); continue; }
            if (w != i) memmove(base + (size_t)w * cb, e, cb);
            w++;
        }
        *n = w;
    }
    return hr;
}
template <int N> static HRESULT STDMETHODCALLTYPE hkCreateDev(void* di, REFGUID g, void** out, LPUNKNOWN u) {
    HRESULT hr = oCreateDev[N](di, g, out, u);
    if (SUCCEEDED(hr) && out && *out) {
        static const GUID kSysKeyboard = { 0x6F1D2B61, 0xD5A0, 0x11CF, { 0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00 } };
        bool kb = IsEqualGUID(g, kSysKeyboard);
        if (kb && g_nKb < 8 && !IsKb(*out)) g_kb[g_nKb++] = *out;
        void** vt = *(void***)*out;
        for (int s = 0; s < 2; s++) {
            if (g_devVt[s] == vt) break;
            if (!g_devVt[s]) {
                g_devVt[s] = vt;
                bool a = s == 0 ? (MH_CreateHook(vt[9], (void*)hkGetState<0>, (void**)&oGetState[0]) == MH_OK && MH_EnableHook(vt[9]) == MH_OK)
                                : (MH_CreateHook(vt[9], (void*)hkGetState<1>, (void**)&oGetState[1]) == MH_OK && MH_EnableHook(vt[9]) == MH_OK);
                bool b = s == 0 ? (MH_CreateHook(vt[10], (void*)hkGetData<0>, (void**)&oGetData[0]) == MH_OK && MH_EnableHook(vt[10]) == MH_OK)
                                : (MH_CreateHook(vt[10], (void*)hkGetData<1>, (void**)&oGetData[1]) == MH_OK && MH_EnableHook(vt[10]) == MH_OK);
                Log("input: DirectInput device hooks %d %d", (int)a, (int)b);
                break;
            }
        }
        Log("input: DirectInput device created (%s)", kb ? "keyboard" : "other");
    }
    return hr;
}
// called by the dinput8 proxy with the interface the game got
void InputBlockDirectInput(void* di) {
    if (!di) return;
    void** vt = *(void***)di;
    for (int s = 0; s < 2; s++) {
        if (g_diVt[s] == vt) return;
        if (!g_diVt[s]) {
            g_diVt[s] = vt;
            bool ok = s == 0 ? (MH_CreateHook(vt[3], (void*)hkCreateDev<0>, (void**)&oCreateDev[0]) == MH_OK && MH_EnableHook(vt[3]) == MH_OK)
                             : (MH_CreateHook(vt[3], (void*)hkCreateDev<1>, (void**)&oCreateDev[1]) == MH_OK && MH_EnableHook(vt[3]) == MH_OK);
            Log("input: DirectInput CreateDevice hook %d", (int)ok);
            return;
        }
    }
}

// --- user32 ----------------------------------------------------------------------------------------------------
static UINT (WINAPI *oRaw)(HRAWINPUT, UINT, LPVOID, PUINT, UINT);
static UINT WINAPI hkRaw(HRAWINPUT h, UINT cmd, LPVOID data, PUINT size, UINT hdr) {
    UINT r = oRaw(h, cmd, data, size, hdr);
    if (cmd == RID_INPUT && data && r != (UINT)-1 && r >= sizeof(RAWINPUTHEADER) + sizeof(RAWKEYBOARD) && On()) {
        RAWINPUT* ri = (RAWINPUT*)data;
        if (ri->header.dwType == RIM_TYPEKEYBOARD && VkBlocked(ri->data.keyboard.VKey)) {
            ri->data.keyboard.VKey = 0xFF; ri->data.keyboard.MakeCode = 0; ri->data.keyboard.Flags &= ~(RI_KEY_E0 | RI_KEY_E1); ri->data.keyboard.Message = WM_NULL;
            InterlockedIncrement(&g_hits[1]);
        }
    }
    return r;
}
static SHORT (WINAPI *oAsync)(int); static SHORT (WINAPI *oKeyState)(int); static BOOL (WINAPI *oKbState)(PBYTE);
static SHORT WINAPI hkAsync(int vk) { if (VkBlocked((UINT)vk) && On()) { InterlockedIncrement(&g_hits[2]); return 0; } return oAsync(vk); }
static SHORT WINAPI hkKeyState(int vk) { if (VkBlocked((UINT)vk) && On()) { InterlockedIncrement(&g_hits[2]); return 0; } return oKeyState(vk); }
static BOOL WINAPI hkKbState(PBYTE k) { BOOL r = oKbState(k); if (r && k && On()) { k[VK_UP] = k[VK_DOWN] = k[VK_LEFT] = k[VK_RIGHT] = 0; } return r; }
static void Msg(LPMSG m) {
    if (m && (m->message == WM_KEYDOWN || m->message == WM_KEYUP || m->message == WM_SYSKEYDOWN || m->message == WM_SYSKEYUP) && VkBlocked((UINT)m->wParam) && On()) { m->message = WM_NULL; InterlockedIncrement(&g_hits[3]); }
}
static BOOL (WINAPI *oPeekA)(LPMSG, HWND, UINT, UINT, UINT); static BOOL (WINAPI *oPeekW)(LPMSG, HWND, UINT, UINT, UINT);
static BOOL (WINAPI *oGetA)(LPMSG, HWND, UINT, UINT); static BOOL (WINAPI *oGetW)(LPMSG, HWND, UINT, UINT);
static BOOL WINAPI hkPeekA(LPMSG m, HWND w, UINT a, UINT b, UINT f) { BOOL r = oPeekA(m, w, a, b, f); if (r && (f & PM_REMOVE)) Msg(m); return r; }
static BOOL WINAPI hkPeekW(LPMSG m, HWND w, UINT a, UINT b, UINT f) { BOOL r = oPeekW(m, w, a, b, f); if (r && (f & PM_REMOVE)) Msg(m); return r; }
static BOOL WINAPI hkGetA(LPMSG m, HWND w, UINT a, UINT b) { BOOL r = oGetA(m, w, a, b); if (r > 0) Msg(m); return r; }
static BOOL WINAPI hkGetW(LPMSG m, HWND w, UINT a, UINT b) { BOOL r = oGetW(m, w, a, b); if (r > 0) Msg(m); return r; }

static bool Api(const char* name, void* hk, void** orig) {
    HMODULE u = GetModuleHandleW(L"user32.dll"); void* f = u ? (void*)GetProcAddress(u, name) : nullptr;
    return f && MH_CreateHook(f, hk, orig) == MH_OK && MH_EnableHook(f) == MH_OK;
}
// start-up (MinHook already initialised)
void InputBlockInstall() {
    int ok = 0;
    ok += Api("GetRawInputData", (void*)hkRaw, (void**)&oRaw);
    ok += Api("GetAsyncKeyState", (void*)hkAsync, (void**)&oAsync);
    ok += Api("GetKeyState", (void*)hkKeyState, (void**)&oKeyState);
    ok += Api("GetKeyboardState", (void*)hkKbState, (void**)&oKbState);
    ok += Api("PeekMessageA", (void*)hkPeekA, (void**)&oPeekA);
    ok += Api("PeekMessageW", (void*)hkPeekW, (void**)&oPeekW);
    ok += Api("GetMessageA", (void*)hkGetA, (void**)&oGetA);
    ok += Api("GetMessageW", (void*)hkGetW, (void**)&oGetW);
    Log("input: %d of 8 key hooks installed (arrow keys are held back from the game while the VR menu is open)", ok);
}
void InputBlockReport() {
    Log("input: menu closed - arrow key events held back: DirectInput %ld, raw input %ld, key state %ld, messages %ld", g_hits[0], g_hits[1], g_hits[2], g_hits[3]);
}
