#pragma once
#include <wrl/client.h>
#include <vector>
#include <array>

// Small real D3D12 workload for the production codec/RGB macro matrix. Every
// target executes the same finite input and is compared byte-for-byte; PSO
// creation alone cannot establish cs_5_0 numerical compatibility.
class ShaderTestDispatch {
 template<class T>using Ptr=Microsoft::WRL::ComPtr<T>;
 Ptr<ID3D12Device> device;
 Ptr<ID3D12CommandQueue> queue;
 Ptr<ID3D12CommandAllocator> allocator;
 Ptr<ID3D12GraphicsCommandList> list;
 Ptr<ID3D12DescriptorHeap> heap;
 Ptr<ID3D12Fence> fence;
 UINT stride{};UINT64 value{};
 std::vector<Ptr<ID3D12Resource>> uploads;
 std::array<Ptr<ID3D12Resource>,5> inputs;
 std::array<Ptr<ID3D12Resource>,2> outputs;
 Ptr<ID3D12Resource> readback,postReadback;
 std::array<UINT,20> constants{};
 D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint{};
 UINT64 outputBytes{};bool textureOutput{},copied{},rgbOutputs{},untiled{};
 static constexpr UINT width=48,height=96,paddedHeight=128;
 D3D12_CPU_DESCRIPTOR_HANDLE Cpu(UINT index){auto h=heap->GetCPUDescriptorHandleForHeapStart();h.ptr+=SIZE_T(index)*stride;return h;}
 D3D12_GPU_DESCRIPTOR_HANDLE Gpu(UINT index){auto h=heap->GetGPUDescriptorHandleForHeapStart();h.ptr+=UINT64(index)*stride;return h;}
 Ptr<ID3D12Resource> Buffer(UINT64 size,D3D12_HEAP_TYPE type,D3D12_RESOURCE_STATES state,bool uav=false){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=type;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;desc.Width=size;desc.Height=1;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;desc.Flags=uav?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
  Ptr<ID3D12Resource> resource;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,state,nullptr,IID_PPV_ARGS(&resource)),"test buffer");return resource;
 }
 void Transition(ID3D12Resource* resource,D3D12_RESOURCE_STATES before,D3D12_RESOURCE_STATES after){D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition={resource,D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES,before,after};list->ResourceBarrier(1,&b);}
 void Submit(){
  Check(list->Close(),"test list close");ID3D12CommandList* commands[]={list.Get()};queue->ExecuteCommandLists(1,commands);Check(queue->Signal(fence.Get(),++value),"test signal");
  HANDLE event=CreateEventW(nullptr,FALSE,FALSE,nullptr);Require(event!=nullptr,"test event");Check(fence->SetEventOnCompletion(value,event),"test fence registration");
  Require(WaitForSingleObject(event,30000)==WAIT_OBJECT_0&&fence->GetCompletedValue()!=UINT64_MAX&&fence->GetCompletedValue()>=value&&SUCCEEDED(device->GetDeviceRemovedReason()),"test GPU completion");CloseHandle(event);
  Check(allocator->Reset(),"test allocator reset");Check(list->Reset(allocator.Get(),nullptr),"test list reset");uploads.clear();
 }
 Ptr<ID3D12Resource> Texture(UINT w,UINT h,DXGI_FORMAT format,bool output,UINT seed){
  D3D12_HEAP_PROPERTIES hp{};hp.Type=D3D12_HEAP_TYPE_DEFAULT;D3D12_RESOURCE_DESC desc{};desc.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;desc.Width=w;desc.Height=h;desc.DepthOrArraySize=desc.MipLevels=1;desc.SampleDesc.Count=1;desc.Format=format;desc.Flags=output?D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS:D3D12_RESOURCE_FLAG_NONE;
  Ptr<ID3D12Resource> resource;Check(device->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,output?D3D12_RESOURCE_STATE_UNORDERED_ACCESS:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&resource)),"test texture");
  if(!output){
   D3D12_PLACED_SUBRESOURCE_FOOTPRINT placed{};UINT64 bytes{};device->GetCopyableFootprints(&desc,0,1,0,&placed,nullptr,nullptr,&bytes);
   auto upload=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(upload->Map(0,nullptr,&mapped),"test texture upload map");std::memset(mapped,0,size_t(bytes));
   const UINT components=format==DXGI_FORMAT_R32_FLOAT?1u:4u;
   for(UINT y=0;y<h;++y)for(UINT x=0;x<w;++x)for(UINT c=0;c<components;++c){float v=components==1?2.0f:(c==3?1.0f:float((x*7+y*11+c*13+seed*17)%97)/64.f);std::memcpy(static_cast<unsigned char*>(mapped)+y*placed.Footprint.RowPitch+(x*components+c)*4,&v,4);}
   upload->Unmap(0,nullptr);D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=resource.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;src.pResource=upload.Get();src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;src.PlacedFootprint=placed;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);Transition(resource.Get(),D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);uploads.push_back(upload);
  }
  return resource;
 }
 static void FloatWord(UINT& word,float value){std::memcpy(&word,&value,4);}
public:
 ShaderTestDispatch(ID3D12Device* d,const wchar_t* name,const D3D_SHADER_MACRO* macros):device(d){
  D3D12_COMMAND_QUEUE_DESC qd{};Check(device->CreateCommandQueue(&qd,IID_PPV_ARGS(&queue)),"test queue");Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&allocator)),"test allocator");Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,allocator.Get(),nullptr,IID_PPV_ARGS(&list)),"test list");Check(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence)),"test fence");
  D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,24,D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE,0};Check(device->CreateDescriptorHeap(&hd,IID_PPV_ARGS(&heap)),"test heap");stride=device->GetDescriptorHandleIncrementSize(hd.Type);
  auto enabled=[&](const char* key){if(macros)for(auto*m=macros;m->Name;++m)if(!std::strcmp(m->Name,key))return !std::strcmp(m->Definition,"1");return false;};
  const bool encode=!wcscmp(name,L"native_codec_encode.hlsl"),decode=!wcscmp(name,L"native_codec_decode.hlsl");
  const bool rgbInput=!wcscmp(name,L"native_game_rgb_input.hlsl"),reflect=!wcscmp(name,L"native_rgb_reflect.hlsl");
  rgbOutputs=rgbInput;untiled=rgbInput&&enabled("NATIVE_RGB_NO_TILES");
  textureOutput=encode||(decode&&!enabled("NATIVE_CODEC_UINT_OUT")&&!enabled("NATIVE_CODEC_R11_OUT")&&!enabled("NATIVE_CODEC_UNORM8_OUT"))||(!decode&&!rgbInput&&!reflect);
  for(UINT i=0;i<5;++i){
   if(i==0&&!encode&&!decode&&!rgbInput){
    const UINT element=reflect?16:4;const UINT64 bytes=UINT64(width)*paddedHeight*(reflect?16:12);inputs[i]=Buffer(bytes,D3D12_HEAP_TYPE_UPLOAD,D3D12_RESOURCE_STATE_GENERIC_READ);void* mapped=nullptr;Check(inputs[i]->Map(0,nullptr,&mapped),"test structured input");for(size_t n=0;n<bytes/4;++n){float v=float(n%97)/64;std::memcpy(static_cast<unsigned char*>(mapped)+n*4,&v,4);}inputs[i]->Unmap(0,nullptr);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.ViewDimension=D3D12_SRV_DIMENSION_BUFFER;view.Buffer.NumElements=UINT(bytes/element);view.Buffer.StructureByteStride=element;device->CreateShaderResourceView(inputs[i].Get(),&view,Cpu(i));
   }else{
    auto format=i==4?DXGI_FORMAT_R32_FLOAT:DXGI_FORMAT_R32G32B32A32_FLOAT;inputs[i]=Texture(i==4?1:width,i==4?1:height,format,false,i);
    D3D12_SHADER_RESOURCE_VIEW_DESC view{};view.Shader4ComponentMapping=D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;view.Format=format;view.ViewDimension=D3D12_SRV_DIMENSION_TEXTURE2D;view.Texture2D.MipLevels=1;device->CreateShaderResourceView(inputs[i].Get(),&view,Cpu(i));
   }
  }
  for(UINT i=0;i<2;++i){
   D3D12_UNORDERED_ACCESS_VIEW_DESC view{};
   if(textureOutput&&i==0){outputs[i]=Texture(width,height,DXGI_FORMAT_R16G16B16A16_FLOAT,true,0);view.Format=DXGI_FORMAT_R16G16B16A16_FLOAT;view.ViewDimension=D3D12_UAV_DIMENSION_TEXTURE2D;auto desc=outputs[i]->GetDesc();device->GetCopyableFootprints(&desc,0,1,0,&footprint,nullptr,nullptr,&outputBytes);}
   else{
    const bool raw=decode;UINT64 bytes=raw?UINT64(1920)*height*8:UINT64(width)*paddedHeight*16;
    outputs[i]=Buffer(bytes,D3D12_HEAP_TYPE_DEFAULT,D3D12_RESOURCE_STATE_UNORDERED_ACCESS,true);view.ViewDimension=D3D12_UAV_DIMENSION_BUFFER;view.Format=raw?DXGI_FORMAT_R32_TYPELESS:DXGI_FORMAT_UNKNOWN;view.Buffer.NumElements=UINT(bytes/(raw?4:16));view.Buffer.StructureByteStride=raw?0:16;view.Buffer.Flags=raw?D3D12_BUFFER_UAV_FLAG_RAW:D3D12_BUFFER_UAV_FLAG_NONE;if(i==0)outputBytes=bytes;
   }
   device->CreateUnorderedAccessView(outputs[i].Get(),nullptr,&view,Cpu(8+i));
  }
  readback=Buffer(outputBytes,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  if(rgbOutputs)postReadback=Buffer(outputs[1]->GetDesc().Width,D3D12_HEAP_TYPE_READBACK,D3D12_RESOURCE_STATE_COPY_DEST);
  constants={width,height,width,height,0,0,width,height,0,0,0,1};FloatWord(constants[8],0.9f);FloatWord(constants[9],1.0f);FloatWord(constants[10],0.4f);
  FloatWord(constants[12],2.f);FloatWord(constants[13],3.f);FloatWord(constants[14],float(width-4));FloatWord(constants[15],float(height-6));constants[16]=width*(enabled("NATIVE_CODEC_UINT_OUT")?8:4);FloatWord(constants[18],1.f);FloatWord(constants[19],1.f);
  if(reflect){constants[0]=width;constants[1]=height;constants[2]=width;constants[3]=paddedHeight;}
  Submit();
 }
 std::vector<unsigned char> Run(ID3D12RootSignature* root,ID3D12PipelineState* pso,const wchar_t* name,UINT sample){
  if(copied)Transition(outputs[0].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  if(copied&&rgbOutputs)Transition(outputs[1].Get(),D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
  ID3D12DescriptorHeap* heaps[]={heap.Get()};list->SetDescriptorHeaps(1,heaps);const UINT clear[]={0,0,0,0};
  for(UINT i=0;i<2;++i)list->ClearUnorderedAccessViewUint(Gpu(8+i),Cpu(8+i),outputs[i].Get(),clear,0,nullptr);
  D3D12_RESOURCE_BARRIER barrier{};barrier.Type=D3D12_RESOURCE_BARRIER_TYPE_UAV;list->ResourceBarrier(1,&barrier);
  auto words=constants;
  if(sample==1){words[17]=0x10000;FloatWord(words[18],2.f);FloatWord(words[19],16.f);}else if(sample==2){words[17]=4;FloatWord(words[18],0.5f);FloatWord(words[19],0.f);}
  list->SetComputeRootSignature(root);list->SetPipelineState(pso);list->SetComputeRootDescriptorTable(0,Gpu(0));list->SetComputeRoot32BitConstants(1,20,words.data(),0);
  const bool tiled=!wcscmp(name,L"native_game_rgb_input.hlsl")||!wcscmp(name,L"native_rgb_reflect.hlsl");list->Dispatch(tiled?width/8:(width+15)/16,tiled?paddedHeight/8:(height+15)/16,1);
  Transition(outputs[0].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);
  if(textureOutput){D3D12_TEXTURE_COPY_LOCATION dst{},src{};dst.pResource=readback.Get();dst.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;dst.PlacedFootprint=footprint;src.pResource=outputs[0].Get();src.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;list->CopyTextureRegion(&dst,0,0,0,&src,nullptr);}else list->CopyResource(readback.Get(),outputs[0].Get());
  if(rgbOutputs){Transition(outputs[1].Get(),D3D12_RESOURCE_STATE_UNORDERED_ACCESS,D3D12_RESOURCE_STATE_COPY_SOURCE);list->CopyResource(postReadback.Get(),outputs[1].Get());}
  Submit();copied=true;void* mapped=nullptr;Check(readback->Map(0,nullptr,&mapped),"read output");std::vector<unsigned char> result;
  if(textureOutput){for(UINT y=0;y<height;++y){auto* row=static_cast<unsigned char*>(mapped)+y*footprint.Footprint.RowPitch;result.insert(result.end(),row,row+width*8);}}else{auto* bytes=static_cast<unsigned char*>(mapped);result.assign(bytes,bytes+size_t(outputBytes));}readback->Unmap(0,nullptr);
  auto nonzero=[](const std::vector<unsigned char>& bytes){for(auto b:bytes)if(b)return true;return false;};
  Require(untiled?!nonzero(result):nonzero(result),"primary shader output is written (untiled u0 intentionally unused)");
  if(rgbOutputs){
   Check(postReadback->Map(0,nullptr,&mapped),"read RGB post_base");
   auto* bytes=static_cast<unsigned char*>(mapped);std::vector<unsigned char> post(bytes,bytes+size_t(outputs[1]->GetDesc().Width));postReadback->Unmap(0,nullptr);
   Require(nonzero(post),"RGB post_base output is written");result.insert(result.end(),post.begin(),post.end());
  }
  return result;
 }
};
