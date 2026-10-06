#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <filesystem>
#include <thread>
#include <cassert>
#include <cstdio>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfShaderCompiler.h"

static void Require(bool ok,const char* what){if(!ok){std::fprintf(stderr,"FAIL: %s\n",what);std::exit(1);}}
static void Check(HRESULT hr,const char* what){if(FAILED(hr)){std::fprintf(stderr,"FAIL: %s hr=0x%08x\n",what,unsigned(hr));std::exit(1);}}
#include "shader_test_dispatch.h"
static const char shader[]="RWStructuredBuffer<float> outData:register(u0);[numthreads(1,1,1)]void main(uint3 p:SV_DispatchThreadID){outData[p.x]=float(p.x)+2;}";
static void Write(const std::filesystem::path& path,const std::string& text){std::ofstream file(path,std::ios::binary);file<<text;Require(bool(file),"write fixture");}

static void PolicyTests(){
 for(int mode=0;mode<4;++mode){
  int calls=0;ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
  auto attempt=[&](const char* target,ID3DBlob**,ID3DBlob** error){
   ++calls;
   if(calls==1){LmxxfShader::NativeShaderSetError(error,mode==0?"error X3000: syntax error":"error X3506: unrecognized compiler target 'cs_5_1'");return E_FAIL;}
   Require(!std::strcmp(target,"cs_5_0"),"only known fallback target");
   if(mode==2){LmxxfShader::NativeShaderSetError(error,"error X3000: second failure");return E_FAIL;}return S_OK;
  };
  HRESULT hr=LmxxfShader::NativeCompileShaderPolicy(attempt,"cs_5_1",mode!=3,&code,&errors);
  Require(calls==((mode==0||mode==3)?1:2),"target-only and opt-in fallback");
  if(mode==2){auto text=LmxxfShader::NativeShaderErrorText(errors);Require(text.find("X3506")!=std::string::npos&&text.find("second failure")!=std::string::npos,"both failure diagnostics retained");}
  Require((mode==1)==SUCCEEDED(hr),"policy result");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 }
}
static void IncludeTests(const std::filesystem::path& root){
 auto folder=root/L"\u7f16\u8bd1\u5668 include tests";std::filesystem::create_directories(folder/L"nested");
 auto source=folder/L"include shader.hlsl";
 Write(folder/L"nested/leaf.hlsli","float value(){return 4;}\n");
 Write(folder/L"child.hlsli","#include \"nested/leaf.hlsli\"\n");
 Write(source,"#include \"child.hlsli\"\nRWStructuredBuffer<float> outData:register(u0);[numthreads(1,1,1)]void main(){outData[0]=value();}");
 ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
 Check(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors),"uncached relative include from another CWD");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 // Presence of the snapshotted header takes the dependency.unknown branch.
 Write(folder/L"native_half_square.hlsli","float square(float x){return x*x;}");
 Check(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors),"unknown include fallback uses private file compiler");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 Write(source,"#include \"native_half_square.hlsli\"\nRWStructuredBuffer<float> outData:register(u0);[numthreads(1,1,1)]void main(){outData[0]=square(3);}");
 Check(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors),"snapshotted include");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 auto compiles=LmxxfShader::NativeShaderCache().compiles;
 Write(folder/L"native_half_square.hlsli","float square(float x){return x*x+1;}");
 Check(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors),"changed include");
 Require(LmxxfShader::NativeShaderCache().compiles==compiles+1,"include contents isolate cache");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 Write(source,"#include \"missing.hlsli\"\n"+std::string(shader));
 Require(FAILED(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors)),"missing include fails");
 Require(LmxxfShader::NativeShaderErrorText(errors).find("missing.hlsli")!=std::string::npos,"missing include diagnostic retained");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
}
static HMODULE WINAPI FailLoad(LPCWSTR,HANDLE,DWORD){SetLastError(ERROR_MOD_NOT_FOUND);return nullptr;}
static FARPROC WINAPI MissingSymbol(HMODULE module,LPCSTR name){return !std::strcmp(name,"D3DCompileFromFile")?nullptr:GetProcAddress(module,name);}
static void FailureTests(){
 for(auto result:{LmxxfShader::NativeResolveShaderCompiler(&FailLoad),LmxxfShader::NativeResolveShaderCompiler(&LoadLibraryExW,&MissingSymbol)}){
  Require(FAILED(result.status)&&!result.module&&!result.compile&&!result.compileFile&&!result.createBlob&&!result.error.empty(),"loader failure never publishes partial bindings");
  ID3DBlob* code=nullptr;ID3DBlob* error=nullptr;
  Require(FAILED(LmxxfShader::NativeCompileShaderWithCompiler(result,L"unused",nullptr,"main",&code,&error))&&!code&&error,"loader failure retains error without compiler");LmxxfShader::NativeShaderRelease(error);
 }
 D3D_SHADER_MACRO bad[]={{"NATIVE_CODEC_UINT_OUT","1"},{"NATIVE_CODEC_R11_OUT","1"},{nullptr,nullptr}};
 Require(!LmxxfShader::NativeShaderAllows50(L"native_codec_decode.hlsl","main",bad),"untested output combination cannot downgrade");
 Require(!LmxxfShader::NativeShaderAllows50(L"unknown.hlsl","main",nullptr),"unknown shader cannot downgrade");
}
static void CacheTests(const std::filesystem::path& folder){
 const auto& compiler=LmxxfShader::NativePrivateShaderCompiler();
 auto source=folder/L"cache-negative.hlsl";Write(source,shader);
 const auto key=LmxxfShader::NativeShaderCacheKey(compiler,shader,"main",nullptr,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3);
 const auto hash=LmxxfShader::NativeShaderHex(LmxxfShader::NativeShaderHash(key.data(),key.size()));
 const auto path=folder/L"shader-cache"/(std::wstring(hash.begin(),hash.end())+L".v2.dxbc");
 auto other=compiler;other.identity+="different compiler";
 std::vector<std::string> wrongKeys={
  LmxxfShader::NativeShaderCacheKey(compiler,shader,"main",nullptr,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3),
  LmxxfShader::NativeShaderCacheKey(compiler,shader,"main",nullptr,"cs_5_1",D3DCOMPILE_DEBUG),
  LmxxfShader::NativeShaderCacheKey(other,shader,"main",nullptr,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3)};
 // Wrong identities are placed at the expected filename, exercising full-key
 // validation even in a filename-hash collision, followed by real recompilation.
 for(int mode=0;mode<5;++mode){
  LmxxfShader::NativeShaderCache().entries.clear();
  if(mode<3)LmxxfShader::NativeWriteShaderCache(path.wstring(),wrongKeys[mode],{1,2,3,4});
  else if(mode==3)Write(path,"truncated");
  else{
   LmxxfShader::NativeWriteShaderCache(path.wstring(),key,{1,2,3,4});
   std::fstream file(path,std::ios::binary|std::ios::in|std::ios::out);file.seekp(-1,std::ios::end);file.put(9);
  }
  const auto before=LmxxfShader::NativeShaderCache().compiles,hits=LmxxfShader::NativeShaderCache().diskHits;ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
  Check(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors),"invalid cache recompiles");
  Require(LmxxfShader::NativeShaderCache().compiles==before+1&&LmxxfShader::NativeShaderCache().diskHits==hits,"invalid cache must miss");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 }
}
static HRESULT WINAPI TargetReject(LPCVOID data,SIZE_T size,LPCSTR name,const D3D_SHADER_MACRO* macros,ID3DInclude* include,LPCSTR entry,LPCSTR target,UINT flags,UINT flags2,ID3DBlob** code,ID3DBlob** error){
 if(!std::strcmp(target,"cs_5_1")){LmxxfShader::NativeShaderSetError(error,"error X3506: unrecognized compiler target 'cs_5_1'");return E_FAIL;}
 return LmxxfShader::NativePrivateShaderCompiler().compile(data,size,name,macros,include,entry,target,flags,flags2,code,error);
}
static void FallbackCacheTests(const std::filesystem::path& folder){
 auto source=folder/L"native_rgb_texture.hlsl";Write(source,shader);
 auto compiler=LmxxfShader::NativePrivateShaderCompiler();compiler.compile=&TargetReject;compiler.identity+="|harness-target-reject";
 for(int pass=0;pass<3;++pass){
  if(pass==2)LmxxfShader::NativeShaderCache().entries.clear();
  auto before=LmxxfShader::NativeShaderCache().compiles,hits=LmxxfShader::NativeShaderCache().hits;
  ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
  Check(LmxxfShader::NativeCompileShaderWithCompiler(compiler,source.wstring(),nullptr,"main",&code,&errors),"fallback compile/cache");
  Require(code&&LmxxfShader::NativeShaderErrorText(errors).find("X3506")!=std::string::npos,"fallback retains original error on success");
  Require(LmxxfShader::NativeShaderCache().compiles==before+(pass?1:2)&&LmxxfShader::NativeShaderCache().hits==hits+(pass?1:0),"fallback uses effective-target memory/disk key");
  LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 }
}
static void PsoMatrix(const std::filesystem::path& shaders){
 IDXGIFactory4* factory=nullptr;IDXGIAdapter* warp=nullptr;ID3D12Device* device=nullptr;
 Check(CreateDXGIFactory1(IID_PPV_ARGS(&factory)),"factory");Check(factory->EnumWarpAdapter(IID_PPV_ARGS(&warp)),"WARP");
 Check(D3D12CreateDevice(warp,D3D_FEATURE_LEVEL_11_0,IID_PPV_ARGS(&device)),"WARP device");warp->Release();factory->Release();
 D3D12_DESCRIPTOR_RANGE ranges[2]={{D3D12_DESCRIPTOR_RANGE_TYPE_SRV,8,0,0,0},{D3D12_DESCRIPTOR_RANGE_TYPE_UAV,8,0,0,8}};
 D3D12_ROOT_PARAMETER parameter[2]{};parameter[0].ParameterType=D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;parameter[0].DescriptorTable={2,ranges};parameter[1].ParameterType=D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;parameter[1].Constants={0,0,20};
 D3D12_ROOT_SIGNATURE_DESC desc{};desc.NumParameters=2;desc.pParameters=parameter;
 ID3DBlob* rootBlob=nullptr;ID3DBlob* error=nullptr;Check(D3D12SerializeRootSignature(&desc,D3D_ROOT_SIGNATURE_VERSION_1,&rootBlob,&error),"root serialize");LmxxfShader::NativeShaderRelease(error);
 ID3D12RootSignature* root=nullptr;Check(device->CreateRootSignature(0,rootBlob->GetBufferPointer(),rootBlob->GetBufferSize(),IID_PPV_ARGS(&root)),"root");rootBlob->Release();
 unsigned variants=0;
 auto compile=[&](const wchar_t* name,const D3D_SHADER_MACRO* macros){
  Require(LmxxfShader::NativeShaderAllows50(name,"main",macros),"tested macro combination allowed");
  auto path=shaders/name;
  ShaderTestDispatch dispatch(device,name,macros);
  std::array<std::vector<unsigned char>,3> expected;
  for(const char* target:{"cs_5_1","cs_5_0"}){
   ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
   HRESULT hr=LmxxfShader::NativePrivateShaderCompiler().compileFile(path.c_str(),macros,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main",target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors);
   if(errors)std::fprintf(stderr,"%s",LmxxfShader::NativeShaderErrorText(errors).c_str());Check(hr,"production shader matrix compile");LmxxfShader::NativeShaderRelease(errors);
   D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc{};psoDesc.pRootSignature=root;psoDesc.CS={code->GetBufferPointer(),code->GetBufferSize()};ID3D12PipelineState* pso=nullptr;
   Check(device->CreateComputePipelineState(&psoDesc,IID_PPV_ARGS(&pso)),"production shader real PSO");
   for(UINT sample=0;sample<3;++sample){auto output=dispatch.Run(root,pso,name,sample);if(!std::strcmp(target,"cs_5_1"))expected[sample]=std::move(output);else Require(output==expected[sample],"cs_5_0 vs cs_5_1 exact GPU output");}
   pso->Release();code->Release();++variants;
  }
 };
 for(const char* exposure:{"0","1"})for(const char* fit:{"0","1"})for(const char* srgb:{"0","1"}){
  D3D_SHADER_MACRO plain[]={{"NATIVE_CODEC_EXPOSURE",exposure},{"NATIVE_CODEC_FIT",fit},{"NATIVE_CODEC_SRGB_IO",srgb},{nullptr,nullptr}};
  compile(L"native_codec_encode.hlsl",plain);compile(L"native_codec_decode.hlsl",plain);
  for(const char* mode:{"NATIVE_CODEC_UINT_OUT","NATIVE_CODEC_R11_OUT"}){
   D3D_SHADER_MACRO macros[]={{"NATIVE_CODEC_EXPOSURE",exposure},{"NATIVE_CODEC_FIT",fit},{"NATIVE_CODEC_SRGB_IO",srgb},{mode,"1"},{nullptr,nullptr}};compile(L"native_codec_decode.hlsl",macros);
  }
  for(const char* bgra:{"0","1"})for(const char* tint:{"0","1"}){
   D3D_SHADER_MACRO macros[]={{"NATIVE_CODEC_EXPOSURE",exposure},{"NATIVE_CODEC_FIT",fit},{"NATIVE_CODEC_SRGB_IO",srgb},{"NATIVE_CODEC_UNORM8_OUT","1"},{"NATIVE_CODEC_BGRA",bgra},{"NATIVE_CODEC_DEBUG_TINT",tint},{nullptr,nullptr}};compile(L"native_codec_decode.hlsl",macros);
  }
 }
 for(const char* tiles:{"0","1"}){D3D_SHADER_MACRO macros[]={{"NATIVE_RGB_NO_TILES",tiles},{nullptr,nullptr}};compile(L"native_game_rgb_input.hlsl",macros);}
 compile(L"native_rgb_texture.hlsl",nullptr);compile(L"native_rgb_reflect.hlsl",nullptr);
 root->Release();device->Release();std::printf("PSO shader/target variants: %u\n",variants);
}

int wmain(int argc,wchar_t** argv){
 Require(argc==4,"arguments: fixture-root cold|warm shader-root");
 std::filesystem::path folder=argv[1];bool warm=!wcscmp(argv[2],L"warm");
 std::filesystem::create_directories(folder);
 auto fake=std::filesystem::absolute(folder/L"game/d3dcompiler_47.dll");
 HMODULE old=LoadLibraryW(fake.c_str());Require(old!=nullptr,"preload game compiler");
 auto oldCompile=GetProcAddress(old,"D3DCompile");auto oldCalls=reinterpret_cast<UINT(*)()>(GetProcAddress(old,"OldCompilerCalls"));Require(oldCompile&&oldCalls,"old compiler exports");
 auto concurrent=folder/L"concurrent.hlsl";Write(concurrent,shader);
 std::thread threads[8];for(int i=0;i<8;++i)threads[i]=std::thread([&,i]{
  const auto& compiler=LmxxfShader::NativePrivateShaderCompiler();Require(SUCCEEDED(compiler.status),compiler.error.c_str());
  ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
  HRESULT hr=i%2?compiler.compileFile(concurrent.c_str(),nullptr,D3D_COMPILE_STANDARD_FILE_INCLUDE,"main","cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3,0,&code,&errors):
   LmxxfShader::NativeCompileShaderBlob(shader,sizeof shader-1,"threaded",nullptr,nullptr,"main",&code,&errors);
  Check(hr,"concurrent first memory/file compilation");Require(code!=nullptr,"concurrent bytecode");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 });for(auto& thread:threads)thread.join();
 const auto& compiler=LmxxfShader::NativePrivateShaderCompiler();Require(compiler.module!=old,"system module is distinct");
 Require(reinterpret_cast<FARPROC>(compiler.compile)!=oldCompile,"private compile symbol");
 auto source=folder/L"cache.hlsl";
 if(!warm)Write(source,shader);
 ID3DBlob* code=nullptr;ID3DBlob* errors=nullptr;
 Check(LmxxfShader::CompileNativeShader(source.wstring(),nullptr,"main",&code,&errors),"cache shader");LmxxfShader::NativeShaderRelease(code);LmxxfShader::NativeShaderRelease(errors);
 Require(LmxxfShader::NativeShaderCache().diskHits==(warm?1u:0u),"separate-process disk cache identity");Require(LmxxfShader::NativeShaderCache().compiles==(warm?0u:1u),"cold compilation and warm no-compilation");
 auto key=LmxxfShader::NativeShaderCacheKey(compiler,shader,"main",nullptr,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3);
 Require(key!=LmxxfShader::NativeShaderCacheKey(compiler,shader,"main",nullptr,"cs_5_0",D3DCOMPILE_OPTIMIZATION_LEVEL3),"cache target isolation");
 Require(key!=LmxxfShader::NativeShaderCacheKey(compiler,shader,"main",nullptr,"cs_5_1",D3DCOMPILE_DEBUG),"cache flags isolation");
 auto other=compiler;other.identity+="different compiler";Require(key!=LmxxfShader::NativeShaderCacheKey(other,shader,"main",nullptr,"cs_5_1",D3DCOMPILE_OPTIMIZATION_LEVEL3),"cache compiler isolation");
 if(!warm){PolicyTests();FailureTests();IncludeTests(folder);CacheTests(folder);FallbackCacheTests(folder);PsoMatrix(argv[3]);}
 Require(GetProcAddress(old,"D3DCompile")==oldCompile&&oldCalls()==0,"game compiler binding preserved and never used");FreeLibrary(old);
 std::puts(warm?"shader compiler warm: PASS":"shader compiler cold: PASS");
}
