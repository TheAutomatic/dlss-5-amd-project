#include <windows.h>
#include <d3dcompiler.h>
#include <atomic>
static std::atomic<UINT> calls{0};
extern "C" __declspec(dllexport) UINT OldCompilerCalls(){return calls.load();}
extern "C" __declspec(dllexport) HRESULT WINAPI OldCompile(const void*,SIZE_T,const char*,const D3D_SHADER_MACRO*,
    ID3DInclude*,const char*,const char*,UINT,UINT,ID3DBlob** code,ID3DBlob** errors)
{
    ++calls;
    if(code)*code=nullptr;
    if(errors)*errors=nullptr;
    return E_INVALIDARG;
}
