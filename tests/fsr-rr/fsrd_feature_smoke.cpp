// Native NGX consumer: the production DLL owns FSRDFeature, configuration, routing,
// deferred recreation, converter, RR and SR. Never link offline_chain or product TUs.
// Stage this EXE beside the actual host as dxgi.dll, its OptiScaler.ini and providers.
// Build: tests\fsr-rr\fsrd_feature_smoke.cmd <staging-directory>
// Run:  fsrd_feature_smoke.exe --luid <hex-LUID> [--guided-mask 12|15] [--out <directory>]
// 12 tests Auto/forced Indirect; 15 tests forced Split. Estimates/recovery must be off.
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <DirectXMath.h>
#include <DirectXPackedVector.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

using Microsoft::WRL::ComPtr;
namespace fs = std::filesystem;
namespace {
constexpr unsigned RW = 192, RH = 128, OW = RW * 2, OH = RH * 2;
constexpr float Near = .05f, Far = 800.f;
constexpr D3D12_RESOURCE_STATES ReadState = static_cast<D3D12_RESOURCE_STATES>(
    D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

void Require(bool ok, const std::string& message)
{
    if (!ok) throw std::runtime_error(message);
}
void Check(HRESULT result, const char* operation)
{
    char error[128];
    std::snprintf(error, sizeof(error), "%s: HRESULT 0x%08lX", operation, static_cast<unsigned long>(result));
    Require(SUCCEEDED(result), error);
}
void Ngx(NVSDK_NGX_Result result, const char* operation)
{
    char error[128];
    std::snprintf(error, sizeof(error), "%s: NGX 0x%08X", operation, unsigned(result));
    Require(result == NVSDK_NGX_Result_Success, error);
}
template<class T> T Export(HMODULE module, const char* name)
{
    const auto address = GetProcAddress(module, name);
    Require(address != nullptr, std::string("missing host export: ") + name);
    return reinterpret_cast<T>(address);
}
std::string Text(const fs::path& path)
{
    std::ifstream file(path, std::ios::binary);
    Require(bool(file), "cannot read " + path.string());
    return {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
}

struct Vec
{
    float x, y, z;
    Vec operator+(Vec b) const { return {x+b.x, y+b.y, z+b.z}; }
    Vec operator-(Vec b) const { return {x-b.x, y-b.y, z-b.z}; }
    Vec operator*(float s) const { return {x*s, y*s, z*s}; }
};
float Dot(Vec a, Vec b) { return a.x*b.x + a.y*b.y + a.z*b.z; }
Vec Unit(Vec v) { return v * (1.f / std::sqrt(Dot(v,v))); }
Vec Cross(Vec a, Vec b) { return {a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x}; }
struct Hit { Vec point, normal; float distance; unsigned face; };
// A closed, inward-facing room, in metres. Both guides are actual secondary-ray
// segment lengths against this geometry, never camera depth or zero placeholders.
Hit Trace(Vec origin, Vec direction)
{
    constexpr float low[3] = {-10,-10,-20}, high[3] = {10,10,2};
    float best = std::numeric_limits<float>::max();
    unsigned face = 0;
    const float o[3] = {origin.x,origin.y,origin.z}, d[3] = {direction.x,direction.y,direction.z};
    // All rays begin inside the closed room. The first forward boundary is
    // therefore the nearest slab exit; a tolerant face-rectangle test could
    // incorrectly choose a face just beyond an edge and spawn outside the room.
    Require(o[0]>low[0] && o[0]<high[0] && o[1]>low[1] && o[1]<high[1] &&
            o[2]>low[2] && o[2]<high[2], "analytic ray origin is outside the room");
    for (unsigned axis=0; axis<3; ++axis)
    {
        if (std::abs(d[axis]) < 1e-7f) continue;
        const unsigned side = d[axis]>0 ? 1u : 0u;
        const float t = ((side ? high[axis] : low[axis]) - o[axis]) / d[axis];
        if (t>0 && t<best) { best=t; face=axis*2+side; }
    }
    Require(std::isfinite(best) && best > 0 && best < 100,
            "analytic ray did not intersect the actual room");
    Vec normal{};
    if (face/2==0) normal.x = face%2 ? -1.f : 1.f;
    if (face/2==1) normal.y = face%2 ? -1.f : 1.f;
    if (face/2==2) normal.z = face%2 ? -1.f : 1.f;
    return {origin+direction*best,normal,best,face};
}
struct Fixture
{
    std::vector<uint16_t> color, normals, motion;
    std::vector<uint8_t> diffuse, specular;
    std::vector<float> depth, specularDistance, diffuseDistance;
    DirectX::XMFLOAT4X4 view{}, projection{};
    Fixture() : color(RW*RH*4), normals(RW*RH*4), motion(RW*RH*2),
        diffuse(RW*RH*4), specular(RW*RH*4), depth(RW*RH),
        specularDistance(RW*RH), diffuseDistance(RW*RH)
    {
        using namespace DirectX;
        // DirectX row-vector storage is NGX's column-major storage of the
        // mathematical column-vector transform. Product transposes it on input.
        XMStoreFloat4x4(&view, XMMatrixIdentity());
        XMStoreFloat4x4(&projection, XMMatrixPerspectiveFovRH(XM_PI / 3.f, float(RW)/RH, Far, Near));
        const float tanHalf = std::tan(XM_PI/6.f), aspect = float(RW)/RH;
        for (unsigned y=0; y<RH; ++y) for (unsigned x=0; x<RW; ++x)
        {
            const unsigned i=y*RW+x;
            const Vec ray = Unit({(2*(x+.5f)/RW-1)*aspect*tanHalf,
                                  (1-2*(y+.5f)/RH)*tanHalf,-1});
            const Hit hit=Trace({0,0,0},ray);
            // At an exact room edge, offsetting only the selected face normal
            // leaves the origin on another wall. Retreat along the incident ray
            // so mirror and cosine rays start strictly inside every incident slab.
            const Vec start=hit.point-ray*.001f;
            const Vec reflected=ray-hit.normal*(2*Dot(ray,hit.normal));
            specularDistance[i]=Trace(start,reflected).distance;
            const Vec axis=std::abs(hit.normal.y)<.9f ? Vec{0,1,0} : Vec{1,0,0};
            const Vec tangent=Unit(Cross(axis,hit.normal)), bitangent=Cross(hit.normal,tangent);
            const float u=(float((i*73u)%1021u)+.5f)/1021.f;
            const float phi=XM_2PI*(float((i*137u)%1019u)+.5f)/1019.f;
            const Vec scattered=hit.normal*std::sqrt(1-u)+
                tangent*(std::sqrt(u)*std::cos(phi))+bitangent*(std::sqrt(u)*std::sin(phi));
            diffuseDistance[i]=Trace(start,scattered).distance;
            const float z=hit.point.z;
            depth[i]=(projection._33*z+projection._43)/(-z);
            Require(depth[i]>0 && depth[i]<1, "invalid analytic reversed hardware depth");
            const float checker=((int(std::floor(hit.point.x))+int(std::floor(hit.point.y)))&1) ? .7f : 1.f;
            const float albedo[3]={.55f*checker,.40f*checker,.28f*checker};
            const float n[4]={hit.normal.x,hit.normal.y,hit.normal.z,.35f};
            for(unsigned c=0;c<4;++c)
            {
                normals[4*i+c]=PackedVector::XMConvertFloatToHalf(n[c]);
                diffuse[4*i+c]=c==3 ? 255 : uint8_t(albedo[c]*255);
                specular[4*i+c]=c==3 ? 255 : 10;
                const float light=.8f+.2f*std::max(0.f,Dot(hit.normal,Unit({.3f,.6f,.7f})));
                color[4*i+c]=PackedVector::XMConvertFloatToHalf(c==3 ? 1.f : albedo[c]*light+.02f);
            }
        }
    }
};

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource,
             D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after)
{
    if(before==after) return;
    D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};
    list->ResourceBarrier(1,&b);
}
ComPtr<ID3D12Resource> Buffer(ID3D12Device* device, UINT64 size, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES heap{}; heap.Type=type;
    D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width=size; desc.Height=1; desc.DepthOrArraySize=1; desc.MipLevels=1;
    desc.SampleDesc.Count=1; desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> result;
    Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,
        type==D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
        nullptr,IID_PPV_ARGS(&result)),"Create buffer");
    return result;
}
struct Texture
{
    ComPtr<ID3D12Resource> resource, upload, readback;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
    D3D12_RESOURCE_STATES state=D3D12_RESOURCE_STATE_COPY_DEST;
    unsigned width, height, rowBytes;
    Texture(ID3D12Device* device,unsigned w,unsigned h,DXGI_FORMAT format,unsigned bytes,bool output=false)
        : width(w),height(h),rowBytes(w*bytes)
    {
        D3D12_HEAP_PROPERTIES heap{}; heap.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{}; desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width=w; desc.Height=h; desc.DepthOrArraySize=1; desc.MipLevels=1;
        desc.Format=format; desc.SampleDesc.Count=1;
        desc.Flags=output ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        Check(device->CreateCommittedResource(&heap,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,
            IID_PPV_ARGS(&resource)),"Create texture");
        UINT64 total=0; device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&total);
        upload=Buffer(device,total,D3D12_HEAP_TYPE_UPLOAD);
        if(output) readback=Buffer(device,total,D3D12_HEAP_TYPE_READBACK);
    }
    void Upload(ID3D12GraphicsCommandList* list,const void* data,D3D12_RESOURCE_STATES finalState=ReadState)
    {
        uint8_t* mapped=nullptr; D3D12_RANGE empty{};
        Check(upload->Map(0,&empty,reinterpret_cast<void**>(&mapped)),"Map upload");
        for(unsigned y=0;y<height;++y)
            std::memcpy(mapped+footprint.Offset+y*size_t(footprint.Footprint.RowPitch),
                        static_cast<const uint8_t*>(data)+y*size_t(rowBytes),rowBytes);
        upload->Unmap(0,nullptr);
        Barrier(list,resource.Get(),state,D3D12_RESOURCE_STATE_COPY_DEST);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};
        dst.pResource=resource.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.pResource=upload.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; src.PlacedFootprint=footprint;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(list,resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,finalState); state=finalState;
    }
    void CopyOut(ID3D12GraphicsCommandList* list)
    {
        Barrier(list,resource.Get(),state,D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst{},src{};
        dst.pResource=readback.Get(); dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT; dst.PlacedFootprint=footprint;
        src.pResource=resource.Get(); src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
        Barrier(list,resource.Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,state);
    }
    std::vector<uint16_t> Read()
    {
        uint8_t* mapped=nullptr;
        Check(readback->Map(0,nullptr,reinterpret_cast<void**>(&mapped)),"Map readback");
        std::vector<uint16_t> result(size_t(width)*height*4);
        for(unsigned y=0;y<height;++y)
            std::memcpy(result.data()+size_t(y)*width*4,mapped+footprint.Offset+y*size_t(footprint.Footprint.RowPitch),rowBytes);
        D3D12_RANGE empty{}; readback->Unmap(0,&empty);
        return result;
    }
};

// Query only the production interface identity; never install or construct a proxy
// inside the consumer. A plain list must fail the test before feature creation.
const GUID RecordingResources={0x8e54c39a,0x4a7e,0x4c2e,{0x9c,0x8f,0x63,0xfa,0x75,0x8a,0x61,0x0e}};
using InitFn=NVSDK_NGX_Result (*)(const char*,int,const char*,const wchar_t*,ID3D12Device*,const void*,int);
using CapsFn=NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter**);
using CreateFn=NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*,int,NVSDK_NGX_Parameter*,NVSDK_NGX_Handle**);
using EvalFn=NVSDK_NGX_Result (*)(ID3D12GraphicsCommandList*,const NVSDK_NGX_Handle*,const NVSDK_NGX_Parameter*,void*);
using ReleaseFn=NVSDK_NGX_Result (*)(NVSDK_NGX_Handle*);
using DestroyFn=NVSDK_NGX_Result (*)(NVSDK_NGX_Parameter*);
using ShutdownFn=NVSDK_NGX_Result (*)(ID3D12Device*);

unsigned LastMask(const std::string& log)
{
    const std::string key="[RR_DIAG] signal plan: mask=";
    const auto offset=log.rfind(key);
    Require(offset!=std::string::npos,"production RR context signal-plan evidence missing");
    return unsigned(std::stoul(log.substr(offset+key.size()),nullptr,0));
}
void ResetEvidence(const std::string& log)
{
    const auto offset=log.rfind("[RR_DIAG] first dispatch succeeded:");
    Require(offset!=std::string::npos,"production RR first-dispatch evidence missing");
    const auto end=log.find('\n',offset);
    Require(log.substr(offset,end-offset).find("reset=true")!=std::string::npos,
            "production recreated RR context did not dispatch with reset=true");
}
}

int wmain(int argc,wchar_t** argv)
try
{
    uint64_t wantedLuid=0; unsigned guidedMask=12; fs::path out=L"feature-smoke-output";
    for(int i=1;i<argc;++i)
    {
        const std::wstring arg=argv[i];
        Require(i+1<argc,"option requires a value");
        if(arg==L"--luid") wantedLuid=std::stoull(argv[++i],nullptr,16);
        else if(arg==L"--guided-mask") guidedMask=unsigned(std::stoul(argv[++i]));
        else if(arg==L"--out") out=argv[++i];
        else throw std::runtime_error("unknown option");
    }
    Require(wantedLuid!=0,"explicit --luid <hex adapter LUID> is required; no default adapter or WARP");
    Require(guidedMask==12 || guidedMask==15,"--guided-mask must be 12 (Auto/Indirect) or 15 (Split)");
    wchar_t exe[MAX_PATH]; Require(GetModuleFileNameW(nullptr,exe,MAX_PATH)!=0,"GetModuleFileNameW");
    const fs::path stage=fs::path(exe).parent_path();
    // Production discovers configuration and dependent DLLs relative to the game.
    Require(SetCurrentDirectoryW(stage.c_str())!=0,"SetCurrentDirectoryW(stage)");
    fs::create_directories(out);
    Require(fs::exists(stage/L"OptiScaler.ini"),"stage actual OptiScaler.ini before launching");
    const fs::path logPath=stage/L"fsrd-feature-smoke-host.log";
    // Preserve earlier evidence; use a new staging directory for each process/run.
    Require(!fs::exists(logPath),"use a fresh staging directory (host log already exists)");
    HMODULE host=LoadLibraryW((stage/L"dxgi.dll").c_str());
    Require(host!=nullptr,"cannot load staged production dxgi.dll");
    auto factoryFn=Export<HRESULT(WINAPI*)(UINT,REFIID,void**)>(host,"CreateDXGIFactory2");
    auto init=Export<InitFn>(host,"NVSDK_NGX_D3D12_Init_with_ProjectID");
    auto caps=Export<CapsFn>(host,"NVSDK_NGX_D3D12_GetCapabilityParameters");
    auto create=Export<CreateFn>(host,"NVSDK_NGX_D3D12_CreateFeature");
    auto evaluate=Export<EvalFn>(host,"NVSDK_NGX_D3D12_EvaluateFeature");
    auto release=Export<ReleaseFn>(host,"NVSDK_NGX_D3D12_ReleaseFeature");
    auto destroy=Export<DestroyFn>(host,"NVSDK_NGX_D3D12_DestroyParameters");
    auto shutdown=Export<ShutdownFn>(host,"NVSDK_NGX_D3D12_Shutdown1");
    HMODULE d3d=LoadLibraryW(L"d3d12.dll"); Require(d3d!=nullptr,"LoadLibrary D3D12");
    auto createDevice=Export<PFN_D3D12_CREATE_DEVICE>(d3d,"D3D12CreateDevice");
    ComPtr<IDXGIFactory4> factory; Check(factoryFn(0,IID_PPV_ARGS(&factory)),"CreateDXGIFactory2");
    LUID luid{}; luid.LowPart=DWORD(wantedLuid); luid.HighPart=LONG(wantedLuid>>32);
    ComPtr<IDXGIAdapter1> adapter; Check(factory->EnumAdapterByLuid(luid,IID_PPV_ARGS(&adapter)),"EnumAdapterByLuid");
    DXGI_ADAPTER_DESC1 adapterDesc{}; Check(adapter->GetDesc1(&adapterDesc),"Adapter description");
    Require(!(adapterDesc.Flags&DXGI_ADAPTER_FLAG_SOFTWARE),"software adapter is not feature acceptance");
    ComPtr<ID3D12Device> device; Check(createDevice(adapter.Get(),D3D_FEATURE_LEVEL_12_0,IID_PPV_ARGS(&device)),"CreateDevice");
    D3D12_COMMAND_QUEUE_DESC q{}; q.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue; Check(device->CreateCommandQueue(&q,IID_PPV_ARGS(&queue)),"CreateCommandQueue");
    Ngx(init("a0f57b54-1daf-4934-90ae-c4035c19df04",0,"1.0",L".",device.Get(),nullptr,0x15),"Init");
    ComPtr<ID3D12CommandAllocator> allocator;
    Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"Create allocator");
    ComPtr<ID3D12GraphicsCommandList> list;
    Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"Create list");
    ComPtr<IUnknown> observable;
    Check(list->QueryInterface(RecordingResources,reinterpret_cast<void**>(observable.GetAddressOf())),
          "actual production command-list recording interface");
    observable.Reset();
    ComPtr<ID3D12Fence> fence; Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"Create fence");
    HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr); Require(event!=nullptr,"CreateEvent");
    UINT64 sequence=0;
    auto submit=[&] {
        Check(list->Close(),"Close"); ID3D12CommandList* command=list.Get(); queue->ExecuteCommandLists(1,&command);
        Check(queue->Signal(fence.Get(),++sequence),"Signal");
        if(fence->GetCompletedValue()<sequence)
        {
            Check(fence->SetEventOnCompletion(sequence,event),"SetEventOnCompletion");
            Require(WaitForSingleObject(event,120000)==WAIT_OBJECT_0,"GPU completion timeout");
        }
        Check(device->GetDeviceRemovedReason(),"Device removed reason");
        Check(allocator->Reset(),"Reset allocator"); Check(list->Reset(allocator.Get(),nullptr),"Reset list");
    };
    Fixture fixture;
    Texture color(device.Get(),RW,RH,DXGI_FORMAT_R16G16B16A16_FLOAT,8),
        normals(device.Get(),RW,RH,DXGI_FORMAT_R16G16B16A16_FLOAT,8),
        motion(device.Get(),RW,RH,DXGI_FORMAT_R16G16_FLOAT,4),
        depth(device.Get(),RW,RH,DXGI_FORMAT_R32_FLOAT,4),
        diffuse(device.Get(),RW,RH,DXGI_FORMAT_R8G8B8A8_UNORM,4),
        specular(device.Get(),RW,RH,DXGI_FORMAT_R8G8B8A8_UNORM,4),
        specDistance(device.Get(),RW,RH,DXGI_FORMAT_R32_FLOAT,4),
        diffDistance(device.Get(),RW,RH,DXGI_FORMAT_R32_FLOAT,4),
        exposure(device.Get(),1,1,DXGI_FORMAT_R32_FLOAT,4),
        output(device.Get(),OW,OH,DXGI_FORMAT_R16G16B16A16_FLOAT,8,true);
    color.Upload(list.Get(),fixture.color.data()); normals.Upload(list.Get(),fixture.normals.data());
    motion.Upload(list.Get(),fixture.motion.data()); depth.Upload(list.Get(),fixture.depth.data());
    diffuse.Upload(list.Get(),fixture.diffuse.data()); specular.Upload(list.Get(),fixture.specular.data());
    specDistance.Upload(list.Get(),fixture.specularDistance.data()); diffDistance.Upload(list.Get(),fixture.diffuseDistance.data());
    const float one=1; exposure.Upload(list.Get(),&one);
    NVSDK_NGX_Parameter* p=nullptr; Ngx(caps(&p),"GetCapabilityParameters");
    p->Set("Width",RW); p->Set("Height",RH); p->Set("OutWidth",OW); p->Set("OutHeight",OH);
    p->Set("PerfQualityValue",0); p->Set("DLSS.Feature.Create.Flags",1|2|8);
    p->Set("CreationNodeMask",1u); p->Set("VisibilityNodeMask",1u);
    p->Set(NVSDK_NGX_Parameter_DLSS_Denoise_Mode,1);
    p->Set(NVSDK_NGX_Parameter_DLSS_Roughness_Mode,1);
    p->Set(NVSDK_NGX_Parameter_Use_HW_Depth,1);
    // No scalar overrides: the exact finite RH .05/800 matrix must work on frame zero.
    p->Set(NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX,static_cast<void*>(&fixture.view));
    p->Set(NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX,static_cast<void*>(&fixture.projection));
    NVSDK_NGX_Handle* handle=nullptr;
    Ngx(create(list.Get(),NVSDK_NGX_Feature_RayReconstruction,p,&handle),"CreateFeature(RR)");
    Require(handle!=nullptr,"NGX returned no feature handle"); submit();
    p->Set("Color",color.resource.Get()); p->Set("Output",output.resource.Get());
    p->Set("Depth",depth.resource.Get()); p->Set("MotionVectors",motion.resource.Get());
    p->Set("ExposureTexture",exposure.resource.Get());
    p->Set(NVSDK_NGX_Parameter_GBuffer_Normals,normals.resource.Get());
    p->Set(NVSDK_NGX_Parameter_DiffuseAlbedo,diffuse.resource.Get());
    p->Set(NVSDK_NGX_Parameter_SpecularAlbedo,specular.resource.Get());
    p->Set("Jitter.Offset.X",0.f); p->Set("Jitter.Offset.Y",0.f);
    p->Set("MV.Scale.X",1.f); p->Set("MV.Scale.Y",1.f);
    p->Set("DLSS.Pre.Exposure",1.f); p->Set("DLSS.Exposure.Scale",1.f);
    p->Set("FrameTimeDeltaInMsec",16.6667f);
    p->Set("DLSS.Render.Subrect.Dimensions.Width",RW); p->Set("DLSS.Render.Subrect.Dimensions.Height",RH);
    const std::vector<uint16_t> sentinel(size_t(OW)*OH*4,0x7e00u);
    std::ofstream evidence(out/L"frames.csv"); Require(bool(evidence),"open frame evidence");
    evidence<<"phase,frame,guides,ngx_result,fresh_output,context_mask\n";
    constexpr const char* phases[]={"absent","arrival","dropout","return"};
    unsigned frame=0;
    for(unsigned phase=0;phase<4;++phase)
    {
        const bool guides=(phase&1)!=0;
        const unsigned expected=guides ? guidedMask : 3u;
        const size_t phaseLogStart=Text(logPath).size();
        p->Set(NVSDK_NGX_Parameter_DLSSD_SpecularHitDistance,guides ? specDistance.resource.Get() : nullptr);
        p->Set(NVSDK_NGX_Parameter_DLSSD_DiffuseHitDistance,guides ? diffDistance.resource.Get() : nullptr);
        unsigned consecutive=0,failures=0,emptyFrames=0;
        for(unsigned local=0;local<10;++local,++frame)
        {
            // Poison the real output every call: wrapper Success during deferred
            // recreation must not count as an RR+SR dispatch or reuse stale pixels.
            output.Upload(list.Get(),sentinel.data(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            p->Set("Reset",frame==0 ? 1 : 0);
            const auto result=evaluate(list.Get(),handle,p,nullptr);
            output.CopyOut(list.Get()); submit();
            const auto pixels=output.Read();
            const bool untouched=std::all_of(pixels.begin(),pixels.end(),[](uint16_t v){return v==0x7e00u;});
            const bool finite=std::all_of(pixels.begin(),pixels.end(),[](uint16_t v){return (v&0x7c00u)!=0x7c00u;});
            const bool fresh=!untouched && finite;
            Require(untouched || finite,"partial/nonfinite actual SR output");
            const std::string log=Text(logPath);
            const auto mask=LastMask(log);
            evidence<<phases[phase]<<','<<frame<<','<<guides<<",0x"<<std::hex<<unsigned(result)<<std::dec
                    <<','<<fresh<<','<<mask<<'\n'; evidence.flush();
            std::printf("feature-smoke phase=%s frame=%u guides=%u NGX=0x%08X fresh=%u mask=0x%x\n",
                        phases[phase],frame,guides,unsigned(result),fresh,mask); std::fflush(stdout);
            if(phase==0 && local==0)
                Require(result==NVSDK_NGX_Result_Success && fresh,
                        "finite RH .05/800 first native NGX evaluation must succeed and write output");
            if(result!=NVSDK_NGX_Result_Success)
            {
                // This is the existing public wrapper's explicitly failed boundary
                // frame, not a workaround accepted by the Godot consumer. Only the
                // first boundary call may fail, backed by an actual plan-change log.
                ++failures;
                Require(phase>0 && local==0 && failures==1 &&
                        log.substr(phaseLogStart).find("[RR_INPUT] signal plan ")!=std::string::npos,
                        "unexpected NGX failure outside a logged deferred plan-change boundary");
            }
            if(!fresh)
            {
                Require(phase>0 && local<3 && untouched,
                        "missing output outside the three deferred recreation boundary calls");
                ++emptyFrames; consecutive=0;
            }
            else
            {
                Require(result==NVSDK_NGX_Result_Success,"failed NGX call produced untrusted output");
                Require(mask==expected,"actual production context has the wrong signal layout");
                ++consecutive;
            }
            Require(emptyFrames<=3,"deferred recreation did not resume actual RR+SR output within three calls");
        }
        Require(consecutive>=3,"phase did not finish with three actual fresh RR+SR outputs");
        const auto log=Text(logPath);
        Require(LastMask(log)==expected,"production context layout mismatch at phase end");
        ResetEvidence(phase==0 ? log : log.substr(phaseLogStart));
        const auto pixels=output.Read();
        std::ofstream image(out/(std::string(phases[phase])+".rgba16f"),std::ios::binary);
        image.write(reinterpret_cast<const char*>(pixels.data()),std::streamsize(pixels.size()*sizeof(uint16_t)));
        Require(bool(image),"write output evidence");
    }
    // Drain recorded resources before release; leave no unsubmitted feature work.
    submit(); Ngx(release(handle),"ReleaseFeature"); Ngx(destroy(p),"DestroyParameters");
    Ngx(shutdown(device.Get()),"Shutdown1"); CloseHandle(event);
    std::ofstream report(out/L"result.txt");
    report<<"PASS native production NGX RR wrapper; actual startup recording proxy; finite RH near=.05 far=800; "
          <<"guide absence/arrival/dropout/return; guided mask="<<guidedMask
          <<"; real traced secondary ray lengths; fresh GPU-completed finite SR output each stable phase; "
          <<"recreated RR reset confirmed from production logs. SR reset has no external getter; inspect host log/trace. "
          <<"Execution/control-plane smoke only, not a quality verdict.\n";
    Require(bool(report),"write result evidence");
    std::puts("PASS: actual production FSRDFeature native NGX wrapper smoke (not offline_chain)");
    // Do not FreeLibrary: native COM proxy objects are still scoped to the host.
    return 0;
}
catch(const std::exception& error)
{
    std::fprintf(stderr,"FAIL: fsrd_feature_smoke: %s\n",error.what());
    return 1;
}
