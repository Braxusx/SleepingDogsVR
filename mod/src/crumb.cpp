// SDVR - last-state file (build 142)
// sdvr_state.txt next to sdvr.log: a few fixed lines, each overwritten in place through a memory-mapped view, saying what
// each thread was doing last (the OpenXR call it is inside, the frame, memory, the last exception). Writing costs no system
// call and the contents survive ANY way the process ends - also the ones that leave nothing in sdvr.log: a fail-fast abort
// inside another module, the process being ended from outside, a freeze the player closes with Task Manager.
// At start-up the previous run's sdvr.log and sdvr_state.txt are kept as sdvr_previous.log / sdvr_previous_state.txt, so
// starting the game again after a crash no longer wipes the evidence.
#include "crumb.h"
#include "log.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>

enum { LINE = 240 };
static char* g_map; static char g_prevRun[LINE + 1]; static bool g_hadPrev;
static const char* const kLabel[CR_N] = { "", "run", "OpenXR (render thread)", "OpenXR (other thread)", "render thread", "game thread", "last exception",
                                          "last C++ throw", "memory", "watchdog" };
static void Put(int slot, const char* text) {
    if (!g_map || slot < 0 || slot >= CR_N) return;
    char b[LINE + 1]; int n = (int)strlen(text); if (n > LINE - 2) n = LINE - 2;
    memcpy(b, text, n); memset(b + n, ' ', LINE - 2 - n); b[LINE - 2] = '\r'; b[LINE - 1] = '\n';
    memcpy(g_map + slot * LINE, b, LINE);
}
void Crumb(int slot, const char* fmt, ...) {
    if (!g_map) return;
    char t[LINE * 2]; ULONGLONG ms = LogMs();
    int n = snprintf(t, sizeof t, "[%7llu.%03llu] %-22s: ", ms / 1000, ms % 1000, kLabel[slot]);
    va_list ap; va_start(ap, fmt); vsnprintf(t + n, sizeof t - n, fmt, ap); va_end(ap);
    Put(slot, t);
}
void CrumbText(int slot, char* out, size_t cap) {
    out[0] = 0; if (!g_map || slot < 0 || slot >= CR_N || cap < 2) return;
    size_t n = LINE - 2; if (n > cap - 1) n = cap - 1;
    memcpy(out, g_map + slot * LINE, n); while (n && out[n - 1] == ' ') n--; out[n] = 0;
}
void CrumbInit(const wchar_t* dir) {
    wchar_t st[MAX_PATH], pst[MAX_PATH], lg[MAX_PATH], plg[MAX_PATH];
    swprintf(st, MAX_PATH, L"%ls\\sdvr_state.txt", dir); swprintf(pst, MAX_PATH, L"%ls\\sdvr_previous_state.txt", dir);
    swprintf(lg, MAX_PATH, L"%ls\\sdvr.log", dir); swprintf(plg, MAX_PATH, L"%ls\\sdvr_previous.log", dir);
    {   // how did the previous run end? (its "run" line)
        HANDLE f = CreateFileW(st, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        if (f != INVALID_HANDLE_VALUE) {
            char b[LINE * 2] = ""; DWORD got = 0; ReadFile(f, b, sizeof b, &got, nullptr); CloseHandle(f);
            if (got >= LINE * 2) { memcpy(g_prevRun, b + CR_RUN * LINE, LINE - 2); g_prevRun[LINE - 2] = 0; for (int i = LINE - 3; i >= 0 && g_prevRun[i] == ' '; i--) g_prevRun[i] = 0; g_hadPrev = true; }
        }
    }
    MoveFileExW(st, pst, MOVEFILE_REPLACE_EXISTING); MoveFileExW(lg, plg, MOVEFILE_REPLACE_EXISTING);
    HANDLE f = CreateFileW(st, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    HANDLE m = CreateFileMappingW(f, nullptr, PAGE_READWRITE, 0, CR_N * LINE, nullptr);
    if (m) g_map = (char*)MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, CR_N * LINE);
    if (!g_map) return;   // the handles stay open for the life of the process on purpose
    for (int i = 0; i < CR_N; i++) Put(i, "");
}
void CrumbReportPrevious() {
    Put(CR_TITLE, "Sleeping Dogs VR - last state of each part of the mod. Lines are overwritten while the game runs; times are seconds as in sdvr.log.");
    Crumb(CR_RUN, "RUNNING (if the game is closed and this still says RUNNING, the process ended without a recorded exit)");
    if (!g_map) Log("state file: sdvr_state.txt could not be created");
    if (!g_hadPrev) return;
    if (strstr(g_prevRun, "RUNNING"))
        Log("previous run: ended WITHOUT a recorded exit (crash, freeze or ended from outside) - its files are kept as sdvr_previous.log and sdvr_previous_state.txt");
    else Log("previous run: %s", g_prevRun[0] ? g_prevRun : "(no run line)");
}
