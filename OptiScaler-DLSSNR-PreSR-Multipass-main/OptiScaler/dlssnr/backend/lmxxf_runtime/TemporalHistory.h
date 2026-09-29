#pragma once
#include "LmxxfNrApi.h"
#include <windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

// Product-side producer/consumer for the public lmxxf network's float4 history
// input. No model/kernel changes. Rejected pixels use current encoded RGB: the
// prefix kernels use that same RGB when no temporal input is supplied.
// Owns only same-queue resources; the session must drain before deleting/evicting.
namespace LmxxfTemporal
{
inline constexpr char kShader[] = R"hlsl(
Texture2D<float2> Motion : register(t0);
Texture2D<float> Depth : register(t1);
StructuredBuffer<float4> Raw : register(t2);
StructuredBuffer<float4> PreviousRaw : register(t3);
StructuredBuffer<float4> PreviousModel : register(t4);
StructuredBuffer<float4> Warped : register(t5);
RWStructuredBuffer<float4> WarpOut : register(u0);
RWStructuredBuffer<float4> RawOut : register(u1);
RWStructuredBuffer<float4> ModelOut : register(u2);
RWStructuredBuffer<float> Network : register(u3);
cbuffer Params : register(b0) {
    uint width, height, processingHeight, useHistory;
    uint viewX, viewY, viewWidth, viewHeight;
    uint renderWidth, renderHeight, motionWidth, motionHeight;
    float scaleX, scaleY, jitterX, jitterY;
    float smoothStrength, rawThreshold; uint depthInverted, pad;
};
uint2 Mirror(uint index) {
    uint2 p = uint2(index % width, index / width);
    if (p.y >= height) p.y = 2 * height - p.y - 2;
    return p;
}
bool InView(float2 p) {
    return all(p >= float2(viewX, viewY)) &&
           all(p <= float2(viewX + viewWidth - 1, viewY + viewHeight - 1));
}
float2 ViewUV(uint2 p) { return (float2(p) + .5 - float2(viewX, viewY)) / float2(viewWidth, viewHeight); }
float GetDepth(float2 uv) {
    uint2 p = min(uint2(saturate(uv) * float2(renderWidth, renderHeight)), uint2(renderWidth-1, renderHeight-1));
    float d = Depth.Load(int3(p, 0));
    return depthInverted ? d : 1-d;
}
float Max3(float3 v) { return max(v.x, max(v.y, v.z)); }
bool GoodModel(float3 v) {
    // Zero output is the bridge's visual fallback, never valid temporal history.
    return all(isfinite(v)) && all(v >= -.05) && all(v <= 1.5) && Max3(abs(v)) > 1e-7;
}
[numthreads(64,1,1)]
void Reproject(uint3 id : SV_DispatchThreadID) {
    uint i = id.x; if (i >= width * processingHeight) return;
    uint2 p = Mirror(i);
    float3 raw = Raw[i].rgb;
    WarpOut[i] = float4(raw, 0);
    if (!useHistory || !InView(float2(p)) || !all(isfinite(raw))) return;
    float2 uv = ViewUV(p);
    uint2 mp = min(uint2(uv * float2(motionWidth, motionHeight)), uint2(motionWidth-1, motionHeight-1));
    float2 mv = Motion.Load(int3(mp, 0));
    float2 previousUV = uv + mv * float2(scaleX, scaleY) + float2(jitterX, jitterY);
    if (!all(isfinite(previousUV))) return;
    float2 q = previousUV * float2(viewWidth, viewHeight) + float2(viewX, viewY) - .5;
    if (!InView(q)) return; // Never clamp off-screen history back onto an edge.
    uint2 lo = uint2(floor(q)), hi = min(lo+1, uint2(viewX+viewWidth-1, viewY+viewHeight-1));
    float2 f = q-lo;
    uint4 at = uint4(lo.y*width+lo.x, lo.y*width+hi.x, hi.y*width+lo.x, hi.y*width+hi.x);
    float4 r0=PreviousRaw[at.x], r1=PreviousRaw[at.y], r2=PreviousRaw[at.z], r3=PreviousRaw[at.w];
    float4 h0=PreviousModel[at.x], h1=PreviousModel[at.y], h2=PreviousModel[at.z], h3=PreviousModel[at.w];
    if (min(min(h0.w,h1.w),min(h2.w,h3.w)) <= 0) return;
    float depth = GetDepth(uv);
    float4 oldDepth = float4(r0.w,r1.w,r2.w,r3.w);
    // Device-depth heuristic, not world-space reprojection. Conservative at
    // silhouettes: all four taps must agree, so dark foreground cannot bleed in.
    float4 tolerance = 1e-5 + .05 * max(abs(oldDepth), abs(depth));
    if (!isfinite(depth) || depth < 0 || depth > 1 || !all(isfinite(oldDepth)) ||
        any(abs(oldDepth-depth) > tolerance)) return;
    float3 oldRaw = lerp(lerp(r0.rgb,r1.rgb,f.x),lerp(r2.rgb,r3.rgb,f.x),f.y);
    float3 model = lerp(lerp(h0.rgb,h1.rgb,f.x),lerp(h2.rgb,h3.rgb,f.x),f.y);
    if (!all(isfinite(oldRaw)) || !GoodModel(model)) return;
    if (Max3(abs(oldRaw-raw)) > rawThreshold) return;
    // Guard against self-sustaining black history in a newly bright region.
    if (Max3(model) < .02 && Max3(raw) > .08) return;
    WarpOut[i] = float4(model, 1);
}
[numthreads(64,1,1)]
void Finish(uint3 id : SV_DispatchThreadID) {
    uint i=id.x; if(i>=width*processingHeight) return;
    uint2 p=Mirror(i);
    float3 value=float3(Network[i*3],Network[i*3+1],Network[i*3+2]);
    bool good=GoodModel(value);
    float4 previous=Warped[i];
    // Never turn a zero/failed neural output into a visible old image. This
    // condition is on the GPU because outputs are recorded before HIP enqueue.
    if(good && useHistory && smoothStrength>0 && previous.w>0) {
        float difference=Max3(abs(value-previous.rgb));
        float weight=smoothStrength*saturate(1-difference/(8.0/255.0));
        value=lerp(value,previous.rgb,weight);
        Network[i*3]=value.x; Network[i*3+1]=value.y; Network[i*3+2]=value.z;
    }
    float3 raw=Raw[i].rgb;
    float depth=InView(float2(p)) ? GetDepth(ViewUV(p)) : -1;
    RawOut[i]=float4(raw,depth);
    ModelOut[i]=float4(value,good && all(isfinite(raw)) && InView(float2(p)) ? 1 : 0);
}
)hlsl";

struct Parameters
{
    UINT width{}, height{}, processingHeight{}, useHistory{};
    UINT viewX{}, viewY{}, viewWidth{}, viewHeight{};
    UINT renderWidth{}, renderHeight{}, motionWidth{}, motionHeight{};
    float scaleX{}, scaleY{}, jitterX{}, jitterY{};
    float smoothStrength{}, rawThreshold{.08f};
    UINT depthInverted{}, pad{};
};
static_assert(sizeof(Parameters) == 80);

inline DXGI_FORMAT MotionFormat(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_R16G16_TYPELESS) return DXGI_FORMAT_R16G16_FLOAT;
    if (f == DXGI_FORMAT_R32G32_TYPELESS) return DXGI_FORMAT_R32G32_FLOAT;
    return f == DXGI_FORMAT_R16G16_FLOAT || f == DXGI_FORMAT_R32G32_FLOAT ? f : DXGI_FORMAT_UNKNOWN;
}
inline DXGI_FORMAT DepthFormat(DXGI_FORMAT f)
{
    if (f == DXGI_FORMAT_R32_TYPELESS) return DXGI_FORMAT_R32_FLOAT;
    if (f == DXGI_FORMAT_R16_TYPELESS) return DXGI_FORMAT_R16_UNORM;
    if (f == DXGI_FORMAT_R24G8_TYPELESS) return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    return f == DXGI_FORMAT_R32_FLOAT || f == DXGI_FORMAT_R16_FLOAT || f == DXGI_FORMAT_R16_UNORM ? f : DXGI_FORMAT_UNKNOWN;
}
inline void Check(HRESULT hr, const char *what)
{
    if (FAILED(hr)) throw std::runtime_error(std::string("temporal: ") + what + " HRESULT=" + std::to_string(unsigned(hr)));
}
inline void Transition(ID3D12GraphicsCommandList *cmd, ID3D12Resource *r,
                       D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if(a==b) return;
    D3D12_RESOURCE_BARRIER barrier{};
    barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition={r,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,a,b};
    cmd->ResourceBarrier(1,&barrier);
}

class History
{
    ID3D12Resource *previousRaw{}, *previousModel{}, *warped{};
    ID3D12RootSignature *root{};
    ID3D12PipelineState *reproject{}, *finish{};
    struct Binding { ID3D12Resource *motion, *depth; ID3D12DescriptorHeap *heap; };
    std::vector<Binding> bindings;
    ID3D12DescriptorHeap *activeHeap{};
    UINT width{}, height{}, processingHeight{};

    static ID3D12Resource *Buffer(ID3D12Device *device, UINT64 bytes)
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC desc{};
        desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER; desc.Width=bytes; desc.Height=1;
        desc.DepthOrArraySize=desc.MipLevels=1; desc.SampleDesc.Count=1;
        desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; desc.Flags=D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource *r=nullptr;
        Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,
              D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)),"buffer");
        return r;
    }
    void Bind(ID3D12GraphicsCommandList *cmd, ID3D12Resource *raw, ID3D12Resource *output, const Parameters &p)
    {
        cmd->SetDescriptorHeaps(1,&activeHeap);
        cmd->SetComputeRootSignature(root);
        cmd->SetComputeRootDescriptorTable(0,activeHeap->GetGPUDescriptorHandleForHeapStart());
        ID3D12Resource *srvs[]={raw,previousRaw,previousModel,warped};
        for(UINT i=0;i<4;++i) cmd->SetComputeRootShaderResourceView(1+i,srvs[i]->GetGPUVirtualAddress());
        ID3D12Resource *uavs[]={warped,previousRaw,previousModel,output};
        for(UINT i=0;i<4;++i) cmd->SetComputeRootUnorderedAccessView(5+i,uavs[i]->GetGPUVirtualAddress());
        cmd->SetComputeRoot32BitConstants(9,20,&p,0);
    }
public:
    bool valid=false;
    UINT64 sequence=0, tick=0;
    float jitterX=0,jitterY=0;
    UINT motionWidth=0,motionHeight=0, flags=0;
    float scaleX=0,scaleY=0, smoothing=0, paperWhite=1, preExposure=1, exposureScale=1;
    unsigned used=0, resets=0;
    History()=default;
    History(const History&)=delete;
    ~History()
    {
        ClearBindings();
        for(auto *r:{previousRaw,previousModel,warped}) if(r) r->Release();
        if(root) root->Release();
        if(reproject) reproject->Release();
        if(finish) finish->Release();
    }
    void Reset() { if(valid) ++resets; valid=false; }
    bool Matches(UINT w,UINT h,UINT ph) const { return width==w && height==h && processingHeight==ph; }
    void Create(ID3D12Device *device, UINT w, UINT h, UINT ph)
    {
        width=w; height=h; processingHeight=ph;
        previousRaw=Buffer(device,UINT64(w)*ph*16);
        previousModel=Buffer(device,UINT64(w)*ph*16);
        warped=Buffer(device,UINT64(w)*ph*16);
        D3D12_DESCRIPTOR_RANGE range{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,2,0,0,0};
        D3D12_ROOT_PARAMETER params[10]{};
        params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE; params[0].DescriptorTable={1,&range};
        for(UINT i=0;i<4;++i) {params[i+1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_SRV; params[i+1].Descriptor.ShaderRegister=i+2;}
        for(UINT i=0;i<4;++i) {params[i+5].ParameterType=D3D12_ROOT_PARAMETER_TYPE_UAV; params[i+5].Descriptor.ShaderRegister=i;}
        params[9].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS; params[9].Constants={0,0,20};
        D3D12_ROOT_SIGNATURE_DESC desc{}; desc.NumParameters=10; desc.pParameters=params;
        ID3DBlob *blob=nullptr,*error=nullptr;
        HRESULT hr=D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error);
        if(error) error->Release(); Check(hr,"root serialize");
        hr=device->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root));
        blob->Release(); Check(hr,"root create");
        for(unsigned i=0;i<2;++i)
        {
            blob=nullptr; error=nullptr;
            hr=D3DCompile(kShader,sizeof(kShader)-1,"LmxxfTemporal",nullptr,nullptr,i?"Finish":"Reproject",
                          "cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&blob,&error);
            std::string diagnostic;
            if(error) {diagnostic.assign(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize()); error->Release();}
            if(FAILED(hr)) throw std::runtime_error("temporal shader: "+diagnostic);
            D3D12_COMPUTE_PIPELINE_STATE_DESC pso{}; pso.pRootSignature=root;
            pso.CS={blob->GetBufferPointer(),blob->GetBufferSize()};
            hr=device->CreateComputePipelineState(&pso,IID_PPV_ARGS(i?&finish:&reproject));
            blob->Release(); Check(hr,"pipeline");
        }
    }
    bool NeedsEviction(ID3D12Resource *motion,ID3D12Resource *depth) const
    {
        for(const auto &b:bindings) if(b.motion==motion && b.depth==depth) return false;
        return bindings.size()>=8;
    }
    void ClearBindings() // caller must have completed all GPU users
    {
        for(auto &b:bindings) {b.motion->Release(); b.depth->Release(); b.heap->Release();}
        bindings.clear(); activeHeap=nullptr;
    }
    void PrepareBinding(ID3D12Device *device,ID3D12Resource *motion,ID3D12Resource *depth)
    {
        for(const auto &b:bindings) if(b.motion==motion && b.depth==depth) {activeHeap=b.heap; return;}
        if(NeedsEviction(motion,depth)) throw std::runtime_error("temporal binding eviction needs GPU drain");
        D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,2,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};
        ID3D12DescriptorHeap *heap=nullptr; Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"SRV heap");
        auto handle=heap->GetCPUDescriptorHandleForHeapStart();
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{}; sv.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING; sv.Texture2D.MipLevels=1;
        sv.Format=MotionFormat(motion->GetDesc().Format); device->CreateShaderResourceView(motion,&sv,handle);
        handle.ptr+=device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        sv.Format=DepthFormat(depth->GetDesc().Format); device->CreateShaderResourceView(depth,&sv,handle);
        try { bindings.push_back({motion,depth,heap}); } catch(...) {heap->Release();throw;}
        motion->AddRef(); depth->AddRef(); activeHeap=heap;
    }
    void RecordInputs(ID3D12GraphicsCommandList *cmd,ID3D12Resource *raw,ID3D12Resource *output,
                      ID3D12Resource *motion,ID3D12Resource *depth,D3D12_RESOURCE_STATES ms,D3D12_RESOURCE_STATES ds,
                      const Parameters &p)
    {
        Transition(cmd,motion,ms,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmd,depth,ds,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmd,warped,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Bind(cmd,raw,output,p); cmd->SetPipelineState(reproject); cmd->Dispatch((width*processingHeight+63)/64,1,1);
        Transition(cmd,warped,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmd,motion,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ms);
        Transition(cmd,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ds);
    }
    void RecordOutputs(ID3D12GraphicsCommandList *cmd,ID3D12Resource *raw,ID3D12Resource *output,
                       ID3D12Resource *depth,D3D12_RESOURCE_STATES ds,const Parameters &p)
    {
        Transition(cmd,depth,ds,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        for(auto *r:{previousRaw,previousModel,output}) Transition(cmd,r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        Bind(cmd,raw,output,p); cmd->SetPipelineState(finish); cmd->Dispatch((width*processingHeight+63)/64,1,1);
        for(auto *r:{previousRaw,previousModel,output}) Transition(cmd,r,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Transition(cmd,depth,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,ds);
    }
    ID3D12Resource *Warped() const { return warped; }
};
}
