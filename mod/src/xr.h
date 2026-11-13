#pragma once
#include <d3d11.h>
void XrConfigure(const wchar_t* ini);
bool XrActive();
void XrSubmitEye(int eye, ID3D11DeviceContext* ctx, ID3D11Texture2D* src);
void XrAfterPresent();
#include <openxr/openxr.h>
bool XrLocateEyes(XrView out[2]);
long XrLocateFails();   // build 033: eye passes where the headset pose could not be located
long XrSetFrameViews(const XrView v[2]);
void XrSetPresentSeq(long seq);

void XrHudTangents(float* tx, float* ty, float* ty0);
void XrSetHudCutscene(bool on);
