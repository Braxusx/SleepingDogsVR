// app.cpp - Sleeping Dogs VR launcher: window, Direct3D 11 + Dear ImGui loop, pages.
// Built on the Far Cry 2 VR launcher's code (itself modelled on Pizza Parker's Dishonored VR launcher, with permission);
// same layout as the Dragon's Dogma VR launcher (sidebar with the emblem, banner art on top, a "Ready to play" card) in a
// Hong Kong neon look.
#include "game.h"
#include "theme.h"
#include "payload_ids.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <d3d11.h>
#include <thread>
#include <cmath>
#include <cstdio>
#include <atomic>
#include <mutex>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
namespace sdvr { int ElevatedMain(int argc, wchar_t** argv); }
using namespace sdvr;
using theme::g_s;

// ---------------------------------------------------------------- D3D
static ID3D11Device* g_dev = nullptr; static ID3D11DeviceContext* g_ctx = nullptr;
static IDXGISwapChain* g_swap = nullptr; static ID3D11RenderTargetView* g_rtv = nullptr;
static UINT g_resizeW = 0, g_resizeH = 0;
static float g_scale = 1.0f; static bool g_fontsDirty = false;
static int g_framesPending = 3;

static void CreateRT() { ID3D11Texture2D* bb = nullptr; g_swap->GetBuffer(0, IID_PPV_ARGS(&bb)); if (bb) { g_dev->CreateRenderTargetView(bb, nullptr, &g_rtv); bb->Release(); } }
static bool CreateDevice(HWND hwnd) {
    DXGI_SWAP_CHAIN_DESC sd{}; sd.BufferCount = 2; sd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM; sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd; sd.SampleDesc.Count = 1; sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    const D3D_FEATURE_LEVEL fl[] = { D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_1, D3D_FEATURE_LEVEL_10_0 }; D3D_FEATURE_LEVEL got;
    HRESULT hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) { sd.BufferCount = 1; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD; hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx); }
    if (FAILED(hr)) hr = D3D11CreateDeviceAndSwapChain(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, fl, 3, D3D11_SDK_VERSION, &sd, &g_swap, &g_dev, &got, &g_ctx);
    if (FAILED(hr)) return false;
    CreateRT(); return true;
}
// the art (JPEG / PNG resources) decoded with WIC into D3D11 textures
static theme::Image LoadImageRes(int id) {
    theme::Image img;
    HRSRC r = FindResourceW(nullptr, MAKEINTRESOURCEW(id), (LPCWSTR)RT_RCDATA); if (!r) return img;
    HGLOBAL hg = LoadResource(nullptr, r); BYTE* data = hg ? (BYTE*)LockResource(hg) : nullptr; DWORD n = hg ? SizeofResource(nullptr, r) : 0;
    if (!data) return img;
    IWICImagingFactory* f = nullptr; IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr; IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* cv = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f))) && SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory(data, n))
        && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec)) && SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&cv))
        && SUCCEEDED(cv->Initialize(fr, GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) {
        UINT w = 0, h = 0; cv->GetSize(&w, &h);
        std::vector<BYTE> px((size_t)w * h * 4);
        if (SUCCEEDED(cv->CopyPixels(nullptr, w * 4, (UINT)px.size(), px.data()))) {
            D3D11_TEXTURE2D_DESC td{}; td.Width = w; td.Height = h; td.MipLevels = 1; td.ArraySize = 1; td.Format = DXGI_FORMAT_R8G8B8A8_UNORM; td.SampleDesc.Count = 1; td.Usage = D3D11_USAGE_IMMUTABLE; td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            D3D11_SUBRESOURCE_DATA sd{ px.data(), w * 4, 0 }; ID3D11Texture2D* t = nullptr; ID3D11ShaderResourceView* srv = nullptr;
            if (SUCCEEDED(g_dev->CreateTexture2D(&td, &sd, &t)) && SUCCEEDED(g_dev->CreateShaderResourceView(t, nullptr, &srv))) { img.tex = (ImTextureID)srv; img.w = (int)w; img.h = (int)h; }
            if (t) t->Release();
        }
    }
    if (cv) cv->Release(); if (fr) fr->Release(); if (dec) dec->Release(); if (st) st->Release(); if (f) f->Release();
    if (!img.tex) Log("art %d: decode failed", id);
    return img;
}

// ---------------------------------------------------------------- app state
enum Page { PageMain, PageSettings, PageOptional, PageAbout };
struct App {
    Page page = PageMain;
    std::vector<Game> games; int gameIdx = -1;
    Game game; State state; bool running = false; GameConfig cfg;   // 0.8.1: the game's own settings
    Runtime runtime;
    std::atomic<bool> busy{ false }; std::string busyText; std::mutex mx; bool haveReport = false; Report report; Report pending; bool pendingReady = false;
    bool confirmUninstall = false, deleteIni = false;
    std::string notice; ImU32 noticeCol = 0; double noticeAt = 0;
    float scrollReset = -1;
} g_app;

static void Notice(const std::string& t, ImU32 col) { g_app.notice = t; g_app.noticeCol = col; g_app.noticeAt = ImGui::GetTime(); }
static void SelectGame(int i) {
    g_app.gameIdx = i; g_app.game = i >= 0 && i < (int)g_app.games.size() ? g_app.games[i] : Game{};
    g_app.state = Check(g_app.game);
    if (g_app.game.valid) SetPref(L"Game", L"Directory", g_app.game.root);
}
static void Rescan() {
    g_app.games = FindGames();
    std::wstring last = GetPref(L"Game", L"Directory", L""); int sel = g_app.games.empty() ? -1 : 0;
    for (int i = 0; i < (int)g_app.games.size(); ++i) if (_wcsicmp(g_app.games[i].root.c_str(), last.c_str()) == 0) sel = i;
    SelectGame(sel);
    g_app.runtime = ActiveRuntime(); g_app.cfg = CheckGameConfig(g_app.game);
}
static void Refresh() { if (g_app.game.valid) g_app.state = Check(g_app.game); g_app.running = GameRunning(); g_app.runtime = ActiveRuntime(); g_app.cfg = CheckGameConfig(g_app.game); }

static void StartOp(const char* op, bool deleteIni) {
    if (g_app.busy) return;
    g_app.busy = true; g_app.busyText = !strcmp(op, "install") ? "Installing Sleeping Dogs VR..." : "Removing Sleeping Dogs VR...";
    g_app.haveReport = false; g_app.notice.clear();
    Game g = g_app.game; std::string o = op;
    std::thread([g, o, deleteIni] {
        CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        Report r = RunOperation(g, o.c_str(), deleteIni);
        { std::lock_guard<std::mutex> l(g_app.mx); g_app.pending = r; g_app.pendingReady = true; }
        g_app.busy = false;
        CoUninitialize();
    }).detach();
}
static void PollWorker() {
    std::lock_guard<std::mutex> l(g_app.mx);
    if (!g_app.pendingReady) return;
    g_app.pendingReady = false; g_app.report = g_app.pending; g_app.haveReport = true;
    Refresh(); g_framesPending = 3;
}
static void BrowseForGame(HWND hwnd) {
    IFileOpenDialog* d = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&d)))) return;
    DWORD o; d->GetOptions(&o); d->SetOptions(o | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM); d->SetTitle(L"Select the Sleeping Dogs: Definitive Edition folder (the one with SDHDShip.exe)");
    if (SUCCEEDED(d->Show(hwnd))) {
        IShellItem* it = nullptr;
        if (SUCCEEDED(d->GetResult(&it))) {
            PWSTR p = nullptr;
            if (SUCCEEDED(it->GetDisplayName(SIGDN_FILESYSPATH, &p))) {
                Game g = Inspect(p);
                if (g.valid) {
                    for (int i = 0; i < 8; ++i) { std::wstring k = L"Dir" + std::to_wstring(i); if (GetPref(L"Games", k.c_str(), L"").empty() || _wcsicmp(GetPref(L"Games", k.c_str(), L"").c_str(), g.root.c_str()) == 0) { SetPref(L"Games", k.c_str(), g.root); break; } }
                    SetPref(L"Game", L"Directory", g.root); Rescan(); Notice("Game folder added.", theme::Good());
                } else Notice("That folder doesn't contain Sleeping Dogs: Definitive Edition (SDHDShip.exe and data). Choose the game's main folder.", theme::Bad());
                CoTaskMemFree(p);
            }
            it->Release();
        }
    }
    d->Release();
}
static void DesktopShortcut() {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    PWSTR desk = nullptr; if (FAILED(SHGetKnownFolderPath(FOLDERID_Desktop, 0, nullptr, &desk))) return;
    std::wstring lnk = std::wstring(desk) + L"\\Sleeping Dogs VR.lnk"; CoTaskMemFree(desk);
    IShellLinkW* sl = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) {
        sl->SetPath(self); sl->SetIconLocation(self, 0); sl->SetDescription(L"Sleeping Dogs VR launcher");
        std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L'\\')); sl->SetWorkingDirectory(dir.c_str());
        IPersistFile* pf = nullptr;
        if (SUCCEEDED(sl->QueryInterface(IID_PPV_ARGS(&pf)))) { bool ok = SUCCEEDED(pf->Save(lnk.c_str(), TRUE)); pf->Release(); Notice(ok ? "Desktop shortcut created." : "Couldn't create the shortcut.", ok ? theme::Good() : theme::Bad()); }
        sl->Release();
    }
}

// ---------------------------------------------------------------- settings rows (written to sdvr.ini straight away)
struct Opt { const char* label; const wchar_t* value; };
static const Opt kUpscaler[] = { { "Off", L"0" }, { "NVIDIA DLSS", L"1" } };
// sdvr.ini DlssQuality: 0 DLAA, 4 Ultra Quality, 1 Quality, 2 Balanced, 3 Performance
static const Opt kQuality[] = { { "DLAA (native resolution)", L"0" }, { "Ultra Quality", L"4" }, { "Quality", L"1" }, { "Balanced", L"2" }, { "Performance", L"3" } };
static float QualityRatio(int q) { return q == 4 ? 0.77f : q == 1 ? 0.667f : q == 2 ? 0.58f : q == 3 ? 0.5f : 1.0f; }
static const Opt kHudMode[] = { { "Fixed in front of you", L"0" }, { "Follows your head", L"1" }, { "Hidden (menus and subtitles still show)", L"2" } };
static const Opt kMirror[] = { { "Left eye", L"0" }, { "Right eye", L"1" }, { "Alternating", L"2" } };
// Virtual Desktop's per-eye sizes for its Medium / High / Ultra / Godlike quality levels (read from the user's Quest 3, 13 Nov)
static const struct { const char* label; int w, h; } kRender[] = { { "2112 x 2304  (Medium)", 2112, 2304 }, { "2496 x 2688  (High)", 2496, 2688 }, { "2688 x 2880  (Ultra)", 2688, 2880 }, { "3072 x 3264  (Godlike, default)", 3072, 3264 } };
static const int kRenderN = 4;
static const Opt kPreset[] = { { "Transformer K (recommended)", L"11" }, { "Transformer J", L"10" }, { "Transformer 2 M", L"13" }, { "Transformer 2 L", L"12" }, { "NVIDIA recommended per mode", L"16" } };

static bool Installed() { return g_app.game.valid && g_app.state.iniExists; }
static const wchar_t* g_sec = L"VR";   // the sdvr.ini section the rows below read and write
static std::wstring Get(const wchar_t* key, const wchar_t* def) {
    if (!Installed()) return def;
    wchar_t buf[256]; GetPrivateProfileStringW(g_sec, key, def, buf, 256, IniPath(g_app.game).c_str());
    std::wstring v = buf; size_t c = v.find(L';'); if (c != std::wstring::npos) v = v.substr(0, c);
    while (!v.empty() && (v.back() == L' ' || v.back() == L'\t')) v.pop_back();
    return v;
}
static void Set(const wchar_t* key, const std::wstring& v) {
    if (!Installed()) return;
    WritePrivateProfileStringW(g_sec, key, v.c_str(), IniPath(g_app.game).c_str());
    Notice(g_app.running ? "Saved. Takes effect the next time the game starts." : "Saved.", theme::Good());
}
static void RowLabel(const char* label, const char* help, float ctrlW) {
    ImGui::BeginGroup();
    ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted(label); ImGui::PopFont();
    if (help) { ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - ctrlW - 24 * g_s); theme::Wrapped(help, theme::Muted()); ImGui::PopTextWrapPos(); }
    ImGui::EndGroup();
}
static void ComboRow(const char* label, const char* help, const wchar_t* key, const wchar_t* def, const Opt* o, int n, bool enabled = true) {
    ImGui::PushID(label);
    float ctrlW = 300 * g_s, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x;
    float y0 = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(!enabled);
    RowLabel(label, help, ctrlW);
    float y1 = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + avail - ctrlW, y0)); ImGui::SetNextItemWidth(ctrlW);
    std::wstring v = Get(key, def); int cur = -1; for (int k = 0; k < n; ++k) if (v == o[k].value) cur = k;
    if (ImGui::BeginCombo("##c", cur >= 0 ? o[cur].label : "(custom value)")) {
        for (int k = 0; k < n; ++k) if (ImGui::Selectable(o[k].label, k == cur)) Set(key, o[k].value);
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(std::max(y1, ImGui::GetCursorPosY()) + 6 * g_s);
    ImGui::PopID();
}
static void SliderRow(const char* label, const char* help, const wchar_t* key, const wchar_t* def, float mn, float mx, const char* fmt, float shown, bool enabled = true) {
    ImGui::PushID(label);
    float ctrlW = 300 * g_s, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x, y0 = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(!enabled);
    RowLabel(label, help, ctrlW);
    float y1 = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + avail - ctrlW, y0)); ImGui::SetNextItemWidth(ctrlW);
    // the value being dragged lives in ImGui's state storage; the ini is written when the slider is released
    ImGuiStorage* stg = ImGui::GetStateStorage(); ImGuiID vid = ImGui::GetID("##val"), aid = ImGui::GetID("##act");
    float& d = *stg->GetFloatRef(vid, 0.0f); bool& dragging = *stg->GetBoolRef(aid, false);
    if (!dragging) d = (float)_wtof(Get(key, def).c_str()) * shown;
    ImGui::SliderFloat("##s", &d, mn * shown, mx * shown, fmt);
    dragging = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) { wchar_t b[32]; swprintf(b, 32, L"%.2f", d / shown); std::wstring nv = b; while (nv.size() > 3 && nv.back() == L'0' && nv[nv.size() - 2] != L'.') nv.pop_back(); if (nv.size() > 3 && nv.substr(nv.size() - 2) == L".0" && shown == 1 && mx > 20) nv = nv.substr(0, nv.size() - 2); Set(key, nv); }
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(std::max(y1, ImGui::GetCursorPosY()) + 6 * g_s);
    ImGui::PopID();
}
static bool ToggleKey(const char* label, const char* help, const wchar_t* key, const wchar_t* def, bool invert = false, bool enabled = true) {
    bool on = (Get(key, def) != L"0") != invert;
    if (theme::Toggle(label, &on, help, enabled && Installed())) { Set(key, (on != invert) ? L"1" : L"0"); return true; }
    return false;
}
static void NeedInstall() {
    if (Installed()) return;
    theme::BeginCard("needinstall");
    theme::Dot(theme::Warn()); theme::Wrapped(g_app.game.valid ? "Install the VR mod on the Main Menu first; these settings live in its sdvr.ini." : "Choose your game folder on the Main Menu first.", theme::Text());
    theme::EndCard();
}

// ---------------------------------------------------------------- Main Menu
static void ReadyCard() {
    const State& s = g_app.state; bool ok = g_app.game.valid && s.installed;
    theme::BeginCard("ready", 22);
    ImDrawList* dl = ImGui::GetWindowDrawList(); float W = ImGui::GetContentRegionAvail().x;
    {   const char* t = !g_app.game.valid ? "GAME NOT FOUND" : !s.installed ? "INSTALL REQUIRED" : g_app.running ? "NOW PLAYING" : "READY TO PLAY";
        ImFont* f = theme::g_fonts.capsLg; float sp = 2.0f * g_s, tw = theme::SpacedCapsWidth(f, t, sp);
        ImVec2 c = ImGui::GetCursorScreenPos(); theme::SpacedCaps(dl, f, ImVec2(c.x + (W - tw) * 0.5f, c.y), ok ? theme::Teal() : theme::Warn(), t, sp);
        ImGui::Dummy(ImVec2(W, f->FontSize + 10 * g_s)); }
    {   // progress-style bar: how ready the setup is (game found, mod installed, runtime present)
        int steps = (g_app.game.valid ? 1 : 0) + (s.installed && !s.outdated ? 1 : 0) + (g_app.runtime.found ? 1 : 0);
        float frac = steps / 3.0f; ImVec2 c = ImGui::GetCursorScreenPos(); float h = 6 * g_s;
        dl->AddRectFilled(c, ImVec2(c.x + W, c.y + h), IM_COL32(40, 44, 60, 255), h);
        if (frac > 0) dl->AddRectFilled(c, ImVec2(c.x + W * frac, c.y + h), theme::Gold(), h);
        ImGui::Dummy(ImVec2(W, h + 4 * g_s));
        char l[64]; snprintf(l, sizeof l, "%d%%", (int)std::round(frac * 100));
        ImGui::PushFont(theme::g_fonts.bold); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Gold()), "%s", l); ImGui::PopFont();
        ImGui::SameLine(); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Muted()), "%s", !g_app.game.valid ? "Game folder needed" : !s.installed ? "VR mod not installed" : s.outdated ? "Update available: Reinstall" : ("Sleeping Dogs VR " + std::string(kModVersion)).c_str());
        std::string rt = g_app.runtime.found ? "Runtime: " + g_app.runtime.name : "No OpenXR runtime";
        float rw = ImGui::CalcTextSize(rt.c_str()).x; ImGui::SameLine(ImGui::GetCursorPosX() + 0); ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - rw - 22 * g_s));
        ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Muted()), "%s", rt.c_str());
    }
    {   // requirements strip with a gold edge
        ImVec2 c = ImGui::GetCursorScreenPos(); float h = ImGui::GetTextLineHeight() + 14 * g_s;
        dl->AddRectFilled(c, ImVec2(c.x + W - 22 * g_s, c.y + h), IM_COL32(16, 18, 28, 255), 4 * g_s); dl->AddRectFilled(c, ImVec2(c.x + 2.5f * g_s, c.y + h), theme::Gold());
        ImGui::PushFont(theme::g_fonts.small);
        dl->AddText(ImVec2(c.x + 14 * g_s, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f), theme::Muted(), "Requires: a PC VR headset with an OpenXR runtime  \xe2\x80\xa2  gamepad recommended  \xe2\x80\xa2  NVIDIA RTX for DLSS (optional)");
        ImGui::PopFont();
        ImGui::Dummy(ImVec2(W, h + 8 * g_s));
    }
    {   // 0.8.1: the game's own settings the VR mod needs
        const GameConfig& gc = g_app.cfg;
        if (gc.found && !gc.issues.empty()) {
            theme::Dot(theme::Warn()); ImGui::PushFont(theme::g_fonts.bold); ImGui::TextColored(ImGui::ColorConvertU32ToFloat4(theme::Warn()), "Game settings need a change for VR"); ImGui::PopFont();
            for (const auto& i : gc.issues) theme::Bullet(i.c_str());
            if (theme::Button("Fix game settings", true, !g_app.running && !g_app.busy)) {
                std::string err;
                if (FixGameConfig(g_app.game, err)) { Notice("Game settings fixed: 3D on, 1920 x 1080 window, V-Sync and frame limiter off. A backup of DisplaySettings.xml was kept.", theme::Good()); g_app.cfg = CheckGameConfig(g_app.game); }
                else Notice(err, theme::Bad());
            }
            if (g_app.running) { ImGui::SameLine(); theme::Wrapped("Close the game to fix them.", theme::Muted()); }
            ImGui::Dummy(ImVec2(W, 6 * g_s));
        } else {
            ImGui::PushFont(theme::g_fonts.small);
            if (gc.found) { theme::Dot(theme::Good()); theme::Wrapped("Game settings OK: 3D on, 1920 x 1080 window, V-Sync and frame limiter off.", theme::Muted()); }
            else { theme::Dot(theme::Muted()); theme::Wrapped("The launcher checks the game's display settings once the game has run for the first time.", theme::Muted()); }
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(W, 4 * g_s));
        }
    }
    float bw = 170 * g_s; ImGui::SetCursorPosX(ImGui::GetCursorPosX() + (W - bw) * 0.5f - 11 * g_s);
    bool can = ok && !g_app.busy && !g_app.running;
    if (theme::PlayButton(g_app.running ? "RUNNING" : "PLAY", can, bw)) {
        std::string err;
        if (Launch(g_app.game, err)) { Notice("Starting Sleeping Dogs VR... Put your headset on.", theme::Good()); g_app.running = true; }
        else Notice(err, theme::Bad());
    }
    theme::EndCard();
}
static void PageMainDraw(HWND hwnd) {
    theme::Banner("SLEEPING DOGS VR", "A VIRTUAL REALITY MOD", true);
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    float W = ImGui::GetContentRegionMax().x - 34 * g_s - ImGui::GetCursorPosX();
#ifdef SDVR_DIAGNOSTIC
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(255, 196, 70, 255)));
    ImGui::TextWrapped("DIAGNOSTIC BUILD. The mod plays the same as the normal release, and records extra detail for tracking down crashes and freezes. "
                       "If the game crashes or freezes, use About > Collect logs and send the zip with your report.");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1, 4 * g_s));
#endif
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::ColorConvertU32ToFloat4(IM_COL32(198, 204, 218, 255)));
    ImGui::TextWrapped("Hong Kong in full native stereo with head tracking, NVIDIA DLSS / DLAA, an in-headset HUD panel, optional first person view and the SPatch fixes built in");
    ImGui::PopStyleColor();
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    {   // VR mod info grid (two columns, like the concept's publisher / genre block)
        const State& s = g_app.state;
        std::string ver = std::string(kModVersion), status = !g_app.game.valid ? "Game not found" : s.installed ? (s.outdated ? "Installed (update available)" : "Installed") : "Not installed";
        std::string edition = !g_app.game.valid ? "-" : std::string(StoreName(g_app.game.store)) + (g_app.game.build == BuildUnknown ? " (unrecognised version)" : "");
        ImGui::Columns(2, "##info", false);
        theme::InfoPair("VR MOD VERSION", ver.c_str()); theme::InfoPair("GAME EDITION", edition.c_str());
        ImGui::NextColumn();
        theme::InfoPair("STATUS", status.c_str(), s.installed && !s.outdated ? theme::Good() : theme::Warn()); theme::InfoPair("SUPPORTED", "Steam and GOG");
        ImGui::Columns(1);
    }
    // game folders
    theme::SectionTitle("GAME FOLDERS");
    theme::BeginCard("games");
    if (g_app.games.empty()) { theme::Dot(theme::Bad()); theme::Wrapped("Sleeping Dogs: Definitive Edition wasn't found automatically. Use Browse to pick the game folder (the one with SDHDShip.exe).", theme::Text()); }
    for (int i = 0; i < (int)g_app.games.size(); ++i) {
        const Game& g = g_app.games[i]; State st = i == g_app.gameIdx ? g_app.state : Check(g);
        ImGui::PushID(i);
        ImVec2 c = ImGui::GetCursorScreenPos(); float w = ImGui::GetContentRegionAvail().x, h = 50 * g_s;
        bool sel = i == g_app.gameIdx;
        if (ImGui::InvisibleButton("##g", ImVec2(w, h)) && !g_app.busy) { SelectGame(i); g_app.haveReport = false; g_app.confirmUninstall = false; }
        bool hov = ImGui::IsItemHovered(); if (hov) ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(c, ImVec2(c.x + w, c.y + h), sel ? IM_COL32(46, 22, 34, 255) : hov ? theme::CardHi() : IM_COL32(24, 27, 40, 255), 6 * g_s);
        if (sel) dl->AddRect(c, ImVec2(c.x + w, c.y + h), theme::GoldDim(), 6 * g_s, 0, 1.2f * g_s);
        // radio
        ImVec2 rc(c.x + 20 * g_s, c.y + h * 0.5f); dl->AddCircle(rc, 7 * g_s, sel ? theme::Gold() : theme::Muted(), 20, 1.5f * g_s); if (sel) dl->AddCircleFilled(rc, 3.6f * g_s, theme::Gold(), 16);
        // store badge
        const char* sn = StoreName(g.store); ImFont* lf = theme::g_fonts.label; float bw = lf->CalcTextSizeA(lf->FontSize, 1e9f, 0, sn).x + 16 * g_s;
        ImVec2 b0(c.x + 40 * g_s, c.y + h * 0.5f - 10 * g_s); dl->AddRectFilled(b0, ImVec2(b0.x + bw, b0.y + 20 * g_s), g.store == StoreSteam ? IM_COL32(38, 60, 92, 255) : g.store == StoreGog ? IM_COL32(86, 46, 110, 255) : IM_COL32(60, 60, 60, 255), 4 * g_s);
        dl->AddText(lf, lf->FontSize, ImVec2(b0.x + 8 * g_s, b0.y + (20 * g_s - lf->FontSize) * 0.5f), IM_COL32(236, 236, 240, 255), sn);
        std::string path = Narrow(g.root);
        dl->PushClipRect(ImVec2(b0.x + bw + 12 * g_s, c.y), ImVec2(c.x + w - 170 * g_s, c.y + h), true);
        dl->AddText(theme::g_fonts.body, theme::g_fonts.body->FontSize, ImVec2(b0.x + bw + 12 * g_s, c.y + (h - theme::g_fonts.body->FontSize) * 0.5f), theme::Text(), path.c_str());
        dl->PopClipRect();
        const char* stt = st.installed ? (st.outdated ? "Update available" : "VR installed") : "VR not installed";
        ImU32 sc = st.installed ? (st.outdated ? theme::Warn() : theme::Good()) : theme::Muted();
        float sw = ImGui::CalcTextSize(stt).x; ImVec2 sp(c.x + w - sw - 16 * g_s, c.y + (h - ImGui::GetTextLineHeight()) * 0.5f);
        dl->AddCircleFilled(ImVec2(sp.x - 10 * g_s, sp.y + ImGui::GetTextLineHeight() * 0.55f), 3.5f * g_s, sc, 12); dl->AddText(sp, sc, stt);
        ImGui::PopID();
    }
    ImGui::Dummy(ImVec2(1, 2 * g_s));
    if (theme::Button("Browse...", false, !g_app.busy)) BrowseForGame(hwnd);
    ImGui::SameLine(); if (theme::Button("Scan again", false, !g_app.busy)) { Rescan(); Notice(g_app.games.empty() ? "No game folder found." : "Found " + std::to_string(g_app.games.size()) + " game folder(s).", theme::Muted()); }
    if (g_app.game.valid) { ImGui::SameLine(); if (theme::Button("Open game folder")) ShellExecuteW(nullptr, L"open", g_app.game.root.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    theme::EndCard();
    // install
    if (g_app.game.valid) {
        theme::SectionTitle("VR MOD");
        theme::BeginCard("mod");
        const State& s = g_app.state;
        if (s.installed) {
            theme::Dot(s.outdated ? theme::Warn() : theme::Good());
            std::string t = "Installed in this game folder" + (s.installedVersion.empty() ? std::string(".") : " (version " + s.installedVersion + ").");
            if (s.outdated) t += " The files differ from this launcher's: Reinstall to update (your settings are kept).";
            theme::Wrapped(t.c_str(), theme::Text());
        } else { theme::Dot(theme::Muted()); theme::Wrapped("Not installed in this game folder. Install adds the VR files next to SDHDShip.exe; the game's own files are not changed.", theme::Text()); }
        if (s.foreignD3d9) { theme::Dot(theme::Warn()); theme::Wrapped("Another dinput8.dll (e.g. an ASI loader) is in the game folder. Installing keeps it as a backup and puts it back on uninstall.", theme::Text()); }
        if (s.needsElevation) { theme::Dot(theme::Warn()); theme::Wrapped("The game folder needs administrator rights to change; Windows will ask once.", theme::Text()); }
        if (g_app.running) { theme::Dot(theme::Warn()); theme::Wrapped("The game is running. Close it to install or remove the mod.", theme::Text()); }
        ImGui::Dummy(ImVec2(1, 2 * g_s));
        bool can = !g_app.busy && !g_app.running;
        if (theme::Button(s.installed ? "Reinstall / update" : "Install VR mod", true, can)) { g_app.confirmUninstall = false; StartOp("install", false); }
        if (s.installed) { ImGui::SameLine(); if (theme::Button("Uninstall", false, can)) g_app.confirmUninstall = !g_app.confirmUninstall; }
        if (g_app.confirmUninstall && s.installed) {
            ImGui::Dummy(ImVec2(1, 2));
            theme::Wrapped("Remove Sleeping Dogs VR from this game folder? The game itself and your save games are not touched.", theme::Text());
            ImGui::Checkbox("Also delete my VR settings (sdvr.ini, SPatch.ini)", &g_app.deleteIni);
            if (theme::Button("Yes, uninstall", true, can)) { g_app.confirmUninstall = false; StartOp("uninstall", g_app.deleteIni); }
            ImGui::SameLine(); if (theme::Button("Cancel")) g_app.confirmUninstall = false;
        }
        if (g_app.haveReport) {
            ImGui::Dummy(ImVec2(1, 4 * g_s));
            for (auto& l : g_app.report.lines) if (l.size() > 2) { theme::Dot(l[0] == '+' ? theme::Good() : l[0] == '!' ? theme::Warn() : theme::Bad()); theme::Wrapped(l.c_str() + 2, theme::Text()); }
        }
        theme::EndCard();
    }
    // runtime
    theme::SectionTitle("VR RUNTIME");
    theme::BeginCard("runtime");
    const Runtime& rt = g_app.runtime;
    if (rt.found) {
        theme::Dot(theme::Good()); ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted(rt.name.c_str()); ImGui::PopFont();
        theme::Wrapped("The active OpenXR runtime. The game will start in this runtime; switch it in the runtime's own app (Virtual Desktop, SteamVR, Meta Quest Link...).", theme::Muted());
        ImGui::PushFont(theme::g_fonts.small); theme::Wrapped(rt.manifest.c_str(), IM_COL32(104, 110, 128, 255)); ImGui::PopFont();
    } else {
        theme::Dot(theme::Bad()); ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted(rt.manifest.empty() ? "No OpenXR runtime is active" : "The active OpenXR runtime is missing"); ImGui::PopFont();
        theme::Wrapped("Start your headset's PC app (Virtual Desktop, SteamVR or Meta Quest Link) and set it as the OpenXR runtime, then press Check again.", theme::Muted());
    }
    if (theme::Button("Check again")) { g_app.runtime = ActiveRuntime(); }
    theme::EndCard();
    // shortcut
    theme::SectionTitle("DESKTOP SHORTCUT");
    theme::BeginCard("shortcut");
    if (theme::Button("Create desktop shortcut")) DesktopShortcut();
    ImGui::SameLine(); ImGui::AlignTextToFramePadding(); theme::Wrapped("A shortcut to this launcher on your desktop.", theme::Muted());
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 10 * g_s));
    ReadyCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    (void)W;
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- Settings
static void RenderSizeRow() {
    ImGui::PushID("rsize");
    float ctrlW = 300 * g_s, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x, y0 = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(!Installed());
    RowLabel("Picture size per eye", "The size the headset picture is drawn at (with DLSS: the size DLSS rebuilds it to). Larger is sharper and slower. The names match Virtual Desktop's quality levels.", ctrlW);
    float y1 = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + avail - ctrlW, y0)); ImGui::SetNextItemWidth(ctrlW);
    int w = _wtoi(Get(L"RenderWidth", L"3072").c_str()), h = _wtoi(Get(L"RenderHeight", L"3264").c_str()), cur = -1;
    for (int k = 0; k < kRenderN; ++k) if (kRender[k].w == w && kRender[k].h == h) cur = k;
    char custom[64]; snprintf(custom, sizeof custom, "%d x %d  (custom)", w, h);
    if (ImGui::BeginCombo("##c", cur >= 0 ? kRender[cur].label : custom)) {
        for (int k = 0; k < kRenderN; ++k) if (ImGui::Selectable(kRender[k].label, k == cur)) { Set(L"RenderWidth", std::to_wstring(kRender[k].w)); Set(L"RenderHeight", std::to_wstring(kRender[k].h)); }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(std::max(y1, ImGui::GetCursorPosY()) + 6 * g_s);
    ImGui::PopID();
}
// the mod stores the factor it multiplies eye distance and head movement by; the player sees its inverse, "world size"
static void WorldScaleRow() {
    ImGui::PushID("wscale");
    float ctrlW = 300 * g_s, x0 = ImGui::GetCursorPosX(), avail = ImGui::GetContentRegionAvail().x, y0 = ImGui::GetCursorPosY();
    ImGui::BeginDisabled(!Installed());
    RowLabel("World scale", "How large the world feels. 1.00 is the game's own scale; higher makes everything look bigger, lower smaller. Also in the in-game VR menu (Insert).", ctrlW);
    float y1 = ImGui::GetCursorPosY();
    ImGui::SetCursorPos(ImVec2(x0 + avail - ctrlW, y0)); ImGui::SetNextItemWidth(ctrlW);
    ImGuiStorage* stg = ImGui::GetStateStorage(); ImGuiID vid = ImGui::GetID("##val"), aid = ImGui::GetID("##act");
    float& d = *stg->GetFloatRef(vid, 1.0f); bool& dragging = *stg->GetBoolRef(aid, false);
    if (!dragging) { float f = (float)_wtof(Get(L"WorldScale", L"1.0").c_str()); d = f > 0.01f ? 1.0f / f : 1.0f; d = std::round(d * 20.0f) / 20.0f; }
    ImGui::SliderFloat("##s", &d, 0.5f, 2.0f, "%.2f");
    dragging = ImGui::IsItemActive();
    if (ImGui::IsItemDeactivatedAfterEdit()) { d = std::round(d * 20.0f) / 20.0f; if (d < 0.5f) d = 0.5f; if (d > 2.0f) d = 2.0f; wchar_t b[32]; swprintf(b, 32, L"%.4f", 1.0f / d); Set(L"WorldScale", b); }
    ImGui::EndDisabled();
    ImGui::SetCursorPosY(std::max(y1, ImGui::GetCursorPosY()) + 6 * g_s);
    ImGui::PopID();
}
static void Gap() { ImGui::Dummy(ImVec2(1, 4 * g_s)); }
static void PageSettingsDraw() {
    theme::Banner("SETTINGS", "PICTURE  \xe2\x80\xa2  DLSS  \xe2\x80\xa2  HUD  \xe2\x80\xa2  VIEW");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    NeedInstall();
    g_sec = L"VR";
    bool dlss = Get(L"Dlss", L"0") != L"0";
    theme::SectionTitle("PICTURE");
    theme::BeginCard("pic");
    RenderSizeRow();
    {   // DLSS on also needs the game's own anti-aliasing off: written to the game's DisplaySettings.xml by "Fix game settings"
        bool was = dlss;
        ComboRow("Upscaler", "NVIDIA DLSS needs an RTX graphics card. Off draws the picture at its full size with no anti-aliasing from the mod.", L"Dlss", L"0", kUpscaler, 2, Installed());
        dlss = Get(L"Dlss", L"0") != L"0";
        if (dlss != was) {
            g_app.cfg = CheckGameConfig(g_app.game);
            if (dlss && g_app.cfg.found && !g_app.cfg.issues.empty() && !g_app.running) { std::string err; if (FixGameConfig(g_app.game, err)) { Notice("Saved. The game's own anti-aliasing was switched off for DLSS.", theme::Good()); g_app.cfg = CheckGameConfig(g_app.game); } }
        }
    }
    ComboRow("DLSS mode", nullptr, L"DlssQuality", L"0", kQuality, 5, Installed() && dlss);
    if (dlss) {
        int q = _wtoi(Get(L"DlssQuality", L"0").c_str());
        int w = _wtoi(Get(L"RenderWidth", L"3072").c_str()), h = _wtoi(Get(L"RenderHeight", L"3264").c_str()); float r = QualityRatio(q);
        char t[240];
        if (r == 1.0f) snprintf(t, sizeof t, "DLAA draws each eye at %d x %d and smooths the edges: the sharpest picture.", w, h);
        else snprintf(t, sizeof t, "Draws each eye at %d x %d (%.0f%% of the pixels) and DLSS rebuilds it to %d x %d. Lower modes run faster and look softer.", (int)(w * r), (int)(h * r), r * r * 100, w, h);
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - 324 * g_s); theme::Wrapped(t, theme::Muted()); ImGui::PopTextWrapPos();
        Gap();
    }
    ComboRow("DLSS preset", "The DLSS model. K is the tested default.", L"DlssPreset", L"11", kPreset, 5, Installed() && dlss);
    SliderRow("Sharpening", "Contrast-adaptive sharpening of the headset picture, with or without DLSS. 0 is off. Also in the in-game VR menu.", L"Sharpen", L"0", 0.0f, 1.0f, "%.2f", 1, Installed());
    theme::EndCard();

    theme::SectionTitle("HUD PANEL");
    theme::BeginCard("hud");
    ComboRow("HUD", "Health, radar and prompts are shown on a panel floating in front of you. F11 changes this in game.", L"HudMode", L"0", kHudMode, 3, Installed());
    SliderRow("HUD distance", "How far away the panel floats.", L"HudDistance", L"1.4", 0.8f, 3.0f, "%.2f m", 1, Installed());
    SliderRow("HUD size", "Size of the gameplay HUD on the panel. It grows and shrinks around the minimap.", L"HudScale", L"1.0", 0.6f, 1.5f, "%.0f%%", 100, Installed());
    theme::EndCard();

    theme::SectionTitle("VIEW");
    theme::BeginCard("view");
    WorldScaleRow();
    ToggleKey("First person view (Experimental)", "The camera sits at Wei's head instead of behind him, also in cars and on motorcycles. There to try what it feels like; cutscenes keep the game's camera.", L"FirstPerson", L"0");
    Gap();
    ToggleKey("Recentre automatically at start", "Recentres the view during the first seconds after the game starts, so the logos and menus are in front of you.", L"AutoRecenter", L"1");
    Gap();
    ComboRow("Monitor window shows", "Which eye the game's window on your monitor shows. F7 changes this in game.", L"Mirror", L"0", kMirror, 3, Installed());
    theme::EndCard();

    theme::SectionTitle("IN-GAME VR MENU");
    theme::BeginCard("vrmenu");
    {
        theme::Dot(theme::Warn());
        ImGui::PushFont(theme::g_fonts.capsLg);
        theme::Wrapped("Press Insert on the keyboard while playing to open the VR menu in the headset.", theme::Warn());
        ImGui::PopFont();
        theme::Wrapped("Everything in it applies at once and is saved, so you rarely need to come back to this launcher. While the menu is open the arrow keys only steer the menu; the game does not see them.", theme::Muted());
        ImGui::Dummy(ImVec2(1, 6 * g_s));
    }
    ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted("How to use it"); ImGui::PopFont();
    theme::Bullet("Insert: open or close the menu");
    theme::Bullet("Up / Down arrows: choose a line");
    theme::Bullet("Left / Right arrows: change the chosen setting (Right also starts an action such as Recenter view)");
    ImGui::Dummy(ImVec2(1, 6 * g_s));
    ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted("What each line does"); ImGui::PopFont();
    theme::Bullet("DLSS: NVIDIA DLSS on or off (RTX cards)");
    theme::Bullet("DLSS mode: DLAA (full size), Ultra Quality, Quality, Balanced or Performance. Takes effect the next time the game starts");
    theme::Bullet("Sharpening: off, or 0.05 to 1.00. Makes the picture crisper");
    theme::Bullet("Bloom: the glow around bright lights, on or off");
    theme::Bullet("HUD panel: fixed in front of you, following your head, or off");
    theme::Bullet("HUD distance: how far away the HUD panel sits, 0.5 to 5 metres");
    theme::Bullet("HUD scale: how large the HUD elements are");
    theme::Bullet("World scale: how large the world feels, 0.50 to 2.00 (1.00 is the game's own scale)");
    theme::Bullet("First person: view from Wei's eyes instead of from behind him (experimental)");
    theme::Bullet("Monitor window: which eye the game's window on your monitor shows");
    theme::Bullet("Gamepad shortcuts: opens the page described below");
    theme::Bullet("Recenter view: press Right to put the view straight ahead of where you are looking");
    ImGui::Dummy(ImVec2(1, 6 * g_s));
    ImGui::PushFont(theme::g_fonts.bold); ImGui::TextUnformatted("Gamepad shortcuts: choose your own buttons"); ImGui::PopFont();
    theme::Wrapped("Two shortcuts work from the gamepad: first person on / off, and recentre. Each is one or two buttons held together for a moment. Out of the box they are LB + B (first person) and LB + Y (recentre).", theme::Muted());
    theme::Bullet("Open the VR menu, choose Gamepad shortcuts and press Right");
    theme::Bullet("Choose Button 1 or Button 2 of a shortcut and press Right, then press the button you want on the gamepad (you have 10 seconds; Left cancels)");
    theme::Bullet("Left on a button line sets it to None, so a shortcut can be a single button");
    theme::Bullet("Hold time: how long the buttons must be held, 0.2 to 2.0 seconds");
    theme::Bullet("Back returns to the main menu; Insert closes it");
    ImGui::Dummy(ImVec2(1, 4 * g_s));
    theme::Wrapped("The game still receives the buttons you choose. With the standard buttons, Y gets you out of a vehicle and B is the brake, so use those shortcuts on foot, or pick buttons that suit how you play. Triggers and stick clicks can be chosen too. An XInput gamepad is needed.", theme::Muted());
    ImGui::Dummy(ImVec2(1, 6 * g_s));
    ImGui::PushFont(theme::g_fonts.small); theme::Wrapped("Other keys in game: F8 recentres, F11 changes the HUD mode, F7 the monitor window eye.", theme::Muted()); ImGui::PopFont();
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- Optional
static void PageOptionalDraw() {
    theme::Banner("OPTIONAL", "EFFECTS  \xe2\x80\xa2  SPATCH FIXES");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    NeedInstall();
    g_sec = L"VR";
    theme::SectionTitle("SCREEN EFFECTS");
    theme::Wrapped("Effects the flat game draws \"on the camera\". They are off by default because in VR they sit on your face rather than in the world.", theme::Muted());
    Gap();
    theme::BeginCard("fx");
    ToggleKey("Depth of field and speed blur", "The game's blur of distant things and its blur at speed. Off keeps the whole picture sharp.", L"HideBlur", L"1", true);
    Gap();
    ToggleKey("Red screen edges when hurt", "The red vignette at low health.", L"HideHurtOverlay", L"1", true);
    Gap();
    ToggleKey("Water drops on the lens", "Drops on the \"camera lens\" in boats and rain.", L"HideScreenDrops", L"1", true);
    Gap();
    ToggleKey("Bloom", "The glow around bright lights and neon. Also in the in-game VR menu.", L"Bloom", L"1");
    theme::EndCard();

    theme::SectionTitle("VR FIXES");
    theme::BeginCard("fix");
    ToggleKey("Shadows all round you", "Sun shadows cover their full distance in every direction, so distant shadows don't switch off as you turn your head. Recommended.", L"ShadowRadial", L"1");
    Gap();
    ToggleKey("Driving mist in both eyes", "The mist around the car while driving is drawn in both eyes (the game only prepares it for one). Recommended.", L"MistFix", L"1");
    theme::EndCard();

    g_sec = L"SPatch";
    theme::SectionTitle("SPATCH BY XOWNY");
    theme::Wrapped("SPatch is a fix and restoration patch for Sleeping Dogs by xowny, built into the VR mod with his permission. Each switch turns a whole group on; the single features of a group can be set in SPatch.ini in the game folder.", theme::Muted());
    Gap();
    theme::BeginCard("spatch");
    bool sp = Get(L"Enabled", L"1") != L"0";
    ToggleKey("SPatch", "Off turns every SPatch feature off.", L"Enabled", L"1");
    Gap();
    ToggleKey("Stability fixes", "Crash and file-safety fixes. Recommended.", L"Stability", L"1", false, sp);
    Gap();
    ToggleKey("High frame rate timing", "Correct crowd density and camera / vehicle behaviour at high frame rates.", L"Timing", L"0", false, sp);
    Gap();
    ToggleKey("Restored character detail", "Restored eye detail and wetness on characters.", L"Content", L"0", false, sp);
    Gap();
    ToggleKey("Input and camera tweaks", "Raw mouse input, camera smoothing off, stick deadzones.", L"Features", L"0", false, sp);
    Gap();
    ToggleKey("Graphics restorations", "Original fog and neon glow, larger reflections, better texture filtering and shadow filter.", L"Graphics", L"0", false, sp);
    Gap();
    if (theme::Button("SPatch on Nexus Mods")) ShellExecuteW(nullptr, L"open", L"https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine(); if (theme::Button("SPatch on GitHub")) ShellExecuteW(nullptr, L"open", L"https://github.com/xowny/SPatch", nullptr, nullptr, SW_SHOWNORMAL);
    theme::EndCard();
    g_sec = L"VR";
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- About
static void PageAboutDraw() {
    theme::Banner("ABOUT", "CREDITS  \xe2\x80\xa2  HELP");
    ImGui::Indent(34 * g_s); ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - 34 * g_s);
    theme::SectionTitle("SLEEPING DOGS VR");
    theme::BeginCard("about");
    ImGui::PushFont(theme::g_fonts.bold); ImGui::Text("Sleeping Dogs VR %s", kModVersion); ImGui::PopFont();
    theme::Wrapped("A virtual reality mod for Sleeping Dogs: Definitive Edition by Braxus. The whole game in full stereo 3D with head tracking, best played seated with a gamepad.", theme::Muted());
    Gap();
    theme::Bullet("Full native stereo rendering of the game's third-person view, with head tracking and leaning");
    theme::Bullet("NVIDIA DLSS and DLAA, sharpening, and a HUD panel with the minimap and objective markers");
    theme::Bullet("Menus, the map and cutscene subtitles on a screen in front of you");
    theme::Bullet("Experimental first person view, world scale, and an in-game VR menu (Insert)");
    theme::Bullet("SPatch by xowny built in");
    theme::EndCard();
    theme::SectionTitle("NEED HELP?");
    theme::BeginCard("help");
    theme::Wrapped("If the game crashes or something doesn't look right, collect the logs and send the zip with your report. It gathers the mod's log, settings and a short system summary (Windows, CPU, GPU, VR runtime) into one zip on your desktop. Nothing is uploaded.", theme::Muted());
    if (theme::Button("Collect logs", true)) {
        std::string err; std::wstring z = CollectLogs(g_app.game, err);
        if (!z.empty()) { Notice("Saved to your desktop: " + Narrow(z.substr(z.find_last_of(L'\\') + 1)), theme::Good()); std::wstring arg = L"/select,\"" + z + L"\""; ShellExecuteW(nullptr, L"open", L"explorer.exe", arg.c_str(), nullptr, SW_SHOWNORMAL); }
        else Notice(err, theme::Bad());
    }
    ImGui::SameLine(); if (theme::Button("Open launcher log")) { std::wstring p = PrefsDir() + L"\\launcher.log"; ShellExecuteW(nullptr, L"open", p.c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    if (Installed()) { ImGui::SameLine(); if (theme::Button("Open sdvr.ini")) ShellExecuteW(nullptr, L"open", IniPath(g_app.game).c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    theme::EndCard();
    theme::SectionTitle("CREDITS");
    theme::BeginCard("credits");
    theme::Bullet("Sleeping Dogs VR by Braxus. Free software under the GNU GPL v3; source at github.com/Braxusx/SleepingDogsVR.");
    theme::Bullet("SPatch by xowny (GNU GPL v3), built into the mod as sdvr_spatch.dll with the author's permission. Thank you, xowny.");
    if (theme::Button("SPatch on Nexus Mods")) ShellExecuteW(nullptr, L"open", L"https://www.nexusmods.com/sleepingdogsdefinitiveedition/mods/148", nullptr, nullptr, SW_SHOWNORMAL);
    ImGui::SameLine(); if (theme::Button("SPatch on GitHub")) ShellExecuteW(nullptr, L"open", L"https://github.com/xowny/SPatch", nullptr, nullptr, SW_SHOWNORMAL);
    if (g_app.game.valid && g_app.state.installed) { ImGui::SameLine(); if (theme::Button("Licences and notices")) ShellExecuteW(nullptr, L"open", (g_app.game.root + L"\\sdvr_licenses").c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
    Gap();
    theme::Bullet("sdvr_spatch.dll is free software under the GPL v3; its source code, with the VR mod's changes, is in the mod's GitHub repository. The licence text is installed in the game's sdvr_licenses folder.");
    theme::Bullet("NVIDIA DLSS \xc2\xa9 NVIDIA Corporation: proprietary, not part of the mod's open-source licence; shipped under the NVIDIA RTX SDKs licence (copy in sdvr_licenses). MinHook by Tsuda Kageyu (BSD). OpenXR by the Khronos Group (Apache 2.0).");
    theme::Bullet("Launcher: built on the Far Cry 2 VR launcher, whose design follows Pizza Parker's Dishonored VR launcher (with permission). Dear ImGui by Omar Cornut (MIT), LZMA SDK by Igor Pavlov, miniz by Rich Geldreich (MIT).");
    Gap();
    theme::Wrapped("Sleeping Dogs is a trademark of Square Enix. This is an unofficial fan mod, not affiliated with or endorsed by Square Enix or United Front Games. The Sleeping Dogs name and logo belong to Square Enix.", IM_COL32(104, 110, 128, 255));
    theme::EndCard();
    ImGui::Dummy(ImVec2(1, 24 * g_s));
    ImGui::PopTextWrapPos(); ImGui::Unindent(34 * g_s);
}

// ---------------------------------------------------------------- frame
static void DrawUI(HWND hwnd) {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->Pos); ImGui::SetNextWindowSize(vp->Size);
    ImGui::Begin("##root", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float sideW = 214 * g_s, H = vp->Size.y;
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + vp->Size.x, vp->Pos.y + H), theme::Bg());
    // sidebar
    dl->AddRectFilled(vp->Pos, ImVec2(vp->Pos.x + sideW, vp->Pos.y + H), theme::Side());
    dl->AddLine(ImVec2(vp->Pos.x + sideW, vp->Pos.y), ImVec2(vp->Pos.x + sideW, vp->Pos.y + H), IM_COL32(32, 36, 52, 255), 1.0f);
    ImGui::SetCursorPos(ImVec2(0, 0));
    ImGui::BeginChild("##side", ImVec2(sideW, H), 0, ImGuiWindowFlags_NoScrollbar);
    ImGui::Dummy(ImVec2(1, 22 * g_s));
    theme::Emblem(76 * g_s);
    {   // the name under the emblem, on two centred lines (too wide for the sidebar on one)
        ImFont* f = theme::g_fonts.caps; float sp = 1.4f * g_s;
        ImGui::Dummy(ImVec2(1, 8 * g_s));
        for (const char* t : { "SLEEPING DOGS", "VR" }) {
            float tw = theme::SpacedCapsWidth(f, t, sp); ImVec2 c = ImGui::GetCursorScreenPos();
            theme::SpacedCaps(ImGui::GetWindowDrawList(), f, ImVec2(c.x + (sideW - tw) * 0.5f, c.y), theme::Gold(), t, sp); ImGui::Dummy(ImVec2(1, f->FontSize + 2 * g_s));
        }
        ImGui::Dummy(ImVec2(1, 16 * g_s)); }
    static const char* names[] = { "Main Menu", "Settings", "Optional", "About" };
    for (int i = 0; i < 4; ++i) if (theme::NavItem(names[i], i == 0 ? 0 : i == 1 ? 1 : i == 2 ? 2 : 3, g_app.page == i)) { if (g_app.page != i) g_app.scrollReset = 0; g_app.page = (Page)i; }
    {   // bottom: the selected game, like the concept's account chip
        float y = H - 64 * g_s; ImGui::SetCursorPosY(y);
        ImVec2 c = ImGui::GetCursorScreenPos(); ImDrawList* sd = ImGui::GetWindowDrawList();
        sd->AddLine(ImVec2(c.x + 16 * g_s, c.y - 10 * g_s), ImVec2(c.x + sideW - 16 * g_s, c.y - 10 * g_s), IM_COL32(32, 36, 52, 255), 1.0f);
        ImVec2 m(c.x + 34 * g_s, c.y + 20 * g_s);
        bool ok = g_app.game.valid && g_app.state.installed;
        sd->AddCircleFilled(m, 14 * g_s, ok ? IM_COL32(176, 36, 60, 255) : IM_COL32(52, 56, 76, 255), 24);
        ImFont* lf = theme::g_fonts.label; const char* ini = g_app.game.store == StoreGog ? "G" : g_app.game.store == StoreSteam ? "S" : "?";
        ImVec2 isz = lf->CalcTextSizeA(lf->FontSize, 1e9f, 0, ini); sd->AddText(lf, lf->FontSize, ImVec2(m.x - isz.x * 0.5f, m.y - isz.y * 0.5f), IM_COL32(255, 240, 244, 255), ini);
        std::string l1 = g_app.game.valid ? std::string(StoreName(g_app.game.store)) + " edition" : "No game selected";
        sd->AddText(theme::g_fonts.bold, theme::g_fonts.bold->FontSize * 0.92f, ImVec2(m.x + 24 * g_s, m.y - 17 * g_s), theme::Text(), l1.c_str());
        sd->AddText(theme::g_fonts.small, theme::g_fonts.small->FontSize, ImVec2(m.x + 24 * g_s, m.y + 2 * g_s), ok ? theme::Good() : theme::Muted(), !g_app.game.valid ? "Choose a folder" : ok ? (g_app.running ? "Game running" : "VR installed") : "VR not installed");
    }
    ImGui::EndChild();
    // page
    ImGui::SetCursorPos(ImVec2(sideW + 1, 0));
    ImGui::BeginChild("##page", ImVec2(vp->Size.x - sideW - 1, H), 0, 0);
    if (g_app.scrollReset >= 0) { ImGui::SetScrollY(0); g_app.scrollReset = -1; }
    if (g_app.busy) {
        theme::Banner("PLEASE WAIT", nullptr);
        ImGui::Indent(34 * g_s); ImGui::TextUnformatted(g_app.busyText.c_str());
        float t = (float)ImGui::GetTime(); ImVec2 c = ImGui::GetCursorScreenPos(); float r = 14 * g_s;
        ImGui::GetWindowDrawList()->PathArcTo(ImVec2(c.x + r, c.y + r + 8 * g_s), r, t * 5, t * 5 + 4.2f, 28); ImGui::GetWindowDrawList()->PathStroke(theme::Gold(), 0, 3 * g_s);
        ImGui::Unindent(34 * g_s); g_framesPending = 2;
    } else switch (g_app.page) {
        case PageMain: PageMainDraw(hwnd); break;
        case PageSettings: PageSettingsDraw(); break;
        case PageOptional: PageOptionalDraw(); break;
        case PageAbout: PageAboutDraw(); break;
    }
    ImGui::EndChild();
    // notice toast (bottom right of the page, fades after a few seconds)
    if (!g_app.notice.empty()) {
        double age = ImGui::GetTime() - g_app.noticeAt; float a = age < 5.0 ? 1.0f : age < 6.0 ? (float)(6.0 - age) : 0.0f;
        if (a <= 0) g_app.notice.clear();
        else {
            ImDrawList* fg = ImGui::GetForegroundDrawList(); ImFont* f = theme::g_fonts.body;
            float maxW = std::min(520 * g_s, vp->Size.x - sideW - 60 * g_s);
            ImVec2 ts = f->CalcTextSizeA(f->FontSize, 1e9f, maxW, g_app.notice.c_str());
            ImVec2 b(vp->Pos.x + vp->Size.x - 24 * g_s, vp->Pos.y + H - 22 * g_s), a0(b.x - ts.x - 40 * g_s, b.y - ts.y - 22 * g_s);
            fg->AddRectFilled(a0, b, IM_COL32(22, 25, 38, (int)(245 * a)), 8 * g_s); fg->AddRect(a0, b, IM_COL32(64, 70, 96, (int)(255 * a)), 8 * g_s);
            ImU32 col = g_app.noticeCol ? g_app.noticeCol : theme::Text();
            fg->AddCircleFilled(ImVec2(a0.x + 16 * g_s, a0.y + 11 * g_s + f->FontSize * 0.55f), 4 * g_s, (col & 0x00FFFFFF) | ((ImU32)(255 * a) << 24), 12);
            fg->AddText(f, f->FontSize, ImVec2(a0.x + 28 * g_s, a0.y + 11 * g_s), (theme::Text() & 0x00FFFFFF) | ((ImU32)(255 * a) << 24), g_app.notice.c_str(), nullptr, maxW);
            g_framesPending = 2;
        }
    }
    ImGui::End();
}

// ---------------------------------------------------------------- window
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) { g_framesPending = 3; return 1; }
    switch (msg) {
    case WM_SIZE: if (wp != SIZE_MINIMIZED) { g_resizeW = LOWORD(lp); g_resizeH = HIWORD(lp); } g_framesPending = 3; return 0;
    case WM_GETMINMAXINFO: { MINMAXINFO* m = (MINMAXINFO*)lp; m->ptMinTrackSize = { (LONG)(960 * g_scale), (LONG)(620 * g_scale) }; return 0; }
    case WM_DPICHANGED: { g_scale = HIWORD(wp) / 96.0f; g_fontsDirty = true; RECT* r = (RECT*)lp; SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top, SWP_NOZORDER | SWP_NOACTIVATE); return 0; }
    case WM_SYSCOMMAND: if ((wp & 0xfff0) == SC_KEYMENU) return 0; break;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    if (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) g_framesPending = 3;
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int show) {
    int argc = 0; wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; i < argc; ++i) if (!wcscmp(argv[i], L"--op")) { CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED); int r = ElevatedMain(argc, argv); CoUninitialize(); return r; }
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    Log("launcher %s started", kModVersion);
    HANDLE single = CreateMutexW(nullptr, TRUE, L"Local\\SleepingDogsVR.Launcher");
    if (GetLastError() == ERROR_ALREADY_EXISTS) { HWND o = FindWindowW(L"SleepingDogsVRLauncher", nullptr); if (o) { ShowWindow(o, SW_RESTORE); SetForegroundWindow(o); } return 0; }

    WNDCLASSEXW wc{ sizeof wc }; wc.style = CS_CLASSDC; wc.lpfnWndProc = WndProc; wc.hInstance = inst; wc.lpszClassName = L"SleepingDogsVRLauncher";
    wc.hIcon = LoadIconW(inst, MAKEINTRESOURCEW(IDI_APP)); wc.hIconSm = wc.hIcon; wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    RegisterClassExW(&wc);
    POINT pt; GetCursorPos(&pt); HMONITOR mon = MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY);
    MONITORINFO mi{ sizeof mi }; GetMonitorInfoW(mon, &mi);
    UINT dpi = GetDpiForSystem(); { typedef HRESULT (WINAPI *GDFM)(HMONITOR, int, UINT*, UINT*); auto f = (GDFM)GetProcAddress(LoadLibraryW(L"shcore.dll"), "GetDpiForMonitor"); UINT y; if (f) f(mon, 0, &dpi, &y); }
    g_scale = dpi / 96.0f;
    int ww = (int)(1180 * g_scale), wh = (int)(800 * g_scale);
    int workW = mi.rcWork.right - mi.rcWork.left, workH = mi.rcWork.bottom - mi.rcWork.top;
    if (ww > workW - 40) ww = workW - 40; if (wh > workH - 40) wh = workH - 40;
    RECT r{ 0, 0, ww, wh };
    AdjustWindowRectExForDpi(&r, WS_OVERLAPPEDWINDOW, FALSE, 0, dpi);
    int w = r.right - r.left, h = r.bottom - r.top;
    int x = mi.rcWork.left + (workW - w) / 2, y = mi.rcWork.top + (workH - h) / 2;
    HWND hwnd = CreateWindowExW(0, wc.lpszClassName, 
#ifdef SDVR_DIAGNOSTIC
        L"Sleeping Dogs VR - diagnostic build " L"" SDVR_VERSION,
#else
        L"Sleeping Dogs VR",
#endif
        WS_OVERLAPPEDWINDOW, x, y, w, h, nullptr, nullptr, inst, nullptr);
    { BOOL dark = TRUE; typedef HRESULT (WINAPI *DSWA)(HWND, DWORD, LPCVOID, DWORD); auto f = (DSWA)GetProcAddress(LoadLibraryW(L"dwmapi.dll"), "DwmSetWindowAttribute"); if (f) { f(hwnd, 20, &dark, sizeof dark); COLORREF cap = RGB(10, 11, 18); f(hwnd, 35, &cap, sizeof cap); } }
    if (!CreateDevice(hwnd)) { MessageBoxW(hwnd, L"Direct3D 11 isn't available on this PC, so the launcher can't open.", L"Sleeping Dogs VR", MB_ICONERROR); return 1; }
    ShowWindow(hwnd, show); UpdateWindow(hwnd);

    IMGUI_CHECKVERSION(); ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); io.IniFilename = nullptr; io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    theme::LoadFonts(g_scale); theme::ApplyStyle(g_scale);
    ImGui_ImplWin32_Init(hwnd); ImGui_ImplDX11_Init(g_dev, g_ctx);
    theme::g_banner = LoadImageRes(IDR_ART_BANNER); theme::g_emblem = LoadImageRes(IDR_ART_EMBLEM);

    Rescan(); g_app.running = GameRunning();
    double nextPoll = 0;
    bool done = false;
    while (!done) {
        DWORD wait = (g_framesPending > 0 || g_app.busy) ? 16 : 1000;
        MsgWaitForMultipleObjectsEx(0, nullptr, wait, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
        MSG msg; while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) { TranslateMessage(&msg); DispatchMessageW(&msg); if (msg.message == WM_QUIT) done = true; }
        if (done) break;
        if (IsIconic(hwnd)) continue;
        if (GetTickCount64() > nextPoll) { bool run = GameRunning(); if (run != g_app.running) { g_app.running = run; if (!run) Refresh(); g_framesPending = 3; } nextPoll = GetTickCount64() + 1000.0; }
        PollWorker();
        if (g_resizeW) { if (g_rtv) { g_rtv->Release(); g_rtv = nullptr; } g_swap->ResizeBuffers(0, g_resizeW, g_resizeH, DXGI_FORMAT_UNKNOWN, 0); g_resizeW = g_resizeH = 0; CreateRT(); }
        if (g_fontsDirty) { g_fontsDirty = false; ImGui_ImplDX11_InvalidateDeviceObjects(); theme::LoadFonts(g_scale); theme::ApplyStyle(g_scale); ImGui_ImplDX11_CreateDeviceObjects(); }
        if (g_framesPending <= 0 && !g_app.busy) continue;
        --g_framesPending;
        ImGui_ImplDX11_NewFrame(); ImGui_ImplWin32_NewFrame(); ImGui::NewFrame();
        DrawUI(hwnd);
        ImGui::Render();
        const float clear[4] = { 0.055f, 0.063f, 0.094f, 1 };
        g_ctx->OMSetRenderTargets(1, &g_rtv, nullptr); g_ctx->ClearRenderTargetView(g_rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        g_swap->Present(1, 0);
    }
    ImGui_ImplDX11_Shutdown(); ImGui_ImplWin32_Shutdown(); ImGui::DestroyContext();
    if (g_rtv) g_rtv->Release(); if (g_swap) g_swap->Release(); if (g_ctx) g_ctx->Release(); if (g_dev) g_dev->Release();
    DestroyWindow(hwnd); UnregisterClassW(wc.lpszClassName, inst);
    if (single) CloseHandle(single);
    CoUninitialize();
    return 0;
}
