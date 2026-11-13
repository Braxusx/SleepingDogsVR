// SDVR: SPatch was written for a MinHook fork whose MH_CreateHook also enables the hook (and needs no start call).
// With stock MinHook the library is started on first use and the hook is created and enabled here in one step.
// (sdvr_spatch.dll carries its own copy of MinHook, separate from the VR mod's.)
#pragma once
#include "../../../third_party/minhook/include/MinHook.h"
static inline MH_STATUS SdvrSpatchCreateHook(LPVOID target, LPVOID detour, LPVOID* original) {
    static const MH_STATUS once = MH_Initialize(); (void)once;
    MH_STATUS s = MH_CreateHook(target, detour, original);
    if (s != MH_OK) return s;
    return MH_EnableHook(target);
}
#define MH_CreateHook SdvrSpatchCreateHook
