#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>
#include <wrl/client.h>
#include <cstdio>
#include <cstdlib>
#include <imgui/imgui.h>
#include <imgui/imgui_impl_dx11.h>
#include <menu/nr_diagnostic_overlay.h>
using Microsoft::WRL::ComPtr;
static void require(bool b,const char*m){if(!b){fprintf(stderr,"FAIL %s\n",m);exit(1);}}
int main(int argc,char**argv){
    ComPtr<ID3D11Device> device;ComPtr<ID3D11DeviceContext> ctx;
    require(SUCCEEDED(D3D11CreateDevice(nullptr,D3D_DRIVER_TYPE_WARP,nullptr,0,nullptr,0,D3D11_SDK_VERSION,&device,nullptr,&ctx)),"WARP device");
    D3D11_TEXTURE2D_DESC desc{};desc.Width=1280;desc.Height=720;desc.MipLevels=desc.ArraySize=1;desc.Format=DXGI_FORMAT_R8G8B8A8_UNORM;desc.SampleDesc.Count=1;desc.BindFlags=D3D11_BIND_RENDER_TARGET;
    ComPtr<ID3D11Texture2D> target,copy;ComPtr<ID3D11RenderTargetView> rtv;
    require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&target)),"texture");require(SUCCEEDED(device->CreateRenderTargetView(target.Get(),nullptr,&rtv)),"rtv");
    desc.BindFlags=0;desc.Usage=D3D11_USAGE_STAGING;desc.CPUAccessFlags=D3D11_CPU_ACCESS_READ;require(SUCCEEDED(device->CreateTexture2D(&desc,nullptr,&copy)),"staging");
    ImGui::CreateContext();auto&io=ImGui::GetIO();io.IniFilename=nullptr;io.DisplaySize=ImVec2(1280,720);io.DeltaTime=1/60.f;
    require(ImGui_ImplDX11_Init(device.Get(),ctx.Get()),"imgui init");
    require(!NrOverlay::Refresh(),"absent test runtime does not activate UI");
    for(unsigned state=1;state<=5;++state){
        auto&s=NrOverlay::snapshot;s={};s.visible=1;s.state=state;s.width=1920;s.height=1080;s.edge=64;s.updated=GetTickCount64();s.elapsedMs=20000;s.pixels=950;s.fullPairs=8;
        if(state==3){s.centerX=.72f;s.centerY=.28f;}
        ImGui_ImplDX11_NewFrame();ImGui::NewFrame();NrOverlay::Draw();ImGui::Render();
        require(ImGui::GetDrawData()->TotalVtxCount>100,"standalone overlay draws with menu closed");
        float bg[4]={.1f,.1f,.1f,1};ctx->ClearRenderTargetView(rtv.Get(),bg);auto*raw=rtv.Get();ctx->OMSetRenderTargets(1,&raw,nullptr);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());ctx->CopyResource(copy.Get(),target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped{};require(SUCCEEDED(ctx->Map(copy.Get(),0,D3D11_MAP_READ,0,&mapped)),"read overlay");
        auto*p=static_cast<unsigned char*>(mapped.pData);unsigned ink=0;
        const unsigned cx=unsigned(s.centerX*1280),cy=unsigned(s.centerY*720);
        for(unsigned y=cy-25;y<cy+25;y++)for(unsigned x=cx-25;x<cx+25;x++){auto*q=p+y*mapped.RowPitch+x*4;if(state==3?q[1]>200&&q[0]<100:state>=4?q[0]>200&&q[1]<130:q[0]>200&&q[1]>200)++ink;}
        require(ink>100,"selected scaled box and status colour");
        if(state==3&&argc>1){FILE*f=nullptr;fopen_s(&f,argv[1],"wb");require(f!=nullptr,"image output");fprintf(f,"P6\n1280 720\n255\n");for(unsigned y=0;y<720;y++)for(unsigned x=0;x<1280;x++)fwrite(p+y*mapped.RowPitch+x*4,1,3,f);fclose(f);}
        ctx->Unmap(copy.Get(),0);
    }
    ImGui_ImplDX11_Shutdown();ImGui::DestroyContext();puts("PASS test19 overlay: standalone menu-closed rendering, scaled centered ROI, all states, WARP readback.");
}
