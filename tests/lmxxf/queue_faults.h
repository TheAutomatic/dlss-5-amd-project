#pragma once
#include <atomic>

// Harness-only COM wrapper. Execution is real; Signal failure is deterministic
// without removing a physical GPU or introducing a player environment switch.
class SignalFailQueue final:public ID3D12CommandQueue
{
    std::atomic<ULONG> refs{1};
    ID3D12CommandQueue* inner;
public:
    explicit SignalFailQueue(ID3D12CommandQueue* queue):inner(queue){inner->AddRef();}
    ~SignalFailQueue(){inner->Release();}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid,void** out)override{
        if(!out)return E_POINTER;*out=nullptr;
        if(iid!=__uuidof(IUnknown)&&iid!=__uuidof(ID3D12CommandQueue))return E_NOINTERFACE;
        *out=static_cast<ID3D12CommandQueue*>(this);AddRef();return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
    ULONG STDMETHODCALLTYPE Release()override{auto n=--refs;if(!n)delete this;return n;}
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g,UINT* n,void* p)override{return inner->GetPrivateData(g,n,p);}
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g,UINT n,const void* p)override{return inner->SetPrivateData(g,n,p);}
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g,const IUnknown* p)override{return inner->SetPrivateDataInterface(g,p);}
    HRESULT STDMETHODCALLTYPE SetName(LPCWSTR n)override{return inner->SetName(n);}
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID iid,void** out)override{return inner->GetDevice(iid,out);}
    void STDMETHODCALLTYPE UpdateTileMappings(ID3D12Resource* r,UINT n,const D3D12_TILED_RESOURCE_COORDINATE* c,
        const D3D12_TILE_REGION_SIZE* s,ID3D12Heap* h,UINT m,const D3D12_TILE_RANGE_FLAGS* f,const UINT* o,const UINT* z,
        D3D12_TILE_MAPPING_FLAGS flags)override{inner->UpdateTileMappings(r,n,c,s,h,m,f,o,z,flags);}
    void STDMETHODCALLTYPE CopyTileMappings(ID3D12Resource* d,const D3D12_TILED_RESOURCE_COORDINATE* dc,
        ID3D12Resource* s,const D3D12_TILED_RESOURCE_COORDINATE* sc,const D3D12_TILE_REGION_SIZE* n,
        D3D12_TILE_MAPPING_FLAGS f)override{inner->CopyTileMappings(d,dc,s,sc,n,f);}
    void STDMETHODCALLTYPE ExecuteCommandLists(UINT n,ID3D12CommandList*const* lists)override{inner->ExecuteCommandLists(n,lists);}
    void STDMETHODCALLTYPE SetMarker(UINT m,const void* d,UINT n)override{inner->SetMarker(m,d,n);}
    void STDMETHODCALLTYPE BeginEvent(UINT m,const void* d,UINT n)override{inner->BeginEvent(m,d,n);}
    void STDMETHODCALLTYPE EndEvent()override{inner->EndEvent();}
    HRESULT STDMETHODCALLTYPE Signal(ID3D12Fence*,UINT64)override{return E_FAIL;}
    HRESULT STDMETHODCALLTYPE Wait(ID3D12Fence* f,UINT64 n)override{return inner->Wait(f,n);}
    HRESULT STDMETHODCALLTYPE GetTimestampFrequency(UINT64* f)override{return inner->GetTimestampFrequency(f);}
    HRESULT STDMETHODCALLTYPE GetClockCalibration(UINT64* g,UINT64* c)override{return inner->GetClockCalibration(g,c);}
    D3D12_COMMAND_QUEUE_DESC STDMETHODCALLTYPE GetDesc()override{return inner->GetDesc();}
};
