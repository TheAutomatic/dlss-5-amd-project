#pragma once
#include "nr_effects_test_utils.h"
#include <array>

// Explicit FP32 upload/readback keeps synthetic guide values independent of
// game resources and permits NaN, depth discontinuities, and subpixel motion.
using Pixel = std::array<float, 4>;
static Ptr<ID3D12Resource> FloatTexture(ID3D12Device* d, UINT w, UINT h)
{
    auto desc=Effects::Storage::Description(w,h);desc.Format=DXGI_FORMAT_R32G32B32A32_FLOAT;
    D3D12_HEAP_PROPERTIES hp {};hp.Type=D3D12_HEAP_TYPE_DEFAULT;Ptr<ID3D12Resource> r;
    Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&desc,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,nullptr,IID_PPV_ARGS(&r)),"float texture");return r;
}
static std::vector<Pixel> Transfer(ID3D12Device* d, ID3D12CommandQueue* q, ID3D12Resource* r, const std::vector<Pixel>* upload=nullptr)
{
    auto desc=r->GetDesc();D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};UINT64 bytes=0;
    d->GetCopyableFootprints(&desc,0,1,0,&fp,nullptr,nullptr,&bytes);
    D3D12_RESOURCE_DESC bd {};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=bytes;
    bd.Height=1;bd.DepthOrArraySize=bd.MipLevels=1;bd.SampleDesc.Count=1;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    D3D12_HEAP_PROPERTIES hp {};hp.Type=upload?D3D12_HEAP_TYPE_UPLOAD:D3D12_HEAP_TYPE_READBACK;Ptr<ID3D12Resource> buf;
    Check(d->CreateCommittedResource(&hp,D3D12_HEAP_FLAG_NONE,&bd,upload?D3D12_RESOURCE_STATE_GENERIC_READ:D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&buf)),"transfer buffer");
    if(upload){Require(desc.Format==DXGI_FORMAT_R32G32B32A32_FLOAT,"FP32 upload format");void* ptr=nullptr;D3D12_RANGE empty {0,0};Check(buf->Map(0,&empty,&ptr),"upload map");
        for(UINT y=0;y<desc.Height;++y)std::memcpy(static_cast<char*>(ptr)+size_t(y)*fp.Footprint.RowPitch,upload->data()+size_t(y)*desc.Width,size_t(desc.Width)*sizeof(Pixel));buf->Unmap(0,nullptr);}
    Ptr<ID3D12CommandAllocator> a;Ptr<ID3D12GraphicsCommandList> c;
    Check(d->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&a)),"transfer allocator");
    Check(d->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,a.Get(),nullptr,IID_PPV_ARGS(&c)),"transfer list");
    auto state=upload?D3D12_RESOURCE_STATE_COPY_DEST:D3D12_RESOURCE_STATE_COPY_SOURCE;
    Effects::Barrier(c.Get(),r,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,state);
    D3D12_TEXTURE_COPY_LOCATION tex {},buffer {};tex.pResource=r;tex.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    buffer.pResource=buf.Get();buffer.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;buffer.PlacedFootprint=fp;
    if(upload)c->CopyTextureRegion(&tex,0,0,0,&buffer,nullptr);else c->CopyTextureRegion(&buffer,0,0,0,&tex,nullptr);
    Effects::Barrier(c.Get(),r,state,D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);Check(c->Close(),"transfer close");
    ID3D12CommandList* lists[]={c.Get()};q->ExecuteCommandLists(1,lists);WaitQueue(d,q);
    std::vector<Pixel> out(size_t(desc.Width)*desc.Height);
    if(!upload){void* ptr=nullptr;D3D12_RANGE range {0,SIZE_T(bytes)};Check(buf->Map(0,&range,&ptr),"read map");
        for(UINT y=0;y<desc.Height;++y){
            const auto* row=static_cast<const char*>(ptr)+size_t(y)*fp.Footprint.RowPitch;
            if(desc.Format==DXGI_FORMAT_R32G32B32A32_FLOAT)std::memcpy(out.data()+size_t(y)*desc.Width,row,size_t(desc.Width)*sizeof(Pixel));
            else {
                Require(desc.Format==DXGI_FORMAT_R16G16B16A16_FLOAT,"FP16 readback format");
                const auto* half=reinterpret_cast<const UINT16*>(row);
                for(UINT x=0;x<desc.Width;++x)for(unsigned c=0;c<4;++c)out[size_t(y)*desc.Width+x][c]=Half(half[x*4+c]);
            }
        }
        D3D12_RANGE empty {0,0};buf->Unmap(0,&empty);}
    return out;
}
