// SDVR - last-state file (build 142). See crumb.cpp.
#pragma once
#include <windows.h>
enum { CR_TITLE, CR_RUN, CR_XR_RENDER, CR_XR_OTHER, CR_RENDER, CR_GAME, CR_EXC, CR_THROW, CR_MEM, CR_WATCH, CR_N };
void CrumbInit(const wchar_t* dir);                    // call BEFORE LogInit: keeps the previous run's log and state file
void CrumbReportPrevious();                            // after LogInit: says in the new log how the previous run ended
void Crumb(int slot, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void CrumbText(int slot, char* out, size_t cap);       // what a line holds now
// crashlog.cpp
void DiagBeat(int which);                              // 0 = render thread (each present), 1 = game thread (each frame)
void DiagStart(const wchar_t* ini);                    // watchdog thread
struct ID3D11Device; void DiagSetDevice(ID3D11Device* dev);
