#pragma once
#include <algorithm>
#include <cstdio>
#include <dlssnr/backend/lmxxf_runtime/DiagnosticOverlay.h>

namespace NrOverlay {
inline NrDiagnosticOverlay snapshot;
inline bool Refresh() {
    snapshot={};
    HMODULE module=nullptr;
    if (!GetModuleHandleExW(0,L"LmxxfNrRuntime.dll",&module)) return false;
    auto get=reinterpret_cast<NrGetDiagnosticOverlay>(GetProcAddress(module,"LmxxfNrGetDiagnosticOverlay"));
    bool ok=get && get(&snapshot,sizeof(snapshot))==1;
    FreeLibrary(module);
    return ok && snapshot.visible && GetTickCount64()-snapshot.updated<3000;
}
inline void Draw() {
    const auto&s=snapshot;
    if (!s.visible || GetTickCount64()-s.updated>=3000) return;
    auto*vp=ImGui::GetMainViewport(); auto*draw=ImGui::GetForegroundDrawList(vp);
    const auto color=s.state==3?IM_COL32(64,255,96,255):s.state>=4?IM_COL32(255,96,80,255):IM_COL32(255,255,255,255);
    const char*labels[]={"TEST21 READY - F9 starts 20s capture", "TEST21 CAPTURING - BOX LOCKED", "TEST21 SAVING - PLEASE WAIT",
        "TEST21 SAVED - capture is on disk", "TEST21 INCOMPLETE - keep logs; do not repeat", "TEST21: History OFF / Smoothing 0 / Debug OFF"};
    const char*label=labels[s.state<=5?s.state:4];
    float font=ImGui::GetFontSize(); float scale=font/16.0f;
    ImVec2 pos(vp->Pos.x+16*scale,vp->Pos.y+18*scale);
    char details[192];snprintf(details,sizeof(details),"%.1f / %.1f s | F10: box at pointer | Ctrl+Arrows: move box",
        s.elapsedMs/1000.f,s.durationMs/1000.f);
    float width=(std::max)(ImGui::CalcTextSize(label).x,ImGui::CalcTextSize(details).x)+20*scale;
    draw->AddRectFilled(ImVec2(pos.x-8*scale,pos.y-6*scale),ImVec2(pos.x+width,pos.y+font*2.8f),IM_COL32(12,12,12,220),4*scale);
    draw->AddText(pos,color,label);draw->AddText(ImVec2(pos.x,pos.y+font*1.4f),color,details);
    if(s.width && s.height) {
        float hw=vp->Size.x*(std::min)(s.edge,s.width)/s.width/2.f;
        float hh=vp->Size.y*(std::min)(s.edge,s.height)/s.height/2.f;
        ImVec2 center(vp->Pos.x+vp->Size.x*s.centerX,vp->Pos.y+vp->Size.y*s.centerY);
        draw->AddRect(ImVec2(center.x-hw,center.y-hh),ImVec2(center.x+hw,center.y+hh),color,0,0,2*scale);
    }
}
}
