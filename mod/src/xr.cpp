// SDVR - OpenXR output (build 005)
// No loader DLL: the active runtime comes from the registry (or XrRuntimeJson in sdvr.ini); its manifest names the
// runtime DLL and xrNegotiateLoaderRuntimeInterface gives xrGetInstanceProcAddr. The session runs on the GAME's device.
// Frame flow, all on the game's render thread (its present task):
//   Compose(eye 0/1) from the game's stereo path -> XrSubmitEye copies the eye image into that eye's swapchain
//   after the game's Present -> xrEndFrame (projection layer) -> xrWaitFrame -> xrBeginFrame (paces the game)
#define XR_NO_PROTOTYPES
#define XR_USE_PLATFORM_WIN32
#define XR_USE_GRAPHICS_API_D3D11
#include <windows.h>
#include <d3d11.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>
#include <openxr/openxr_loader_negotiation.h>
#include "log.h"
#include "xr.h"

static PFN_xrGetInstanceProcAddr gpa;
static XrInstance g_inst;
static XrSystemId g_sys;
static XrSession g_sess;
static XrSpace g_space;
static XrSessionState g_state = XR_SESSION_STATE_UNKNOWN;
static bool g_running, g_failed, g_frameBegun, g_copied[2];
static XrFrameState g_fs = {XR_TYPE_FRAME_STATE};
static volatile XrTime g_predTime, g_predPeriod;   // build 033: published after xrWaitFrame for the game thread
static volatile LONG g_locateFails;
long XrLocateFails() { return g_locateFails; }
static XrSwapchain g_swc[2];
static ID3D11Texture2D* g_img[2][8];
static uint32_t g_nimg[2], g_w, g_h;
static XrView g_views[2] = {{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
static XrPosef g_renderPose[2];
static ID3D11Device* g_dev;
static ID3D11DeviceContext* g_ctx;
static const wchar_t* g_ini;
static float g_testFovV = 55.f;   // build 005: head-locked test FOV (TrackHead=0)
// build 006: the game thread renders each eye with the views located for its frame; the render thread submits the
// views of the frame it is presenting (PoseDelay game frames behind the newest)
static XrView g_ring[8][2]; static volatile LONG g_ringN = 0; static int g_poseDelay = 1; static volatile bool g_ringOk[8];
static long long g_frame;
// build 021: HUD quad layer (Scaleform UI rendered into its own image, shown on a head-locked panel)
static XrSpace g_viewSpace;
static XrSwapchain g_hudSwc; static ID3D11Texture2D* g_hudImg[8]; static uint32_t g_hudN, g_hudW, g_hudH;
static long long g_hudFrame = -10;
static float g_hudDist = 1.4f, g_hudWidth = 1.5f, g_hudY = 0.f; static int g_hudAlpha = 1;
static volatile bool g_hudCut; static int g_menuCrop = 1; static float g_menuHudScale = 1.3f, g_menuAspect = 16.f / 9.f; static float g_cutScale = 1.25f, g_cutSubY = 0.22f;
void XrSetHudCutscene(bool on) { g_hudCut = on; }
static volatile int g_hudWorld = 1; static XrPosef g_hudAnchor = {{0, 0, 0, 1}, {0, 0, -1.4f}};
void XrSetHudLock(int world) { g_hudWorld = world; }
// 093: HUD scale - the gameplay HUD grows / shrinks about a pivot point on the panel (the minimap), so that element stays where
// it is; "hidden" drops the gameplay HUD layer (menus and cutscene subtitles are still shown).
static float g_hudScale = 1.f, g_hudPivU = 0.066f, g_hudPivV = 0.72f; static volatile bool g_hudHidden;
float XrHudScaleGet() { return g_hudScale; }
void XrHudScaleSet(float s) { if (s >= 0.25f && s <= 4.f) g_hudScale = s; }
void XrSetHudHidden(bool h) { g_hudHidden = h; }
// recentre: the world-locked panel sits HudDistance in front of the recentred head, facing it
// build 041: the panel's half-size in tangent units from the eye (horizontal, vertical) and the tangent of its centre height
void XrHudTangents(float* tx, float* ty, float* ty0) {
    float w = g_hudWidth, h = g_hudW ? w * (float)g_hudH / (float)g_hudW : w;
    *tx = 0.5f * w / g_hudDist; *ty = 0.5f * h / g_hudDist; *ty0 = g_hudY / g_hudDist;
}
static float g_ancHead[3] = { 0.f, 0.f, 0.f }, g_ancYaw = 0.f;   // 092: the recentred head point, so a distance change can move the panel
void XrSetHudAnchor(float x, float y, float z, float yaw) {
    g_ancHead[0] = x; g_ancHead[1] = y; g_ancHead[2] = z; g_ancYaw = yaw;
    XrPosef p; p.orientation = {0.f, sinf(yaw * 0.5f), 0.f, cosf(yaw * 0.5f)};
    p.position = {x - sinf(yaw) * g_hudDist, y + g_hudY, z - cosf(yaw) * g_hudDist};
    g_hudAnchor = p;
}


#define XRFN(name) static PFN_##name name
XRFN(xrCreateInstance); XRFN(xrGetSystem); XRFN(xrCreateSession); XRFN(xrCreateReferenceSpace); XRFN(xrPollEvent);
XRFN(xrBeginSession); XRFN(xrEndSession); XRFN(xrWaitFrame); XRFN(xrBeginFrame); XRFN(xrEndFrame); XRFN(xrLocateViews);
XRFN(xrEnumerateViewConfigurationViews); XRFN(xrEnumerateSwapchainFormats); XRFN(xrCreateSwapchain); XRFN(xrDestroySwapchain); XRFN(xrEnumerateSwapchainImages);
XRFN(xrAcquireSwapchainImage); XRFN(xrWaitSwapchainImage); XRFN(xrReleaseSwapchainImage); XRFN(xrGetInstanceProperties);
XRFN(xrGetSystemProperties); XRFN(xrResultToString);
static PFN_xrGetD3D11GraphicsRequirementsKHR xrGetD3D11GraphicsRequirementsKHR;

static const char* R(XrResult r) {
    static char b[64];
    if (xrResultToString && g_inst && XR_SUCCEEDED(xrResultToString(g_inst, r, b))) return b;
    snprintf(b, sizeof(b), "%d", (int)r);
    return b;
}
#define CHK(call) do { XrResult _r = (call); if (XR_FAILED(_r)) { Log("xr: %s failed: %s", #call, R(_r)); return false; } } while (0)

static bool ReadRuntimeJson(wchar_t* out, DWORD n) {
    out[0] = 0;
    if (g_ini) GetPrivateProfileStringW(L"VR", L"XrRuntimeJson", L"", out, n, g_ini);
    if (out[0]) return true;
    DWORD sz = n * sizeof(wchar_t);
    if (GetEnvironmentVariableW(L"XR_RUNTIME_JSON", out, n) && out[0]) return true;
    return RegGetValueW(HKEY_LOCAL_MACHINE, L"SOFTWARE\\Khronos\\OpenXR\\1", L"ActiveRuntime", RRF_RT_REG_SZ, nullptr, out, &sz) == ERROR_SUCCESS;
}

static bool LoadRuntime() {
    wchar_t json[MAX_PATH];
    if (!ReadRuntimeJson(json, MAX_PATH)) { Log("xr: no active OpenXR runtime registered"); return false; }
    char jsonA[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, json, -1, jsonA, sizeof(jsonA), nullptr, nullptr);
    Log("xr: runtime manifest %s", jsonA);
    FILE* f = _wfopen(json, L"rb");
    if (!f) { Log("xr: cannot open the manifest"); return false; }
    char buf[8192] = {};
    fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    char* p = strstr(buf, "\"library_path\"");
    if (!p) { Log("xr: manifest has no library_path"); return false; }
    p = strchr(p + 14, ':'); if (p) p = strchr(p, '"');
    if (!p) return false;
    char lib[MAX_PATH * 2]; int k = 0;
    for (p++; *p && *p != '"' && k < (int)sizeof(lib) - 1; p++) { if (*p == '\\' && p[1]) p++; lib[k++] = *p; }
    lib[k] = 0;
    wchar_t libW[MAX_PATH * 2], full[MAX_PATH * 2];
    MultiByteToWideChar(CP_UTF8, 0, lib, -1, libW, MAX_PATH * 2);
    bool relative = !(libW[0] && libW[1] == L':') && !(libW[0] == L'\\' && libW[1] == L'\\');
    if (relative) { wcscpy(full, json); wchar_t* s = wcsrchr(full, L'\\'); if (!s) s = wcsrchr(full, L'/'); if (s) s[1] = 0; else full[0] = 0; wcscat(full, libW); }
    else wcscpy(full, libW);
    char fullA[MAX_PATH * 2];
    WideCharToMultiByte(CP_UTF8, 0, full, -1, fullA, sizeof(fullA), nullptr, nullptr);
    HMODULE m = LoadLibraryExW(full, nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) { Log("xr: LoadLibrary(%s) failed (%lu)", fullA, GetLastError()); return false; }
    auto neg = (PFN_xrNegotiateLoaderRuntimeInterface)GetProcAddress(m, "xrNegotiateLoaderRuntimeInterface");
    if (!neg) { Log("xr: %s has no xrNegotiateLoaderRuntimeInterface", fullA); return false; }
    XrNegotiateLoaderInfo li = {XR_LOADER_INTERFACE_STRUCT_LOADER_INFO, XR_LOADER_INFO_STRUCT_VERSION, sizeof(li), 1, XR_CURRENT_LOADER_RUNTIME_VERSION,
                                XR_MAKE_VERSION(1, 0, 0), XR_MAKE_VERSION(1, 1, 0xffff)};
    XrNegotiateRuntimeRequest rr = {XR_LOADER_INTERFACE_STRUCT_RUNTIME_REQUEST, XR_RUNTIME_INFO_STRUCT_VERSION, sizeof(rr)};
    XrResult r = neg(&li, &rr);
    if (XR_FAILED(r) || !rr.getInstanceProcAddr) { Log("xr: runtime negotiation failed (%d)", (int)r); return false; }
    gpa = rr.getInstanceProcAddr;
    Log("xr: runtime %s loaded", fullA);
    return true;
}

template <class T> static bool Get(const char* name, T& fn) {
    gpa(g_inst, name, (PFN_xrVoidFunction*)&fn);
    if (!fn) Log("xr: missing %s", name);
    return fn != nullptr;
}

static bool CreateInstance() {
    Get("xrCreateInstance", xrCreateInstance);
    if (!xrCreateInstance) return false;
    const char* exts[] = {XR_KHR_D3D11_ENABLE_EXTENSION_NAME};
    XrInstanceCreateInfo ci = {XR_TYPE_INSTANCE_CREATE_INFO};
    strcpy(ci.applicationInfo.applicationName, "SDVR");
    strcpy(ci.applicationInfo.engineName, "UFG");
    ci.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
    ci.enabledExtensionCount = 1;
    ci.enabledExtensionNames = exts;
    CHK(xrCreateInstance(&ci, &g_inst));
    bool ok = Get("xrGetSystem", xrGetSystem) && Get("xrCreateSession", xrCreateSession) && Get("xrCreateReferenceSpace", xrCreateReferenceSpace) &&
              Get("xrPollEvent", xrPollEvent) && Get("xrBeginSession", xrBeginSession) && Get("xrEndSession", xrEndSession) &&
              Get("xrWaitFrame", xrWaitFrame) && Get("xrBeginFrame", xrBeginFrame) && Get("xrEndFrame", xrEndFrame) &&
              Get("xrLocateViews", xrLocateViews) && Get("xrEnumerateViewConfigurationViews", xrEnumerateViewConfigurationViews) &&
              Get("xrEnumerateSwapchainFormats", xrEnumerateSwapchainFormats) && Get("xrCreateSwapchain", xrCreateSwapchain) && Get("xrDestroySwapchain", xrDestroySwapchain) &&
              Get("xrEnumerateSwapchainImages", xrEnumerateSwapchainImages) && Get("xrAcquireSwapchainImage", xrAcquireSwapchainImage) &&
              Get("xrWaitSwapchainImage", xrWaitSwapchainImage) && Get("xrReleaseSwapchainImage", xrReleaseSwapchainImage) &&
              Get("xrGetInstanceProperties", xrGetInstanceProperties) && Get("xrGetSystemProperties", xrGetSystemProperties) &&
              Get("xrResultToString", xrResultToString) && Get("xrGetD3D11GraphicsRequirementsKHR", xrGetD3D11GraphicsRequirementsKHR);
    if (!ok) return false;
    XrInstanceProperties ip = {XR_TYPE_INSTANCE_PROPERTIES};
    xrGetInstanceProperties(g_inst, &ip);
    Log("xr: instance from runtime '%s' %u.%u.%u", ip.runtimeName, XR_VERSION_MAJOR(ip.runtimeVersion), XR_VERSION_MINOR(ip.runtimeVersion), XR_VERSION_PATCH(ip.runtimeVersion));
    return true;
}

static bool g_noHmdLogged;
static bool CreateSession() {
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult gr = xrGetSystem(g_inst, &si, &g_sys);
    if (gr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        if (!g_noHmdLogged) { Log("xr: headset not available yet (connect Virtual Desktop) - retrying"); g_noHmdLogged = true; }
        return false;
    }
    if (XR_FAILED(gr)) { Log("xr: xrGetSystem failed: %s", R(gr)); g_failed = true; return false; }
    XrSystemProperties sp = {XR_TYPE_SYSTEM_PROPERTIES};
    xrGetSystemProperties(g_inst, g_sys, &sp);
    Log("xr: system '%s'", sp.systemName);
    XrGraphicsRequirementsD3D11KHR req = {XR_TYPE_GRAPHICS_REQUIREMENTS_D3D11_KHR};
    CHK(xrGetD3D11GraphicsRequirementsKHR(g_inst, g_sys, &req));
    XrGraphicsBindingD3D11KHR gb = {XR_TYPE_GRAPHICS_BINDING_D3D11_KHR};
    gb.device = g_dev;
    XrSessionCreateInfo sc = {XR_TYPE_SESSION_CREATE_INFO};
    sc.next = &gb;
    sc.systemId = g_sys;
    CHK(xrCreateSession(g_inst, &sc, &g_sess));
    XrReferenceSpaceCreateInfo rs = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
    rs.poseInReferenceSpace.orientation.w = 1;
    CHK(xrCreateReferenceSpace(g_sess, &rs, &g_space));
    rs.referenceSpaceType = XR_REFERENCE_SPACE_TYPE_VIEW;
    if (XR_FAILED(xrCreateReferenceSpace(g_sess, &rs, &g_viewSpace))) { g_viewSpace = XR_NULL_HANDLE; Log("xr: no VIEW space - HUD panel off"); }
    XrViewConfigurationView vcv[2] = {{XR_TYPE_VIEW_CONFIGURATION_VIEW}, {XR_TYPE_VIEW_CONFIGURATION_VIEW}};
    uint32_t nv = 0;
    CHK(xrEnumerateViewConfigurationViews(g_inst, g_sys, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 2, &nv, vcv));
    Log("xr: recommended per-eye %ux%u", vcv[0].recommendedImageRectWidth, vcv[0].recommendedImageRectHeight);
    return true;
}

static bool CreateSwapchains(ID3D11Texture2D* eyeTex) {
    D3D11_TEXTURE2D_DESC d; eyeTex->GetDesc(&d);
    int64_t fmts[64]; uint32_t nf = 0;
    CHK(xrEnumerateSwapchainFormats(g_sess, 64, &nf, fmts));
    DXGI_FORMAT cands[2];
    if (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) { cands[0] = DXGI_FORMAT_B8G8R8A8_UNORM_SRGB; cands[1] = DXGI_FORMAT_B8G8R8A8_UNORM; }
    else { cands[0] = DXGI_FORMAT_R8G8B8A8_UNORM_SRGB; cands[1] = DXGI_FORMAT_R8G8B8A8_UNORM; }
    int64_t want = 0;
    for (int c = 0; c < 2 && !want; c++) for (uint32_t i = 0; i < nf; i++) if (fmts[i] == cands[c]) { want = cands[c]; break; }
    if (!want) { Log("xr: no swapchain format compatible with the eye format %d", (int)d.Format); return false; }
    g_w = d.Width; g_h = d.Height;
    for (int e = 0; e < 2; e++) {
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = want; ci.sampleCount = 1; ci.width = g_w; ci.height = g_h; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        CHK(xrCreateSwapchain(g_sess, &ci, &g_swc[e]));
        XrSwapchainImageD3D11KHR im[8];
        for (auto& x : im) x = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        CHK(xrEnumerateSwapchainImages(g_swc[e], 8, &g_nimg[e], (XrSwapchainImageBaseHeader*)im));
        for (uint32_t i = 0; i < g_nimg[e]; i++) g_img[e][i] = im[i].texture;
    }
    Log("xr: swapchains %ux%u format %lld, %u images each", g_w, g_h, (long long)want, g_nimg[0]);
    return true;
}

static void PollEvents() {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    while (xrPollEvent(g_inst, &ev) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            g_state = ((XrEventDataSessionStateChanged*)&ev)->state;
            Log("xr: session state %d", (int)g_state);
            if (g_state == XR_SESSION_STATE_READY) {
                XrSessionBeginInfo bi = {XR_TYPE_SESSION_BEGIN_INFO};
                bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                XrResult r = xrBeginSession(g_sess, &bi);
                g_running = XR_SUCCEEDED(r);
                Log("xr: xrBeginSession -> %s", R(r));
            } else if (g_state == XR_SESSION_STATE_STOPPING) {
                xrEndSession(g_sess);
                g_running = false; g_frameBegun = false;
            }
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
}

// build 027: frame timing capture (F9): per render frame the present interval, time blocked in xrWaitFrame, how many
// game frames (eye-0 passes) began since the previous present, the ring offset used for the pose, the display period
struct TimeRec { float dt, wait; int games, ahead; float period; unsigned char both, render; };
static TimeRec g_trec[300]; static volatile LONG g_tcap = -1; static LONG g_lastRingAtPresent;
static LARGE_INTEGER g_qpf, g_lastPresentT; static float g_lastWait;
static int g_hitchProbe, g_devNotes; static float g_gpuWaitMs;   // 138
static float* g_xrEndMsSlot; volatile float g_gameFrameMs;   // 136: game thread's frame time, written by main.cpp at each eye-0 pass
void XrTimingCapture() { if (g_tcap < 0) g_tcap = 0; }
static void BeginNextFrame() {
    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    // build 033: wait into a local frame state. The game thread reads the predicted display time (XrLocateEyes) while
    // this thread is blocked here; zeroing the shared state before the wait made the eye pass fail for ~a third of the
    // frames, which were then rendered without the headset pose (the "two points" flicker / judder)
    XrFrameState fs = {XR_TYPE_FRAME_STATE};
    LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
    XrResult wr = xrWaitFrame(g_sess, &wi, &fs);
    if (XR_SUCCEEDED(wr)) { g_fs = fs; g_predPeriod = fs.predictedDisplayPeriod; g_predTime = fs.predictedDisplayTime; }
    QueryPerformanceCounter(&b); if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
    g_lastWait = (float)(b.QuadPart - a.QuadPart) * 1000.f / (float)g_qpf.QuadPart;
    if (XR_FAILED(wr)) { g_frameBegun = false; return; }
    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    g_frameBegun = XR_SUCCEEDED(xrBeginFrame(g_sess, &bi));
    g_copied[0] = g_copied[1] = false;
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = g_fs.predictedDisplayTime;
    li.space = g_space;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    g_views[0] = {XR_TYPE_VIEW}; g_views[1] = {XR_TYPE_VIEW};
    if (XR_SUCCEEDED(xrLocateViews(g_sess, &li, &vs, 2, &n, g_views)) && n == 2) { g_renderPose[0] = g_views[0].pose; g_renderPose[1] = g_views[1].pose; }
}

bool XrLocateEyes(XrView out[2]) {   // game thread: views for the next display time
    XrTime pt = g_predTime, pp = g_predPeriod;
    if (!g_running || !g_sess || !pt) { InterlockedIncrement(&g_locateFails); return false; }
    XrViewLocateInfo li = {XR_TYPE_VIEW_LOCATE_INFO};
    li.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
    li.displayTime = pt + pp;
    li.space = g_space;
    XrViewState vs = {XR_TYPE_VIEW_STATE};
    uint32_t n = 0;
    out[0] = {XR_TYPE_VIEW}; out[1] = {XR_TYPE_VIEW};
    if (XR_FAILED(xrLocateViews(g_sess, &li, &vs, 2, &n, out)) || n != 2 || !(vs.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT)) { InterlockedIncrement(&g_locateFails); return false; }
    return true;
}
// build 023: menu mode (pause / front-end blur active): the frame was rendered mono from the game camera with a fixed
// symmetric FOV; it is shown as a world-locked flat screen instead of the head-tracked projection layer
static volatile bool g_nextMenu; static bool g_ringMenu[8];
static float g_bgDist = 3.0f, g_bgTan = 0.8391f;   // 2 x atan(0.8391) = 80 degrees wide
void XrSetMenuMode(bool m) { g_nextMenu = m; }
float XrMenuTan() { return g_bgTan; }
static LONG g_ringSeq[8]; static volatile LONG g_presentSeq = -1; static LONG g_seqHits, g_seqMiss;
// 089: the in-game menu reads and changes the HUD panel distance and width (metres)
void XrHudGet(float* dist, float* width) { *dist = g_hudDist; *width = g_hudWidth; }
void XrHudSet(float dist, float width) {
    if (width >= 0.3f && width <= 10.f) g_hudWidth = width;
    if (dist >= 0.3f && dist <= 10.f && dist != g_hudDist) { g_hudDist = dist; XrSetHudAnchor(g_ancHead[0], g_ancHead[1], g_ancHead[2], g_ancYaw); }   // 092: the fixed panel moves at once
}
long XrSetFrameViews(const XrView v[2]) {   // game thread: the views this frame was rendered with; returns its sequence number
    LONG n = InterlockedIncrement(&g_ringN);
    g_ring[n & 7][0] = v[0]; g_ring[n & 7][1] = v[1]; g_ringMenu[n & 7] = g_nextMenu; g_ringSeq[n & 7] = n; g_ringOk[n & 7] = true;
    return n;
}
// build 028: render thread - the sequence number of the frame now being presented (carried through the game's own eye-copy
// render command), so the submitted pose is exactly the one the frame was rendered with
void XrSetPresentSeq(long seq) { g_presentSeq = seq; }
void XrConfigure(const wchar_t* ini) {
    g_hitchProbe = GetPrivateProfileIntW(L"VR", L"HitchProbe", 0, ini);   // 138 test; 139: off unless set (times the graphics card before each xrEndFrame)
    g_devNotes = GetPrivateProfileIntW(L"VR", L"DevTools", 0, ini) || g_hitchProbe;   // 139: headroom lines only for development
    { wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"HudDistance", L"1.4", b, 32, ini); g_hudDist = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"HudWidth", L"1.5", b, 32, ini); g_hudWidth = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"HudHeightOffset", L"0", b, 32, ini); g_hudY = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"CutsceneHudScale", L"1.25", b, 32, ini); g_cutScale = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"MenuHudScale", L"1.3", b, 32, ini); g_menuHudScale = (float)_wtof(b); if (g_menuHudScale < 0.3f) g_menuHudScale = 1.f;
      GetPrivateProfileStringW(L"VR", L"MinimapAspect", L"1.7778", b, 32, ini); g_menuAspect = (float)_wtof(b); if (g_menuAspect < 0.5f) g_menuAspect = 16.f / 9.f;
      g_menuCrop = GetPrivateProfileIntW(L"VR", L"MenuCrop", 1, ini);
      GetPrivateProfileStringW(L"VR", L"CutsceneSubtitleY", L"0.22", b, 32, ini); g_cutSubY = (float)_wtof(b);
      g_hudAlpha = GetPrivateProfileIntW(L"VR", L"HudAlpha", 1, ini);
      GetPrivateProfileStringW(L"VR", L"HudScale", L"1.0", b, 32, ini); XrHudScaleSet((float)_wtof(b));
      GetPrivateProfileStringW(L"VR", L"HudPivotX", L"0.066", b, 32, ini); g_hudPivU = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"HudPivotY", L"0.72", b, 32, ini); g_hudPivV = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"MenuScreenDistance", L"3.0", b, 32, ini); g_bgDist = (float)_wtof(b);
      GetPrivateProfileStringW(L"VR", L"MenuScreenFov", L"80", b, 32, ini); g_bgTan = tanf((float)_wtof(b) * 0.5f * 3.14159265f / 180.f); }
    g_ini = ini;
    g_poseDelay = GetPrivateProfileIntW(L"VR", L"PoseDelay", 1, ini) & 7;
    wchar_t b[32]; GetPrivateProfileStringW(L"VR", L"TestFovV", L"55", b, 32, ini); g_testFovV = (float)_wtof(b);
}
bool XrActive() { return g_running && g_frameBegun; }

// called from Compose (render thread) with each eye image
void OsdBlit(ID3D11DeviceContext* ctx, ID3D11Texture2D* dst, UINT ew, UINT eh);
void EyeCapGrab(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src);
void XrSubmitEye(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src) {
    if (g_failed || eye < 0 || eye > 1 || !src) return;
    if (!g_inst) {
        ctx->GetDevice(&g_dev); g_ctx = ctx; g_ctx->AddRef();
        if (!LoadRuntime() || !CreateInstance()) { g_failed = true; Log("xr: OpenXR disabled for this run"); return; }
    }
    if (!g_sess) {
        static long long nextTry = 0;
        if (g_frame < nextTry) return;
        nextTry = g_frame + 300;
        if (!CreateSession()) { if (g_sess) g_failed = true; return; }
        if (!CreateSwapchains(src)) { g_failed = true; Log("xr: OpenXR disabled for this run"); return; }
    }
    if (!XrActive() || g_copied[eye] || !g_swc[eye]) return;
    D3D11_TEXTURE2D_DESC d; src->GetDesc(&d);
    if (d.Width != g_w || d.Height != g_h) {   // build 019: the render size changed (settings apply) -> new swapchains
        if (eye != 0 || g_copied[1]) return;
        Log("xr: eye image now %ux%u (swapchains %ux%u) - recreating the swapchains", d.Width, d.Height, g_w, g_h);
        for (int e = 0; e < 2; e++) if (g_swc[e]) { xrDestroySwapchain(g_swc[e]); g_swc[e] = XR_NULL_HANDLE; }
        if (!CreateSwapchains(src)) { g_failed = true; Log("xr: OpenXR disabled for this run"); return; }
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_swc[eye], &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_swc[eye], &wi);
    D3D11_BOX box = {0, 0, 0, g_w, g_h, 1};
    g_ctx->CopySubresourceRegion(g_img[eye][idx], 0, 0, 0, 0, src, 0, &box);
    EyeCapGrab(eye, g_ctx, src);                 // build 013: F10 eye capture (before the label)
    OsdBlit(g_ctx, g_img[eye][idx], g_w, g_h);   // build 011: mode label
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_swc[eye], &ri);
    g_copied[eye] = true;
}

// render thread, right after the game drew its Scaleform UI into tex: copy it into the HUD swapchain
void XrHudSubmit(ID3D11DeviceContext* ctx, ID3D11Texture2D* tex) {
    if (g_failed || !g_sess || !XrActive() || !g_viewSpace || !tex) return;
    D3D11_TEXTURE2D_DESC d; tex->GetDesc(&d);
    if (g_hudSwc && (d.Width != g_hudW || d.Height != g_hudH)) { xrDestroySwapchain(g_hudSwc); g_hudSwc = XR_NULL_HANDLE; }
    if (!g_hudSwc) {
        XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
        ci.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT;
        ci.format = (d.Format == DXGI_FORMAT_B8G8R8A8_UNORM || d.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB) ? DXGI_FORMAT_B8G8R8A8_UNORM_SRGB : DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
        ci.sampleCount = 1; ci.width = d.Width; ci.height = d.Height; ci.faceCount = 1; ci.arraySize = 1; ci.mipCount = 1;
        if (XR_FAILED(xrCreateSwapchain(g_sess, &ci, &g_hudSwc))) { g_hudSwc = XR_NULL_HANDLE; LOG_FIRST(1, "xr: HUD swapchain failed"); return; }
        XrSwapchainImageD3D11KHR im[8]; for (auto& x : im) x = {XR_TYPE_SWAPCHAIN_IMAGE_D3D11_KHR};
        xrEnumerateSwapchainImages(g_hudSwc, 8, &g_hudN, (XrSwapchainImageBaseHeader*)im);
        for (uint32_t i = 0; i < g_hudN; i++) g_hudImg[i] = im[i].texture;
        g_hudW = d.Width; g_hudH = d.Height;
        Log("xr: HUD swapchain %ux%u, panel %.2f m wide at %.2f m", g_hudW, g_hudH, g_hudWidth, g_hudDist);
    }
    uint32_t idx = 0;
    XrSwapchainImageAcquireInfo ai = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g_hudSwc, &ai, &idx))) return;
    XrSwapchainImageWaitInfo wi = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO}; wi.timeout = XR_INFINITE_DURATION;
    xrWaitSwapchainImage(g_hudSwc, &wi);
    D3D11_BOX box = {0, 0, 0, g_hudW, g_hudH, 1};
    ctx->CopySubresourceRegion(g_hudImg[idx], 0, 0, 0, 0, tex, 0, &box);
    XrSwapchainImageReleaseInfo ri = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g_hudSwc, &ri);
    g_hudFrame = g_frame;
}

// after the game's Present (render thread): end this frame, start the next
void XrAfterPresent() {
    ++g_frame;
    if (g_failed || !g_sess) return;
    PollEvents();
    if (g_running && g_frameBegun) {
        XrCompositionLayerProjectionView pv[2];
        float ty = tanf(g_testFovV * 0.5f * 3.14159265f / 180.f), tx = ty * (float)g_w / (float)(g_h ? g_h : 1);
        XrFovf fov = {-atanf(tx), atanf(tx), atanf(ty), -atanf(ty)};
        LONG rn = g_ringN - g_poseDelay; int slot = rn & 7;
        { LONG ps = g_presentSeq; if (ps > 0 && g_ringSeq[ps & 7] == ps && g_ringN - ps < 7) { rn = ps; slot = ps & 7; g_seqHits++; } else g_seqMiss++;
          if (((g_seqHits + g_seqMiss) % 3600) == 0) Log("xr: pose matched by frame number %ld times, fallback %ld times", g_seqHits, g_seqMiss); }
        bool tracked = g_ringN > 0 && g_ringOk[slot];
        for (int e = 0; e < 2; e++) {
            pv[e] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
            pv[e].pose = tracked ? g_ring[slot][e].pose : g_renderPose[e];
            if (tracked) fov = g_ring[slot][e].fov;
            if (pv[e].pose.orientation.w == 0 && pv[e].pose.orientation.x == 0 && pv[e].pose.orientation.y == 0 && pv[e].pose.orientation.z == 0) pv[e].pose.orientation.w = 1;
            pv[e].fov = fov;
            pv[e].subImage.swapchain = g_swc[e];
            pv[e].subImage.imageRect = {{0, 0}, {(int32_t)g_w, (int32_t)g_h}};
        }
        XrCompositionLayerProjection layer = {XR_TYPE_COMPOSITION_LAYER_PROJECTION};
        layer.space = g_space; layer.viewCount = 2; layer.views = pv;
        XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        bool hud = g_hudSwc && g_viewSpace && g_hudFrame >= g_frame - 2;
        bool menuNow = tracked && g_ringMenu[slot];
        if (hud) {
            quad.layerFlags = g_hudAlpha ? XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT : 0;
            if (g_hudAlpha == 2) quad.layerFlags |= XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
            quad.space = g_hudWorld ? g_space : g_viewSpace; quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain = g_hudSwc; quad.subImage.imageRect = {{0, 0}, {(int32_t)g_hudW, (int32_t)g_hudH}};
            if (g_hudWorld) quad.pose = g_hudAnchor;
            else { quad.pose.orientation.w = 1; quad.pose.position = {0.f, g_hudY, -g_hudDist}; }
            quad.size = {g_hudWidth, g_hudWidth * (float)g_hudH / (float)g_hudW};
            if (menuNow && g_menuCrop) {   // build 045: menus show only the game's 16:9 screen area of the HUD (the flat screen),
                // so map names placed off-screen stay hidden; and the panel is MenuHudScale larger
                int bh = (int)((float)g_hudW / g_menuAspect + 0.5f); if (bh > (int)g_hudH) bh = (int)g_hudH;
                quad.subImage.imageRect = {{0, ((int32_t)g_hudH - bh) / 2}, {(int32_t)g_hudW, bh}};
                quad.size = {g_hudWidth * g_menuHudScale, g_hudWidth * g_menuHudScale * (float)bh / (float)g_hudW};
            }
            if (!(menuNow && g_menuCrop) && !g_hudCut && g_hudScale != 1.f) {   // 093: scale about the pivot (gameplay HUD only)
                float W = quad.size.width, H = quad.size.height, k = g_hudScale - 1.f;
                float dx = (0.5f - g_hudPivU) * W * k, dy = (g_hudPivV - 0.5f) * H * k;   // centre shift: right, up (metres, in the panel plane)
                float yaw = g_hudWorld ? 2.f * atan2f(quad.pose.orientation.y, quad.pose.orientation.w) : 0.f;
                quad.pose.position.x += cosf(yaw) * dx; quad.pose.position.z -= sinf(yaw) * dx; quad.pose.position.y += dy;
                quad.size.width = W * g_hudScale; quad.size.height = H * g_hudScale;
            }
            if (g_hudHidden && !menuNow && !g_hudCut) hud = false;   // 093: HUD off = no gameplay HUD at all
            if (g_hudCut && g_cutScale > 0.f && g_cutScale != 1.f) {   // build 041: cutscenes - bigger panel, scaled about the subtitle line
                quad.size.width *= g_cutScale; quad.size.height *= g_cutScale;
                quad.pose.position.y += (g_cutScale - 1.f) * g_cutSubY * g_hudWidth;
            }
        }
        bool menu = tracked && g_ringMenu[slot];
        XrCompositionLayerQuad bg = {XR_TYPE_COMPOSITION_LAYER_QUAD};
        if (menu) {   // the eye 0 image as a world-locked screen, in the same direction as the HUD panel
            float yaw = 2.f * atan2f(g_hudAnchor.orientation.y, g_hudAnchor.orientation.w);
            float hx = g_hudAnchor.position.x + sinf(yaw) * g_hudDist, hz = g_hudAnchor.position.z + cosf(yaw) * g_hudDist;   // head point of the recentre
            bg.space = g_space; bg.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
            bg.subImage.swapchain = g_swc[0]; bg.subImage.imageRect = {{0, 0}, {(int32_t)g_w, (int32_t)g_h}};
            bg.pose.orientation = g_hudAnchor.orientation;
            bg.pose.position = {hx - sinf(yaw) * g_bgDist, g_hudAnchor.position.y - g_hudY, hz - cosf(yaw) * g_bgDist};
            float wq = 2.f * g_bgDist * g_bgTan; bg.size = {wq, wq * (float)g_h / (float)g_w};
        }
        const XrCompositionLayerBaseHeader* layers[3]; uint32_t nl = 0;
        layers[nl++] = menu ? (XrCompositionLayerBaseHeader*)&bg : (XrCompositionLayerBaseHeader*)&layer;
        if (hud) layers[nl++] = (XrCompositionLayerBaseHeader*)&quad;
        XrFrameEndInfo ei = {XR_TYPE_FRAME_END_INFO};
        ei.displayTime = g_fs.predictedDisplayTime;
        ei.environmentBlendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        bool both = g_copied[0] && g_copied[1];
        ei.layerCount = both && g_fs.shouldRender ? nl : 0;
        ei.layers = layers;
        if (g_tcap >= 0 && g_tcap < 300) {
            LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
            TimeRec& r = g_trec[g_tcap];
            r.dt = g_lastPresentT.QuadPart ? (float)(t.QuadPart - g_lastPresentT.QuadPart) * 1000.f / (float)g_qpf.QuadPart : 0.f;
            r.wait = g_lastWait; r.games = (int)(g_ringN - g_lastRingAtPresent); r.ahead = (int)(g_ringN - rn);
            r.period = (float)g_fs.predictedDisplayPeriod / 1e6f; r.both = both; r.render = g_fs.shouldRender;
            if (++g_tcap == 300) {
                Log("timing: 300 frames - present interval ms / xrWaitFrame ms / game frames since last present / display period %.2f ms", r.period);
                char line[1200]; int o = 0;
                for (int i = 0; i < 300; i++) {
                    o += snprintf(line + o, sizeof line - o, " %.1f/%.1f/%d%s%s", g_trec[i].dt, g_trec[i].wait, g_trec[i].games, g_trec[i].both ? "" : "!", g_trec[i].render ? "" : "x");
                    if (i % 20 == 19) { Log("  %s", line); o = 0; }
                }
                g_tcap = -1;
            }
        }
        LARGE_INTEGER tEnd0;
        {   // 136: long-frame notes - every present interval well over the display period, with where the time went
            LARGE_INTEGER t; QueryPerformanceCounter(&t); if (!g_qpf.QuadPart) QueryPerformanceFrequency(&g_qpf);
            static int notes; static float lastEnd; static long long frames;
            float dt = g_lastPresentT.QuadPart ? (float)(t.QuadPart - g_lastPresentT.QuadPart) * 1000.f / (float)g_qpf.QuadPart : 0.f;
            float period = (float)g_predPeriod / 1e6f;
            frames++;
            if (frames > 300 && period > 1.f && dt > period * 1.5f && dt < 2000.f && notes < 200) {
                notes++;
                // "wait" = blocked in xrWaitFrame after the previous present; "submit" = that present's xrEndFrame;
                // the rest is this thread drawing the game's frame (or waiting for the game thread to send it)
                Log("long frame %d: %.1f ms between presents (display period %.2f) - previous xrEndFrame %.1f, xrWaitFrame %.1f, rest (game + drawing) %.1f; game frames in it %d; game thread's last frame %.1f ms; graphics card finish wait before that xrEndFrame %.1f ms",
                    notes, dt, period, lastEnd, g_lastWait, dt - lastEnd - g_lastWait, (int)(g_ringN - g_lastRingAtPresent), g_gameFrameMs, g_gpuWaitMs);
            }
            {   // 137: headroom - how long this thread sleeps in xrWaitFrame (time to spare) over each 600 presents
                static float sw, se, sr, mnw = 1e9f, mxe, sg, mxg; static int n, tight;
                if (dt > 0 && dt < 2000.f) { sw += g_lastWait; se += lastEnd; sr += dt - lastEnd - g_lastWait; if (g_lastWait < mnw) mnw = g_lastWait; if (lastEnd > mxe) mxe = lastEnd; if (g_lastWait < 1.f) tight++; sg += g_gpuWaitMs; if (g_gpuWaitMs > mxg) mxg = g_gpuWaitMs; n++; }
                if (n >= 600) { if (g_devNotes) Log("headroom over 600 presents: xrWaitFrame average %.1f ms (least %.1f, under 1 ms in %d frames), xrEndFrame average %.1f (longest %.1f), game + drawing average %.1f (of it graphics card finish wait %.1f, longest %.1f); display period %.2f",
                                    sw / n, mnw, tight, se / n, mxe, sr / n, sg / n, mxg, period); sw = se = sr = sg = 0; mnw = 1e9f; mxe = mxg = 0; n = tight = 0; }
            }
            g_lastPresentT = t; g_lastRingAtPresent = g_ringN; tEnd0 = t; g_xrEndMsSlot = &lastEnd;
        }
        if (g_hitchProbe && g_ctx && g_dev) {   // 138: wait here until the graphics card has finished everything sent so far, and time it
            static ID3D11Query* q; if (!q) { D3D11_QUERY_DESC qd = {D3D11_QUERY_EVENT, 0}; g_dev->CreateQuery(&qd, &q); }
            if (q) {
                LARGE_INTEGER a, b; QueryPerformanceCounter(&a);
                g_ctx->End(q); g_ctx->Flush();
                for (;;) { BOOL done = FALSE; HRESULT h = g_ctx->GetData(q, &done, sizeof done, 0); QueryPerformanceCounter(&b);
                           if ((h == S_OK && done) || FAILED(h) || (b.QuadPart - a.QuadPart) * 1000 / g_qpf.QuadPart > 150) break; SwitchToThread(); }
                g_gpuWaitMs = (float)(b.QuadPart - a.QuadPart) * 1000.f / (float)g_qpf.QuadPart;
                QueryPerformanceCounter(&tEnd0);
            }
        }
        XrResult r = xrEndFrame(g_sess, &ei);
        { LARGE_INTEGER t; QueryPerformanceCounter(&t); if (g_xrEndMsSlot) *g_xrEndMsSlot = (float)(t.QuadPart - tEnd0.QuadPart) * 1000.f / (float)g_qpf.QuadPart; }
        if (XR_FAILED(r)) LOG_FIRST(5, "xr: xrEndFrame failed: %s", R(r));
        if (!both) LOG_FIRST(3, "xr: frame ended without both eyes (copied %d %d)", (int)g_copied[0], (int)g_copied[1]);
        g_frameBegun = false;
    }
    if (g_running) BeginNextFrame();
}
