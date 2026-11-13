// SDVR - crash notes in sdvr.log (build 096)
// A vectored handler writes one entry for each distinct place a serious exception (access violation, illegal instruction,
// divide by zero, stack overflow) is raised: module + offset of the faulting instruction, the address touched, and the
// call chain. Exceptions that some code then handles (guarded reads) get logged too, once per place, so after a crash the
// LAST entry in the log is the one that took the game down. At most 48 distinct places are written per run.
#include <windows.h>
#include <stdio.h>
#include <string.h>
#include <MinHook.h>
#include "log.h"
void LogFlush();

static volatile LONG g_n; static void* g_seen[48]; static volatile LONG g_busy;
static void Where(void* addr, char* out, size_t cap) {
    HMODULE m = nullptr; char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)addr, &m) && m && GetModuleFileNameA(m, path, MAX_PATH)) {
        const char* n = strrchr(path, '\\'); n = n ? n + 1 : path;
        snprintf(out, cap, "%s+0x%llx", n, (unsigned long long)((uintptr_t)addr - (uintptr_t)m));
    } else snprintf(out, cap, "%p", addr);
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
    if (isCxx) { if (InterlockedIncrement(&cxx) > 8) return EXCEPTION_CONTINUE_SEARCH; ip = (void*)((uintptr_t)ip + (uintptr_t)cxx); }
    LONG n = g_n; for (LONG i = 0; i < n && i < 48; i++) if (g_seen[i] == ip) return EXCEPTION_CONTINUE_SEARCH;
    if (n >= 48 || InterlockedExchange(&g_busy, 1)) return EXCEPTION_CONTINUE_SEARCH;
    g_seen[n] = ip; g_n = n + 1;
    char w[300]; Where(ip, w, sizeof w);
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        Log("EXCEPTION %ld: access violation at %s, %s address %p (thread %lu)", n + 1, w, ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "writing" : ep->ExceptionRecord->ExceptionInformation[0] == 8 ? "executing" : "reading", (void*)ep->ExceptionRecord->ExceptionInformation[1], GetCurrentThreadId());
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
    LogFlush();
}
static void (WINAPI *oExit)(UINT); static BOOL (WINAPI *oTerm)(HANDLE, UINT);
static void WINAPI hkExit(UINT code) { Chain("ExitProcess", code); oExit(code); }
static BOOL WINAPI hkTerm(HANDLE h, UINT code) { if (h == GetCurrentProcess() || GetProcessId(h) == GetCurrentProcessId()) Chain("TerminateProcess", code); return oTerm(h, code); }
static LONG (WINAPI *oUef)(EXCEPTION_POINTERS*);
static LONG WINAPI hkUef(EXCEPTION_POINTERS* ep) {
    char w[300]; Where(ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionAddress : nullptr, w, sizeof w);
    Log("PROCESS END: unhandled exception 0x%08lx at %s (thread %lu)", ep && ep->ExceptionRecord ? ep->ExceptionRecord->ExceptionCode : 0ul, w, GetCurrentThreadId()); LogFlush();
    return oUef(ep);
}
static bool Api(const wchar_t* dll, const char* name, void* hk, void** orig) {
    HMODULE m = GetModuleHandleW(dll); void* f = m ? (void*)GetProcAddress(m, name) : nullptr;
    return f && MH_CreateHook(f, hk, orig) == MH_OK && MH_EnableHook(f) == MH_OK;
}
void CrashLogInstall() {
    int ok = Api(L"kernelbase.dll", "ExitProcess", (void*)hkExit, (void**)&oExit) || Api(L"kernel32.dll", "ExitProcess", (void*)hkExit, (void**)&oExit);
    ok += Api(L"kernelbase.dll", "TerminateProcess", (void*)hkTerm, (void**)&oTerm) || Api(L"kernel32.dll", "TerminateProcess", (void*)hkTerm, (void**)&oTerm);
    ok += Api(L"kernelbase.dll", "UnhandledExceptionFilter", (void*)hkUef, (void**)&oUef) || Api(L"kernel32.dll", "UnhandledExceptionFilter", (void*)hkUef, (void**)&oUef);
    Log("crash notes: process-end hooks %d of 3", ok);
    AddVectoredExceptionHandler(1, Veh); Log("crash notes: on (an EXCEPTION entry is written for each distinct fault; after a crash the last one is the cause)"); }
