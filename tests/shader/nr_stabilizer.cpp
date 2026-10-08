#include "nr_effects_float_utils.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/effects/NrStabilizerShader.h"
#include <array>

static float Encode(float x){x=x/(1+x);return x<=.0031308f?12.92f*x:1.055f*std::pow(x,1/2.4f)-.055f;}
int main()
{
    Ptr<ID3D12Debug> debug;if(SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&debug))))debug->EnableDebugLayer();
    Ptr<IDXGIFactory4> factory;Ptr<IDXGIAdapter> adapter;Ptr<ID3D12Device> d;Ptr<ID3D12CommandQueue> q;
    Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");SelectEffectsAdapter(factory.Get(), &adapter);
    Check(D3D12CreateDevice(adapter.Get(),D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&d)),"device");D3D12_COMMAND_QUEUE_DESC qd {};
    Check(d->CreateCommandQueue(&qd,IID_PPV_ARGS(&q)),"queue");
    constexpr UINT w=8,h=4;constexpr size_t n=w*h;
    std::array<Ptr<ID3D12Resource>,7> textures;for(auto& t:textures)t=FloatTexture(d.Get(),w,h);
    std::vector<Pixel> base(n,Pixel{.2f,.2f,.2f,.37f}),result(n,Pixel{.4f,.4f,.4f,.9f});
    std::vector<Pixel> motion(n,Pixel{}),depth(n,Pixel{.5f,0,0,0}),history(n,Pixel{.1f,.1f,.1f,.5f});
    Transfer(d.Get(),q.Get(),textures[0].Get(),&base);Transfer(d.Get(),q.Get(),textures[1].Get(),&result);
    auto guides=[&]{Transfer(d.Get(),q.Get(),textures[2].Get(),&motion);Transfer(d.Get(),q.Get(),textures[3].Get(),&depth);Transfer(d.Get(),q.Get(),textures[4].Get(),&history);};guides();
    D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,5,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,2,0,0,5}};
    D3D12_ROOT_PARAMETER params[2] {};params[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;params[0].DescriptorTable={2,ranges};
    params[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;params[1].Constants={0,0,20};
    D3D12_ROOT_SIGNATURE_DESC rd {2,params,0,nullptr,D3D12_ROOT_SIGNATURE_FLAG_NONE};Ptr<ID3DBlob> blob,error,code;Ptr<ID3D12RootSignature> root;Ptr<ID3D12PipelineState> pso;
    Check(D3D12SerializeRootSignature(&rd,D3D_ROOT_SIGNATURE_VERSION_1,&blob,&error),"root blob");
    Check(d->CreateRootSignature(0,blob->GetBufferPointer(),blob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");
    auto hr=NativeCompileShaderBlob(Effects::StabilizerShader.c_str(),Effects::StabilizerShader.size(),"NR stabilizer",nullptr,nullptr,"main",&code,&error);
    if(FAILED(hr)&&error)std::fprintf(stderr,"%s",static_cast<const char*>(error->GetBufferPointer()));Check(hr,"stabilizer shader");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};pd.pRootSignature=root.Get();pd.CS={code->GetBufferPointer(),code->GetBufferSize()};Check(d->CreateComputePipelineState(&pd,IID_PPV_ARGS(&pso)),"PSO");
    Ptr<ID3D12DescriptorHeap> heap;D3D12_DESCRIPTOR_HEAP_DESC hd {D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,7,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Check(d->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"heap");
    auto cpu=heap->GetCPUDescriptorHandleForHeapStart();const auto stride=d->GetDescriptorHandleIncrementSize(hd.Type);
    for(unsigned i=0;i<7;++i){if(i<5){D3D12_SHADER_RESOURCE_VIEW_DESC v {};v.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;v.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;v.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;v.Texture2D.MipLevels=1;d->CreateShaderResourceView(textures[i].Get(),&v,cpu);}else{D3D12_UNORDERED_ACCESS_VIEW_DESC v {};v.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;v.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;d->CreateUnorderedAccessView(textures[i].Get(),nullptr,&v,cpu);}cpu.ptr+=stride;}
    Effects::StabilizerConstants c {w,h,w,h,1,1,0,0,.8f,4.f/255,1,1,1};
    auto run=[&]{Ptr<ID3D12CommandAllocator> a;Ptr<ID3D12GraphicsCommandList> list;Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"allocator");Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&list)),"list");
        for(unsigned i=5;i<7;++i)Effects::Barrier(list.Get(),textures[i].Get(),D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        auto* raw=heap.Get();list->SetDescriptorHeaps(1,&raw);list->SetComputeRootSignature(root.Get());list->SetPipelineState(pso.Get());list->SetComputeRootDescriptorTable(0,heap->GetGPUDescriptorHandleForHeapStart());list->SetComputeRoot32BitConstants(1,20,&c,0);list->Dispatch(1,1,1);
        for(unsigned i=5;i<7;++i)Effects::Barrier(list.Get(),textures[i].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        Check(list->Close(),"close");ID3D12CommandList* lists[]={list.Get()};q->ExecuteCommandLists(1,lists);WaitQueue(d.Get(),q.Get());return Transfer(d.Get(),q.Get(),textures[5].Get());};
    auto equal=[&](const std::vector<Pixel>& values,float expected){for(auto v:values){Require(std::abs(v[0]-expected)<2e-5f,"expected image");Require(v[3]==.37f,"original alpha");}};
    c.flags=0;equal(run(),.4f); // No history must preserve the current correction.
    c.flags=1;auto filtered=run();Require(filtered[0][0]<.4f&&filtered[0][0]>.3f,"bounded attenuation");
    c.intensity=.5f;auto attenuated=run();Require(std::abs(attenuated[0][0]-(.2f+filtered[0][0])*.5f)<2e-5f,"intensity follows stabilization");
    auto next=Transfer(d.Get(),q.Get(),textures[6].Get());c.intensity=1;run();auto nextFull=Transfer(d.Get(),q.Get(),textures[6].Get());Require(next==nextFull,"intensity cannot contaminate history");
    c.residual={1.5f,.5f,1.f,1.f};
    auto shaped=run();auto nextShaped=Transfer(d.Get(),q.Get(),textures[6].Get());
    Require(nextShaped==nextFull,"spatial controls cannot contaminate stored temporal history");
    Require(shaped[0][0]>filtered[0][0],"low-frequency gain follows stabilized correction");
    c.residual={};
    c.intensity=0;equal(run(),.2f);c.intensity=1;
    c.alpha=0;equal(run(),.4f);c.alpha=.8f;c.threshold=0;equal(run(),.4f);c.threshold=4.f/255;
    for(auto& v:depth)v[0]=.8f;guides();equal(run(),.4f); // disocclusion
    for(auto& v:depth)v[0]=.5f;for(auto& v:motion)v[0]=std::numeric_limits<float>::quiet_NaN();guides();equal(run(),.4f);
    for(auto& v:motion)v[0]=0;for(auto& v:depth)v[0]=-1;guides();equal(run(),.4f);
    // Translation and jitter choose the same previous texel. Mixed depth taps
    // must not borrow a rejected surface even when another tap passes.
    for(auto& v:depth)v[0]=.5f;for(size_t i=0;i<n;++i)history[i]={i%w<4?.1f:.3f,.1f,.1f,.5f};guides();
    c.mvScaleX=1;for(auto& v:motion)v[0]=1;guides();auto moved=run();for(auto& v:motion)v[0]=0;guides();c.jitterX=1;auto jittered=run();Require(moved==jittered,"motion and jitter alignment");
    c.jitterX=.5f;history[1]={100,100,100,.1f};guides();auto rejected=run();Require(rejected[0][0]<.4f,"depth weighted history excludes foreign surface");
    c.jitterX=0;c.flags=3;for(auto& v:depth)v[0]=.5f;guides();auto reversed=run();c.flags=1;Require(run()==reversed,"reversed depth convention");
    // Static alternating correction: cumulative history reduces variation.
    for(auto& v:history)v={0,0,0,.5f};c.flags=0;float prev=0,variation=0,unfiltered=0;
    c.residual={1.5f,.5f,.5f,.5f};
    for(unsigned frame=0;frame<20;++frame){c.preExposure=.2f+frame*.002f;for(auto& v:base)v={.2f*c.preExposure,.2f*c.preExposure,.2f*c.preExposure,.37f};Transfer(d.Get(),q.Get(),textures[0].Get(),&base);float value=(frame%2?.42f:.4f)*c.preExposure;for(auto& v:result)v={value,value,value,.9f};Transfer(d.Get(),q.Get(),textures[1].Get(),&result);guides();auto out=run();if(frame>5){variation+=std::abs(out[0][0]/c.preExposure-prev);unfiltered+=.03f;}prev=out[0][0]/c.preExposure;history=Transfer(d.Get(),q.Get(),textures[6].Get());c.flags=1;}
    Require(variation<unfiltered*.65f,"flicker reduction survives continuously changing pre-exposure");
    Ptr<ID3D12InfoQueue> info;if(SUCCEEDED(d.As(&info)))for(UINT64 i=0;i<info->GetNumStoredMessages();++i){SIZE_T bytes=0;info->GetMessage(i,nullptr,&bytes);std::vector<char> storage(bytes);auto* m=reinterpret_cast<D3D12_MESSAGE*>(storage.data());Check(info->GetMessage(i,m,&bytes),"debug message");if(m->Severity<=D3D12_MESSAGE_SEVERITY_ERROR)std::fprintf(stderr,"%s\n",m->pDescription);Require(m->Severity>D3D12_MESSAGE_SEVERITY_ERROR,"debug validation");}
    std::puts("NR stabilizer shader: PASS (flicker, motion/jitter, depth, invalid guides, intensity)");
}
