// SDVR - crash notes in sdvr.log (build 096)
// A vectored handler writes one entry for each distinct place a serious exception (access violation, illegal instruction,
// divide by zero, stack overflow) is raised: module + offset of the faulting instruction, the address touched, and the
// call chain. Exceptions that some code then handles (guarded reads) get logged too, once per place, so after a crash the
// LAST entry in the log is the one that took the game down. At most 48 distinct places are written per run.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <MinHook.h>
#include <stdint.h>
#include <psapi.h>
#include <d3d11.h>
#include <dxgi1_4.h>
#include <cpuid.h>
#include "log.h"
#include "crumb.h"

static volatile LONG g_n; static void* g_seen[96]; static volatile LONG g_busy; static volatile LONG g_endLogged;
static void Where(void* addr, char* out, size_t cap) {
    HMODULE m = nullptr; char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m && GetModuleFileNameA(m, path, MAX_PATH)) {
        const char* n = strrchr(path, '\\'); n = n ? n + 1 : path;
        snprintf(out, cap, "%s+0x%llx", n, (unsigned long long)((uintptr_t)addr - (uintptr_t)m));
    } else snprintf(out, cap, "%p", addr);
}
static bool Rd(uintptr_t a, void* o, size_t n) { SIZE_T g = 0; return a > 0x10000 && ReadProcessMemory(GetCurrentProcess(), (void*)a, o, n, &g) && g == n; }
static void RdText(uintptr_t a, char* out, size_t cap) {   // a zero-ended string that may sit at the end of a page
    memset(out, 0, cap);
    for (size_t n = cap - 1; n >= 8; n /= 2) if (Rd(a, out, n)) break; else memset(out, 0, cap);
    out[cap - 1] = 0; for (char* c = out; *c; c++) if (*c < 32 || *c > 126) *c = '?';
}
// 142: what a C++ throw carries - the class name from the compiler's throw information (MSVC layout: ThrowInfo ->
// CatchableTypeArray -> CatchableType -> TypeDescriptor, 32-bit offsets from the throwing module's base) and, for classes
// derived from std::exception, its message. Everything is read with ReadProcessMemory, so a wrong layout only gives no text.
static void CxxDescribe(const EXCEPTION_RECORD* r, char* out, size_t cap) {
    out[0] = 0; if (r->NumberParameters < 4 || r->ExceptionInformation[0] < 0x19930520 || r->ExceptionInformation[0] > 0x19930522) return;
    uintptr_t obj = r->ExceptionInformation[1], ti = r->ExceptionInformation[2], base = r->ExceptionInformation[3];
    int32_t arr = 0, n = 0; if (!Rd(ti + 12, &arr, 4) || !arr || !Rd(base + arr, &n, 4) || n <= 0) return;
    if (n > 8) n = 8;
    bool stdexc = false; int o = 0;
    for (int i = 0; i < n; i++) {
        int32_t ct = 0, td = 0; if (!Rd(base + arr + 4 + 4 * i, &ct, 4) || !Rd(base + ct + 4, &td, 4)) break;
        char nm[96]; RdText(base + td + 16, nm, sizeof nm);
        if (strstr(nm, "exception@std")) stdexc = true;
        if (i < 2 && nm[0]) o += snprintf(out + o, cap - o, "%s%s", i ? " : " : "", nm);
    }
    if (stdexc) { uintptr_t w = 0; char msg[160]; if (Rd(obj + 8, &w, 8) && w) { RdText(w, msg, sizeof msg); if (msg[0] && o < (int)cap - 20) snprintf(out + o, cap - o, " - message '%s'", msg); } }
}
static LONG CALLBACK Veh(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    // 097: every error-class exception (0xC...), plus C++ throws (0xE06D7363) - those all share one raise address, so the
    // first 8 are written instead of one per place
    static volatile LONG cxx;
    bool isCxx = code == 0xE06D7363;
    if (!isCxx && (code & 0xF0000000) != 0xC0000000) return EXCEPTION_CONTINUE_SEARCH;
    void* ip = ep->ExceptionRecord->ExceptionAddress;
    // 134: the render thread's command executor sets the vertex buffer (+0xa1a199) / index buffer (+0xa1a21d) of a draw from a
    // buffer object in rbx: it reads the D3D wrapper at [rbx+0xb8] and then [wrapper+0x58]. When the object was freed and its
    // memory reused (seen once: GOG run, end of the prologue cutscene, wrapper = pixel data), that read faults and the game
    // ends. The command is carried on as the game itself does for "no buffer" (rbx == 0): r15 / r12 / r13 are still 0 here.
    if (code == EXCEPTION_ACCESS_VIOLATION) {
        static uintptr_t base; if (!base) base = (uintptr_t)GetModuleHandleW(nullptr);
        uintptr_t rva = (uintptr_t)ip - base; uintptr_t to = rva == 0xa1a1b9 ? 0xa1a1de : rva == 0xa1a241 ? 0xa1a26a : 0;
        if (to) {
            static volatile LONG rec; LONG k = InterlockedIncrement(&rec);
            if (k <= 20 || k % 1000 == 0) {
                char nm[40] = ""; SIZE_T got = 0; ReadProcessMemory(GetCurrentProcess(), (void*)(ep->ContextRecord->Rbx + 0x34), nm, sizeof nm - 1, &got);
                for (char& ch : nm) if (ch && (ch < 32 || ch > 126)) ch = '?';
                Log("RECOVERED %ld: draw with a freed %s buffer (object %p, wrapper %p, name '%s') - buffer left out, game continues", k, rva == 0xa1a1b9 ? "vertex" : "index",
                    (void*)ep->ContextRecord->Rbx, (void*)ep->ContextRecord->Rax, nm);
                LogFlush();
            }
            ep->ContextRecord->Rip = base + to; ep->ContextRecord->R15 = 0; ep->ContextRecord->R12 = 0; ep->ContextRecord->R13 = 0;
            return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    // 142: C++ throws are told apart by the class thrown (one entry per class, up to 40) instead of "the first 8"; the
    // state file always carries the latest one, also when its class was already written to the log
    char cxxText[420] = "";
    if (isCxx) {
        LONG k = InterlockedIncrement(&cxx);
        if (k <= 4000 || k % 64 == 0) {
            CxxDescribe(ep->ExceptionRecord, cxxText, sizeof cxxText);
            char w0[300] = ""; void* fr[14]; USHORT nf = RtlCaptureStackBackTrace(0, 14, fr, nullptr);   // first frame outside this handler / the system = who threw
            for (USHORT i = 0; i < nf; i++) { Where(fr[i], w0, sizeof w0); if (_strnicmp(w0, "ntdll", 5) && _strnicmp(w0, "KERNELBASE", 10) && _strnicmp(w0, "dinput8", 7) && _strnicmp(w0, "VCRUNTIME", 9)) break; }
            Crumb(CR_THROW, "number %ld: %s thrown from %s (thread %lu)", k, cxxText[0] ? cxxText : "(class not readable)", w0, GetCurrentThreadId());
        }
        static volatile LONG cxxLogged; if (cxxLogged >= 40) return EXCEPTION_CONTINUE_SEARCH;
        ip = ep->ExceptionRecord->NumberParameters >= 3 ? (void*)ep->ExceptionRecord->ExceptionInformation[2] : (void*)((uintptr_t)ip + (uintptr_t)k);
        LONG n0 = g_n; bool seen = false; for (LONG i = 0; i < n0 && i < 96; i++) if (g_seen[i] == ip) seen = true;
        if (!seen) InterlockedIncrement(&cxxLogged);
    }
    char w[300]; Where(isCxx ? ep->ExceptionRecord->ExceptionAddress : ip, w, sizeof w);
    if (!isCxx) {
        if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
            Crumb(CR_EXC, "access violation at %s, address %p (thread %lu)", w, (void*)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
        else Crumb(CR_EXC, "code 0x%08lx at %s (thread %lu)", code, w, GetCurrentThreadId());
    }
    LONG n = g_n; for (LONG i = 0; i < n && i < 96; i++) if (g_seen[i] == ip) return EXCEPTION_CONTINUE_SEARCH;
    if (n >= 96 || InterlockedExchange(&g_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_seen[n] = ip; g_n = n + 1;
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        Log("EXCEPTION %ld: access violation at %s, %s address %p (thread %lu)", n + 1, w, ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : "reading", (void*)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
    else if (isCxx) Log("EXCEPTION %ld: C++ throw of %s (thread %lu) - normal if the thrower catches it itself", n + 1, cxxText[0] ? cxxText : "(class not readable)", GetCurrentThreadId());
    else Log("EXCEPTION %ld: code 0x%08lx at %s (thread %lu)", n + 1, code, w, GetCurrentThreadId());
    if (code != EXCEPTION_STACK_OVERFLOW) {
        const CONTEXT* c0 = ep->ContextRecord;
        Log("  rax %016llx rbx %016llx rcx %016llx rdx %016llx", c0->Rax, c0->Rbx, c0->Rcx, c0->Rdx);
        Log("  rsi %016llx rdi %016llx r8  %016llx r9  %016llx", c0->Rsi, c0->Rdi, c0->R8, c0->R9);
        CONTEXT c = *c0; char line[900]; int o = 0;
        for (int f = 0; f < 14 && c.Rip; f++) {
            DWORD64 base = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
            if (!rf) { if (IsBadReadPtr((void*)c.Rsp, 8)) break; c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }   // leaf function
            else { void* hd = nullptr; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, nullptr); }
            if (!c.Rip) break;
            Where((void*)c.Rip, w, sizeof w); o += snprintf(line + o, sizeof line - o, "%s%s", f ? " < " : "", w);
            if (o > 780) break;
        }
        Log("  called from: %s", o ? line : "(no call chain)");
    }
    LogFlush();
    InterlockedExchange(&g_busy, 0);
    return EXCEPTION_CONTINUE_SEARCH;
}
// 097: who ends the process. A game that shuts itself down (its own fatal-error path, exit(), abort()) raises no exception;
// the call chain into ExitProcess / TerminateProcess says where the decision was made.
static void Chain(const char* what, UINT code) {
    void* fr[20]; USHORT n = RtlCaptureStackBackTrace(1, 20, fr, nullptr);
    char line[1200]; int o = 0; char w[300];
    for (USHORT i = 0; i < n && o < 1050; i++) { Where(fr[i], w, sizeof w); o += snprintf(line + o, sizeof line - o, "%s%s", i ? " < " : "", w); }
    Log("PROCESS END: %s (code 0x%x, thread %lu), called from: %s", what, code, GetCurrentThreadId(), line);
    { char x[220], r[220], g[220]; CrumbText(CR_XR_RENDER, x, sizeof x); CrumbText(CR_RENDER, r, sizeof r); CrumbText(CR_GAME, g, sizeof g);
      Log("  at that moment - %s", x); Log("  at that moment - %s", r); Log("  at that moment - %s", g); }
    if (!InterlockedExchange(&g_endLogged, 1)) { Where(n ? fr[0] : nullptr, w, sizeof w); Crumb(CR_RUN, "ENDED by %s, code 0x%x, called from %s (thread %lu)", what, code, w, GetCurrentThreadId()); }
    LogFlush();
}
static void (WINAPI *oExit)(UINT); static BOOL (WINAPI *oTerm)(HANDLE, UINT);
static void WINAPI hkExit(UINT code) { Chain("ExitProcess", code); oExit(code); }
static BOOL WINAPI hkTerm(HANDLE h, UINT code) { if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) Chain("TerminateProcess", code); return oTerm(h, code); }
static LONG (WINAPI *oUef)(EXCEPTION_POINTERS*);
static LONG WINAPI hkUef(EXCEPTION_POINTERS* ep) {
    char w[300]; Where(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr, w, sizeof w);
    Log("PROCESS END: unhandled exception 0x%08lx at %s (thread %lu)", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul, w, GetCurrentThreadId()); LogFlush();
    if (!InterlockedExchange(&g_endLogged, 1)) Crumb(CR_RUN, "ENDED by an unhandled exception 0x%08lx at %s (thread %lu)", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul, w, GetCurrentThreadId());
    return oUef(ep);
}
// 142: the other ways a process is ended on purpose. A fail-fast (RaiseFailFastException, used by Microsoft's error-handling
// library that many runtimes are built with), the C runtime's abort() / terminate(), and the system call every exit ends in.
// What still cannot be seen from inside: the __fastfail instruction, and another program ending this one - for those the
// state file (sdvr_state.txt) keeps saying RUNNING and shows what each thread was doing.
static void (WINAPI *oFailFast)(PEXCEPTION_RECORD, PCONTEXT, DWORD);
static void WINAPI hkFailFast(PEXCEPTION_RECORD r, PCONTEXT c, DWORD fl) {
    if (r) { char w[300]; Where(r->ExceptionAddress, w, sizeof w); Log("FAIL-FAST: exception code 0x%08lx at %s", r->ExceptionCode, w); }
    Chain("RaiseFailFastException", r ? r->ExceptionCode : 0); oFailFast(r, c, fl);
}
static void (__cdecl *oAbort)(); static void __cdecl hkAbort() { Chain("abort() of the C runtime", 3); oAbort(); }
static void (__cdecl *oTerminate)(); static void __cdecl hkTerminate() { Chain("terminate() of the C runtime (a C++ exception nobody caught - see the last C++ throw)", 0); oTerminate(); }
static void (__cdecl *oAbortM)(); static void __cdecl hkAbortM() { Chain("abort() of msvcrt", 3); oAbortM(); }
static LONG (NTAPI *oNtTerm)(HANDLE, LONG);
static LONG NTAPI hkNtTerm(HANDLE h, LONG st) {
    if ((h == nullptr || h == GetCurrentProcess()) && !g_endLogged) Chain("NtTerminateProcess (an exit that did not go through ExitProcess)", (UINT)st);
    return oNtTerm(h, st);
}
static bool Api(const wchar_t* dll, const char* name, void* hk, void** orig) {
    HMODULE m = GetModuleHandleW(dll); void* f = m ? (void*)GetProcAddress(m, name) : nullptr;
    return f && MH_CreateHook(f, hk, orig) == MH_OK && MH_EnableHook(f) == MH_OK;
}
void CrashLogInstall() {
    int ok = Api(L"kernelbase.dll", "ExitProcess", (void*)hkExit, (void**)&oExit) || Api(L"kernel32.dll", "ExitProcess", (void*)hkExit, (void**)&oExit);
    ok += Api(L"kernelbase.dll", "TerminateProcess", (void*)hkTerm, (void**)&oTerm) || Api(L"kernel32.dll", "TerminateProcess", (void*)hkTerm, (void**)&oTerm);
    ok += Api(L"kernelbase.dll", "UnhandledExceptionFilter", (void*)hkUef, (void**)&oUef) || Api(L"kernel32.dll", "UnhandledExceptionFilter", (void*)hkUef, (void**)&oUef);
    int ok2 = Api(L"kernelbase.dll", "RaiseFailFastException", (void*)hkFailFast, (void**)&oFailFast) || Api(L"kernel32.dll", "RaiseFailFastException", (void*)hkFailFast, (void**)&oFailFast);
    ok2 += Api(L"ucrtbase.dll", "abort", (void*)hkAbort, (void**)&oAbort);
    ok2 += Api(L"ucrtbase.dll", "terminate", (void*)hkTerminate, (void**)&oTerminate);
    ok2 += Api(L"msvcrt.dll", "abort", (void*)hkAbortM, (void**)&oAbortM);
    ok2 += Api(L"ntdll.dll", "NtTerminateProcess", (void*)hkNtTerm, (void**)&oNtTerm);
    Log("crash notes: process-end hooks %d of 3, abort / fail-fast hooks %d of 5", ok, ok2);
    AddVectoredExceptionHandler(1, Veh); Log("crash notes: on (an EXCEPTION entry is written for each distinct fault; after a crash the last one is the cause)"); }

// ---------------------------------------------------------------------------------------------------------------------
// 142: watchdog. A thread of its own that (1) notices when the render thread or the game thread stops finishing frames and
// writes what both were doing (their last OpenXR call and their call chains) - a freeze leaves no exception, (2) keeps a
// memory line (process, system, graphics card) in the state file and now and then in the log, (3) reports a lost graphics
// device, (4) writes the machine and the foreign modules loaded into the game once.
static volatile LONG64 g_beat[2]; static volatile DWORD g_beatTid[2];
void DiagBeat(int w) { g_beatTid[w] = GetCurrentThreadId(); InterlockedIncrement64(&g_beat[w]); }
static ID3D11Device* g_wdDev; static IDXGIAdapter3* g_wdAd3;
void DiagSetDevice(ID3D11Device* dev) {
    if (g_wdDev || !dev) return;
    dev->AddRef();
    IDXGIDevice* dd = nullptr; IDXGIAdapter* ad = nullptr;
    if (SUCCEEDED(dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dd)) && dd) {
        if (SUCCEEDED(dd->GetAdapter(&ad)) && ad) {
            DXGI_ADAPTER_DESC d = {}; ad->GetDesc(&d); char nm[160]; WideCharToMultiByte(CP_UTF8, 0, d.Description, -1, nm, sizeof nm, nullptr, nullptr);
            LARGE_INTEGER v = {}; ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &v);
            Log("graphics card: %s, %llu MB of its own memory, vendor 0x%04x, driver %u.%u.%u.%u", nm, (unsigned long long)(d.DedicatedVideoMemory >> 20), d.VendorId,
                HIWORD(v.HighPart), LOWORD(v.HighPart), HIWORD(v.LowPart), LOWORD(v.LowPart));
            IDXGIAdapter3* a3 = nullptr; if (SUCCEEDED(ad->QueryInterface(__uuidof(IDXGIAdapter3), (void**)&a3))) g_wdAd3 = a3;
            ad->Release();
        }
        dd->Release();
    }
    g_wdDev = dev;
}
static void ThreadChain(DWORD tid, char* line, size_t cap) {
    line[0] = 0; if (!tid) { snprintf(line, cap, "(thread not seen yet)"); return; }
    HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
    if (!h) { snprintf(line, cap, "(thread %lu cannot be opened - it has ended)", tid); return; }
    // the addresses are collected while the thread is held still (its stack cannot change under the unwinder); the module
    // names are looked up after it runs again, because that needs the loader's lock, which the thread itself may hold
    DWORD64 ips[18]; int n = 0; CONTEXT c; memset(&c, 0, sizeof c); c.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
    if (SuspendThread(h) != (DWORD)-1) {
        if (GetThreadContext(h, &c)) {
            ips[n++] = c.Rip;
            for (int f = 0; f < 17 && c.Rip; f++) {
                DWORD64 base = 0; PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(c.Rip, &base, nullptr);
                if (!rf) { if (IsBadReadPtr((void*)c.Rsp, 8)) break; c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; }
                else { void* hd = nullptr; DWORD64 ef = 0; RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, c.Rip, rf, &c, &hd, &ef, nullptr); }
                if (!c.Rip) break; ips[n++] = c.Rip;
            }
        }
        ResumeThread(h);
    }
    CloseHandle(h);
    int o = 0; char w[300];
    for (int i = 0; i < n && o < (int)cap - 320; i++) { Where((void*)ips[i], w, sizeof w); o += snprintf(line + o, cap - o, "%s%s", i ? " < " : "", w); }
    if (!n) snprintf(line, cap, "(no call chain)");
}
static void MemText(char* out, size_t cap, double* vramShare) {
    PROCESS_MEMORY_COUNTERS_EX pm = {}; pm.cb = sizeof pm; GetProcessMemoryInfo(GetCurrentProcess(), (PROCESS_MEMORY_COUNTERS*)&pm, sizeof pm);
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms);
    int o = snprintf(out, cap, "game process %llu MB (peak %llu), system memory %lu%% in use, %llu MB still free to commit",
                     (unsigned long long)(pm.PrivateUsage >> 20), (unsigned long long)(pm.PeakPagefileUsage >> 20), ms.dwMemoryLoad, (unsigned long long)(ms.ullAvailPageFile >> 20));
    *vramShare = 0;
    if (g_wdAd3) { DXGI_QUERY_VIDEO_MEMORY_INFO vi = {};
        if (SUCCEEDED(g_wdAd3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &vi)) && vi.Budget) {
            snprintf(out + o, cap - o, "; graphics memory used by the game %llu MB of %llu MB allowed", (unsigned long long)(vi.CurrentUsage >> 20), (unsigned long long)(vi.Budget >> 20));
            *vramShare = (double)vi.CurrentUsage / (double)vi.Budget; } }
}
static void Machine() {
    char cpu[64] = "(unknown)"; unsigned r[4];
    if (__get_cpuid_max(0x80000000, nullptr) >= 0x80000004) { for (unsigned i = 0; i < 3; i++) { __get_cpuid(0x80000002 + i, &r[0], &r[1], &r[2], &r[3]); memcpy(cpu + 16 * i, r, 16); } cpu[48] = 0; }
    const char* cp = cpu; while (*cp == ' ') cp++;
    OSVERSIONINFOW vi = {}; vi.dwOSVersionInfoSize = sizeof vi;
    if (auto rgv = (LONG(WINAPI*)(OSVERSIONINFOW*))GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) rgv(&vi);
    MEMORYSTATUSEX ms = {}; ms.dwLength = sizeof ms; GlobalMemoryStatusEx(&ms); SYSTEM_INFO si; GetSystemInfo(&si);
    Log("machine: Windows %lu.%lu build %lu, %s, %lu logical processors, %llu MB memory", vi.dwMajorVersion, vi.dwMinorVersion, vi.dwBuildNumber, cp, si.dwNumberOfProcessors,
        (unsigned long long)(ms.ullTotalPhys >> 20));
}
static void Modules() {   // what else is loaded into the game: overlays, capture tools, injectors - a frequent cause of crashes
    static HMODULE mods[1024]; DWORD need = 0; if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &need)) return;
    wchar_t win[MAX_PATH] = L"", exe[MAX_PATH] = L""; GetWindowsDirectoryW(win, MAX_PATH); GetModuleFileNameW(nullptr, exe, MAX_PATH);
    if (wchar_t* sl = wcsrchr(exe, L'\\')) *sl = 0;
    size_t nw = wcslen(win), ne = wcslen(exe); int n = (int)(need / sizeof(HMODULE)); if (n > 1024) n = 1024; int shown = 0;
    for (int i = 0; i < n && shown < 60; i++) {
        wchar_t p[MAX_PATH] = L""; if (!GetModuleFileNameW(mods[i], p, MAX_PATH)) continue;
        if (!_wcsnicmp(p, win, nw) || !_wcsnicmp(p, exe, ne)) continue;
        char a[MAX_PATH * 2]; WideCharToMultiByte(CP_UTF8, 0, p, -1, a, sizeof a, nullptr, nullptr);
        Log("module from outside Windows and the game folder: %s", a); shown++;
    }
    Log("modules: %d loaded, %d of them from outside Windows and the game folder", n, shown);
}
static const char* DevReason(HRESULT h) {
    switch ((unsigned)h) { case 0x887A0005: return "device removed"; case 0x887A0006: return "device hung (the driver stopped answering)"; case 0x887A0007: return "device reset";
        case 0x887A0020: return "driver internal error"; case 0x887A0001: return "invalid call"; case 0x8007000E: return "out of memory"; default: return "other"; }
}
static DWORD WINAPI Watchdog(LPVOID) {
    static const char* const nm[2] = { "render", "game" };
    static const double lim[2][3] = { { 4, 15, 60 }, { 8, 30, 120 } };
    LONG64 last[2] = { 0, 0 }; ULONGLONG since[2] = { GetTickCount64(), GetTickCount64() }; int stage[2] = { 0, 0 }; int reports[2] = { 0, 0 };
    ULONGLONG t0 = GetTickCount64(), nextMem = 0, nextMemLog = t0 + 20000; bool mods = false, lost = false, vramWarned = false;
    for (;;) {
        Sleep(500); ULONGLONG now = GetTickCount64();
        for (int w = 0; w < 2; w++) {
            LONG64 b = g_beat[w];
            if (b != last[w]) {
                if (stage[w]) { Log("STALL ended: the %s thread finished a frame again after %.1f s", nm[w], (now - since[w]) / 1000.0); Crumb(CR_WATCH, "%s thread running again after a %.1f s stall", nm[w], (now - since[w]) / 1000.0); LogFlush(); }
                last[w] = b; since[w] = now; stage[w] = 0; continue;
            }
            double idle = (now - since[w]) / 1000.0;
            if (!b || stage[w] >= 3 || idle < lim[w][stage[w]] || reports[w] >= 12) continue;
            stage[w]++; reports[w]++;
            char x[220], r[220], g[220], c0[1500], c1[1500];
            CrumbText(CR_XR_RENDER, x, sizeof x); CrumbText(CR_RENDER, r, sizeof r); CrumbText(CR_GAME, g, sizeof g);
            ThreadChain(g_beatTid[0], c0, sizeof c0); ThreadChain(g_beatTid[1], c1, sizeof c1);
            Log("STALL: the %s thread has not finished a frame for %.1f s%s", nm[w], idle, w ? " (normal while the game loads, as long as the render thread keeps going)" : "");
            Log("  %s", x); Log("  %s", r); Log("  %s", g);
            Log("  render thread %lu is in: %s", g_beatTid[0], c0);
            Log("  game thread %lu is in: %s", g_beatTid[1], c1);
            Crumb(CR_WATCH, "%s thread: no frame for %.1f s (details in sdvr.log)", nm[w], idle);
            LogFlush();
        }
        if (now >= nextMem) {
            nextMem = now + 2000; char m[400]; double share = 0; MemText(m, sizeof m, &share); Crumb(CR_MEM, "%s", m);
            if (now >= nextMemLog) { nextMemLog = now + 60000; Log("memory: %s", m); }
            if (share > 0.97 && !vramWarned) { vramWarned = true; Log("WARNING: graphics memory is full - %s", m); LogFlush(); }
            if (share < 0.85) vramWarned = false;
            if (g_wdDev && !lost) { HRESULT h = g_wdDev->GetDeviceRemovedReason(); if (h != S_OK) { lost = true; Log("GRAPHICS DEVICE LOST: reason 0x%08lx (%s) - %s", (unsigned long)h, DevReason(h), m); Crumb(CR_WATCH, "graphics device lost: 0x%08lx (%s)", (unsigned long)h, DevReason(h)); LogFlush(); } }
        }
        if (!mods && now - t0 > 15000) { mods = true; Modules(); }
    }
    return 0;
}
void DiagStart(const wchar_t*) {
    Machine();
    if (HANDLE t = CreateThread(nullptr, 0, Watchdog, nullptr, 0, nullptr)) CloseHandle(t);
}
