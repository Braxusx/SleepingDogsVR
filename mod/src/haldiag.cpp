// SDVR build 020 - Scaleform HAL census: counts calls of every method of the D3D11 HAL vtable (0x1418c3d98), split into
// "inside an eye pass" and "elsewhere", with the last caller, to find BeginScene / BeginDisplay / render target calls.
#include <windows.h>
#include <stdint.h>
#include <string.h>
#include "MinHook.h"
#include <initializer_list>
#include "log.h"

extern uintptr_t g_base;
struct Slot { uint64_t lastCaller, count, tramp, inEye; };
static Slot* g_slots; static uint8_t* g_code; static uintptr_t g_fn[48]; static int g_n;

void HalDiagInstall(volatile bool* inEyeFlag) {
    const uintptr_t vt = g_base + 0x18c3d98;
    uint8_t* mem = (uint8_t*)VirtualAlloc(nullptr, 16384, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE);
    if (!mem) return;
    g_slots = (Slot*)mem; g_code = mem + 4096;
    for (int i = 0; i < 40; i++) {
        uintptr_t f = *(uintptr_t*)(vt + 8 * i);
        g_fn[i] = f;
        bool dup = false; for (int k = 0; k < i; k++) if (g_fn[k] == f) dup = true;
        if (dup || f < g_base + 0x900000 || f > g_base + 0xb00000) continue;   // Scaleform code only, no shared stubs
        uint8_t* c = g_code + i * 64; Slot* s = &g_slots[i];
        int p = 0;
        auto b = [&](std::initializer_list<uint8_t> l) { for (uint8_t x : l) c[p++] = x; };
        auto q = [&](uint64_t v) { memcpy(c + p, &v, 8); p += 8; };
        b({ 0x4C, 0x8B, 0x1C, 0x24 });                 // mov r11, [rsp]
        b({ 0x49, 0xBA }); q((uint64_t)s);             // mov r10, slot
        b({ 0x4D, 0x89, 0x1A });                       // mov [r10], r11
        b({ 0xF0, 0x49, 0xFF, 0x42, 0x08 });           // lock inc qword [r10+8]
        b({ 0x49, 0xBB }); q((uint64_t)inEyeFlag);     // mov r11, &g_inEye
        b({ 0x41, 0x80, 0x3B, 0x00 });                 // cmp byte [r11], 0
        b({ 0x74, 0x05 });                             // je +5
        b({ 0xF0, 0x49, 0xFF, 0x42, 0x18 });           // lock inc qword [r10+24]
        b({ 0x41, 0xFF, 0x62, 0x10 });                 // jmp [r10+16]
        void* tr = nullptr;
        if (MH_CreateHook((void*)f, c, &tr) == MH_OK && MH_EnableHook((void*)f) == MH_OK) { s->tramp = (uint64_t)tr; g_n++; }
        else Log("haldiag: vtable %d (+0x%llx) not hooked", i, (unsigned long long)(f - g_base));
    }
    Log("haldiag: %d HAL methods counted", g_n);
}
void HalDiagReport(long long frames) {
    if (!g_slots) return;
    Log("haldiag after %lld frames: index rva | calls (in eye) | last caller", frames);
    for (int i = 0; i < 40; i++) {
        Slot& s = g_slots[i];
        if (!s.tramp || !s.count) continue;
        Log("  [%2d] +0x%llx | %llu (%llu) | +0x%llx", i, (unsigned long long)(g_fn[i] - g_base), (unsigned long long)s.count, (unsigned long long)s.inEye,
            (unsigned long long)(s.lastCaller - g_base));
    }
}
