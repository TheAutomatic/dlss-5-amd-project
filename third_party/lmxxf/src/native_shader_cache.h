#pragma once
#include <windows.h>
#include <d3dcompiler.h>
#include <atomic>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <filesystem>

// No imported D3DCompiler entry points: the game may already have loaded an old
// DLL with the same basename. A full system path and actual symbol ownership
// are both checked before publishing this immutable, process-lifetime binding.
struct NativeShaderCompiler {
 using CompileFn=decltype(&D3DCompile);
 using FileFn=decltype(&D3DCompileFromFile);
 using BlobFn=decltype(&D3DCreateBlob);
 HMODULE module{};
 CompileFn compile{};
 FileFn compileFile{};
 BlobFn createBlob{};
 HRESULT status=E_FAIL;
 std::wstring path;
 std::string identity,error;
};

inline std::string NativeShaderUtf8(const std::wstring& value){
 int n=WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),nullptr,0,nullptr,nullptr);
 if(n<=0)return {};
 std::string out(size_t(n),'\0');
 WideCharToMultiByte(CP_UTF8,WC_ERR_INVALID_CHARS,value.data(),int(value.size()),out.data(),n,nullptr,nullptr);
 return out;
}
inline uint64_t NativeShaderHash(const void* data,size_t size){
 uint64_t h=14695981039346656037ull;
 for(size_t i=0;i<size;++i){h^=static_cast<const unsigned char*>(data)[i];h*=1099511628211ull;}
 return h;
}
inline std::string NativeShaderHex(uint64_t value){char text[17]{};std::snprintf(text,sizeof text,"%016llx",static_cast<unsigned long long>(value));return text;}

// Errors must remain available even when no compiler DLL can be loaded.
class NativeShaderError final:public ID3DBlob {
 std::atomic<ULONG> refs{1};
 std::string text;
public:
 explicit NativeShaderError(std::string value):text(std::move(value)){text.push_back('\0');}
 HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id,void** out)override{
  if(!out)return E_POINTER;*out=nullptr;
  if(id!=__uuidof(IUnknown)&&id!=__uuidof(ID3DBlob))return E_NOINTERFACE;
  *out=static_cast<ID3DBlob*>(this);AddRef();return S_OK;
 }
 ULONG STDMETHODCALLTYPE AddRef()override{return ++refs;}
 ULONG STDMETHODCALLTYPE Release()override{ULONG n=--refs;if(!n)delete this;return n;}
 void* STDMETHODCALLTYPE GetBufferPointer()override{return text.data();}
 SIZE_T STDMETHODCALLTYPE GetBufferSize()override{return text.size();}
};
inline void NativeShaderSetError(ID3DBlob** errors,const std::string& text){if(errors)*errors=new NativeShaderError(text);}
inline std::string NativeShaderErrorText(ID3DBlob* error){
 if(!error)return {};
 std::string text(static_cast<const char*>(error->GetBufferPointer()),error->GetBufferSize());
 while(!text.empty()&&text.back()=='\0')text.pop_back();return text;
}
inline void NativeShaderRelease(ID3DBlob*& blob){if(blob){blob->Release();blob=nullptr;}}

inline std::string NativeShaderFileVersion(HMODULE module){
 HRSRC resource=FindResourceW(module,MAKEINTRESOURCEW(1),MAKEINTRESOURCEW(16));
 if(!resource)return {};
 DWORD size=SizeofResource(module,resource);
 auto* bytes=static_cast<const unsigned char*>(LockResource(LoadResource(module,resource)));
 if(!bytes||size<6)return {};
 WORD length{},valueLength{};std::memcpy(&length,bytes,2);std::memcpy(&valueLength,bytes+2,2);
 if(length>size||valueLength<sizeof(VS_FIXEDFILEINFO))return {};
 const wchar_t expected[]=L"VS_VERSION_INFO";
 if(size<6+sizeof expected||std::memcmp(bytes+6,expected,sizeof expected))return {};
 size_t offset=(6+sizeof expected+3)&~size_t(3);
 if(offset+sizeof(VS_FIXEDFILEINFO)>length)return {};
 VS_FIXEDFILEINFO version{};std::memcpy(&version,bytes+offset,sizeof version);
 if(version.dwSignature!=0xfeef04bd)return {};
 return std::to_string(HIWORD(version.dwFileVersionMS))+"."+std::to_string(LOWORD(version.dwFileVersionMS))+"."+
        std::to_string(HIWORD(version.dwFileVersionLS))+"."+std::to_string(LOWORD(version.dwFileVersionLS));
}
inline NativeShaderCompiler NativeResolveShaderCompiler(
 decltype(&LoadLibraryExW) load=&LoadLibraryExW,decltype(&GetProcAddress) symbol=&GetProcAddress){
 NativeShaderCompiler result;
 wchar_t directory[32768]{};UINT count=GetSystemDirectoryW(directory,32768);
 if(!count||count>=32768){result.error="GetSystemDirectoryW failed";return result;}
 result.path=std::wstring(directory,count)+L"\\d3dcompiler_47.dll";
 result.module=load(result.path.c_str(),nullptr,LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR|LOAD_LIBRARY_SEARCH_SYSTEM32);
 if(!result.module){result.status=HRESULT_FROM_WIN32(GetLastError());result.error="System32 compiler load failed: "+NativeShaderUtf8(result.path);return result;}
 result.compile=reinterpret_cast<NativeShaderCompiler::CompileFn>(symbol(result.module,"D3DCompile"));
 result.compileFile=reinterpret_cast<NativeShaderCompiler::FileFn>(symbol(result.module,"D3DCompileFromFile"));
 result.createBlob=reinterpret_cast<NativeShaderCompiler::BlobFn>(symbol(result.module,"D3DCreateBlob"));
 wchar_t actual[32768]{};DWORD actualSize=GetModuleFileNameW(result.module,actual,32768);
 bool valid=actualSize&&actualSize<32768&&!_wcsicmp(actual,result.path.c_str());
 for(auto fn:{reinterpret_cast<FARPROC>(result.compile),reinterpret_cast<FARPROC>(result.compileFile),reinterpret_cast<FARPROC>(result.createBlob)}){
  HMODULE owner{};
  valid=valid&&fn&&GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                   reinterpret_cast<LPCWSTR>(fn),&owner)&&owner==result.module;
 }
 const auto version=NativeShaderFileVersion(result.module);
 std::ifstream file(result.path.c_str(),std::ios::binary);
 std::string bytes((std::istreambuf_iterator<char>(file)),{});
 valid=valid&&!version.empty()&&file.is_open()&&!file.bad()&&!bytes.empty();
 if(!valid){
  result.error="System32 compiler source, exports or version verification failed: "+NativeShaderUtf8(result.path);
  result.compile=nullptr;result.compileFile=nullptr;result.createBlob=nullptr;
  FreeLibrary(result.module);result.module=nullptr;return result;
 }
 result.identity=NativeShaderUtf8(result.path)+"|version="+version+"|bytes="+std::to_string(bytes.size())+"|hash="+NativeShaderHex(NativeShaderHash(bytes.data(),bytes.size()));
 result.status=S_OK;
 // Keep this module loaded: compile/blob functions and returned COM blobs may
 // remain in use on other threads until process exit. No static unload race.
 std::fprintf(stderr,"lmxxf_shader compiler=%s\n",result.identity.c_str());
 return result;
}
inline const NativeShaderCompiler& NativePrivateShaderCompiler(){
 static const NativeShaderCompiler compiler=NativeResolveShaderCompiler();return compiler;
}

inline bool NativeUnsupportedShaderTarget(HRESULT hr,ID3DBlob* errors,const char* requested){
 if(SUCCEEDED(hr)||!requested||std::strcmp(requested,"cs_5_1"))return false;
 const auto text=NativeShaderErrorText(errors);
 return text.find("error X3506: unrecognized compiler target 'cs_5_1'")!=std::string::npos;
}
// The attempt owns cache selection for the effective target. Tests supply an
// ordinary callable; there is no process-global injection or player switch.
template<class Attempt>
inline HRESULT NativeCompileShaderPolicy(Attempt&& attempt,const char* requested,bool allow50,ID3DBlob** code,ID3DBlob** errors){
 if(!code||!requested)return E_INVALIDARG;*code=nullptr;if(errors)*errors=nullptr;
 ID3DBlob* firstError=nullptr;
 HRESULT first=attempt(requested,code,&firstError);
 if(!allow50||!NativeUnsupportedShaderTarget(first,firstError,requested)){
  if(errors)*errors=firstError;else NativeShaderRelease(firstError);return first;
 }
 NativeShaderRelease(*code);
 const auto original=NativeShaderErrorText(firstError);NativeShaderRelease(firstError);
 ID3DBlob* secondError=nullptr;
 HRESULT second=attempt("cs_5_0",code,&secondError);
 const auto fallback=NativeShaderErrorText(secondError);NativeShaderRelease(secondError);
 std::string diagnostic="requested=cs_5_1 hr=0x"+NativeShaderHex(uint32_t(first))+": "+original+
   "\neffective=cs_5_0 hr=0x"+NativeShaderHex(uint32_t(second))+": "+fallback;
 NativeShaderSetError(errors,diagnostic);
 std::fprintf(stderr,"lmxxf_shader %s\n",diagnostic.c_str());
 return second;
}
inline HRESULT NativeCompileShaderBlob(const void* data,SIZE_T size,const char* name,const D3D_SHADER_MACRO* macros,
 ID3DInclude* include,const char* entry,ID3DBlob** code,ID3DBlob** errors,const char* target="cs_5_1",
 UINT flags=D3DCOMPILE_OPTIMIZATION_LEVEL3){
 if(!code||!entry||!target)return E_INVALIDARG;*code=nullptr;if(errors)*errors=nullptr;
 const auto& compiler=NativePrivateShaderCompiler();
 if(FAILED(compiler.status)){NativeShaderSetError(errors,compiler.error);return compiler.status;}
 return compiler.compile(data,size,name,macros,include,entry,target,flags,0,code,errors);
}

struct NativeShaderCacheState {
 std::mutex mutex;
 std::map<std::string,std::vector<unsigned char>> entries;
 size_t hits{},compiles{},diskHits{};
};
inline NativeShaderCacheState& NativeShaderCache(){static NativeShaderCacheState state;return state;}
struct NativeHalfInclude final:ID3DInclude {
 std::string bytes;bool unknown{};
 HRESULT STDMETHODCALLTYPE Open(D3D_INCLUDE_TYPE type,const char* name,const void*,const void** data,UINT* size)override{
  if(type!=D3D_INCLUDE_LOCAL||!name||std::strcmp(name,"native_half_square.hlsli")){unknown=true;return E_FAIL;}
  *data=bytes.data();*size=UINT(bytes.size());return S_OK;
 }
 HRESULT STDMETHODCALLTYPE Close(const void*)override{return S_OK;}
};
inline void NativeShaderKeyPart(std::string& key,const std::string& part){
 uint64_t size=part.size();key.append(reinterpret_cast<const char*>(&size),sizeof size);key+=part;
}
inline std::string NativeShaderCacheKey(const NativeShaderCompiler& compiler,const std::string& source,const char* entry,
 const D3D_SHADER_MACRO* macros,const char* target,UINT flags,const std::string& includeName={},const std::string& includeBytes={}){
 std::string key="lmxxf-shader-cache-v2";
 for(const auto& part:{compiler.identity,std::string(target),std::to_string(flags),std::string("flags2=0"),source,std::string(entry),includeName,includeBytes})NativeShaderKeyPart(key,part);
 if(macros)for(auto* macro=macros;macro->Name;++macro){NativeShaderKeyPart(key,macro->Name);NativeShaderKeyPart(key,macro->Definition?macro->Definition:"");}
 return key;
}
// Cache files contain the exact key and a payload checksum, not just a hashed
// filename. Torn files and hash collisions are misses, never compiler failures.
inline bool NativeReadShaderCache(const std::wstring& path,const std::string& key,std::vector<unsigned char>& bytes){
 std::ifstream file(path.c_str(),std::ios::binary|std::ios::ate);
 if(!file)return false;
 auto total=file.tellg();file.seekg(0);
 uint64_t header[4]{};
 if(!file.read(reinterpret_cast<char*>(header),sizeof header)||header[0]!=0x324548434143534cull||header[1]!=key.size()||
    !header[2]||header[2]>64*1024*1024||total!=std::streamoff(sizeof header+key.size()+header[2]))return false;
 std::string stored(key.size(),'\0');
 if(!file.read(stored.data(),std::streamsize(stored.size()))||stored!=key)return false;
 bytes.resize(size_t(header[2]));
 if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))return false;
 return header[3]==NativeShaderHash(bytes.data(),bytes.size());
}
inline void NativeWriteShaderCache(const std::wstring& path,const std::string& key,const std::vector<unsigned char>& bytes){
 static std::atomic<uint64_t> sequence{0};
 const auto temporary=path+L"."+std::to_wstring(GetCurrentProcessId())+L"."+std::to_wstring(++sequence)+L".tmp";
 bool written=false;
 {
  std::ofstream file(temporary.c_str(),std::ios::binary|std::ios::trunc);
  uint64_t header[]={0x324548434143534cull,uint64_t(key.size()),uint64_t(bytes.size()),NativeShaderHash(bytes.data(),bytes.size())};
  file.write(reinterpret_cast<const char*>(header),sizeof header);file.write(key.data(),std::streamsize(key.size()));
  file.write(reinterpret_cast<const char*>(bytes.data()),std::streamsize(bytes.size()));file.flush();written=bool(file);
 }
 if(!written||!MoveFileExW(temporary.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))DeleteFileW(temporary.c_str());
}
inline bool NativeShaderAllows50(const std::wstring& path,const char* entry,const D3D_SHADER_MACRO* macros){
 if(std::strcmp(entry,"main"))return false;
 const auto name=std::filesystem::path(path).filename().wstring();
 const bool decode=name==L"native_codec_decode.hlsl",codec=decode||name==L"native_codec_encode.hlsl";
 const bool rgb=name==L"native_game_rgb_input.hlsl";
 if(!codec&&!rgb&&name!=L"native_rgb_texture.hlsl"&&name!=L"native_rgb_reflect.hlsl")return false;
 std::map<std::string,bool> values;
 if(macros)for(auto* m=macros;m->Name;++m){
  if(!m->Definition||(std::strcmp(m->Definition,"0")&&std::strcmp(m->Definition,"1")))return false;
  if(!values.emplace(m->Name,!std::strcmp(m->Definition,"1")).second)return false;
  if(codec){
   bool known=false;for(const char* key:{"NATIVE_CODEC_EXPOSURE","NATIVE_CODEC_FIT","NATIVE_CODEC_UINT_OUT","NATIVE_CODEC_SRGB_IO","NATIVE_CODEC_UNORM8_OUT","NATIVE_CODEC_BGRA","NATIVE_CODEC_DEBUG_TINT","NATIVE_CODEC_R11_OUT"})known|=!std::strcmp(m->Name,key);
   if(!known)return false;
  }else if(!rgb||std::strcmp(m->Name,"NATIVE_RGB_NO_TILES"))return false;
 }
 if(codec){
  const int outputs=int(values["NATIVE_CODEC_UINT_OUT"])+int(values["NATIVE_CODEC_R11_OUT"])+int(values["NATIVE_CODEC_UNORM8_OUT"]);
  if(outputs>1||(!decode&&outputs))return false;
  if((values["NATIVE_CODEC_BGRA"]||values["NATIVE_CODEC_DEBUG_TINT"])&&!values["NATIVE_CODEC_UNORM8_OUT"])return false;
 }
 return true;
}
inline HRESULT NativeCompileShaderWithCompiler(const NativeShaderCompiler& compiler,const std::wstring& path,const D3D_SHADER_MACRO* macros,const char* entry,ID3DBlob** code,ID3DBlob** errors){
 if(!code||!entry)return E_INVALIDARG;*code=nullptr;if(errors)*errors=nullptr;
 if(FAILED(compiler.status)){NativeShaderSetError(errors,compiler.error);return compiler.status;}
 std::ifstream file(path.c_str(),std::ios::binary);if(!file)return HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND);
 std::string source((std::istreambuf_iterator<char>(file)),{});
 const bool hasInclude=source.find("include")!=std::string::npos;
 NativeHalfInclude dependency;bool snapshot=false;
 const auto sourceName=NativeShaderUtf8(path);
 if(hasInclude){
  auto header=std::filesystem::path(path).parent_path()/L"native_half_square.hlsli";
  std::ifstream input(header.c_str(),std::ios::binary);
  if(input){dependency.bytes.assign(std::istreambuf_iterator<char>(input),{});snapshot=!sourceName.empty()&&!dependency.bytes.empty()&&dependency.bytes.size()<1024*1024&&dependency.bytes.find("include")==std::string::npos;}
 }
 const bool allow50=!hasInclude&&NativeShaderAllows50(path,entry,macros);
 const auto attempt=[&](const char* target,ID3DBlob** blob,ID3DBlob** error)->HRESULT{
  if(hasInclude&&!snapshot)return compiler.compileFile(path.c_str(),macros,D3D_COMPILE_STANDARD_FILE_INCLUDE,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob,error);
  const auto key=NativeShaderCacheKey(compiler,source,entry,macros,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,snapshot?sourceName:"",snapshot?dependency.bytes:"");
  auto& state=NativeShaderCache();std::lock_guard<std::mutex> lock(state.mutex);
  const wchar_t* flag=_wgetenv(L"DLSS5_SHADER_DISK_CACHE");bool disk=!(flag&&!wcscmp(flag,L"0"));
  std::wstring diskPath;
  if(disk){
   auto folder=std::filesystem::path(path).parent_path()/L"shader-cache";CreateDirectoryW(folder.c_str(),nullptr);
   const auto hash=NativeShaderHex(NativeShaderHash(key.data(),key.size()));
   diskPath=(folder/(std::wstring(hash.begin(),hash.end())+L".v2.dxbc")).wstring();
  }
  auto found=state.entries.find(key);
  if(found==state.entries.end()&&disk){
   std::vector<unsigned char> bytes;
   if(NativeReadShaderCache(diskPath,key,bytes)){found=state.entries.emplace(key,std::move(bytes)).first;++state.diskHits;}
  }
  if(found!=state.entries.end()){
   HRESULT hr=compiler.createBlob(found->second.size(),blob);if(FAILED(hr))return hr;
   std::memcpy((*blob)->GetBufferPointer(),found->second.data(),found->second.size());++state.hits;
   if(state.hits<=3)std::fprintf(stderr,"lmxxf_shader cache=hit effective=%s diskHits=%zu\n",target,state.diskHits);
   return S_OK;
  }
  ++state.compiles;
  HRESULT hr=compiler.compile(source.data(),source.size(),snapshot?sourceName.c_str():"native-standalone",macros,snapshot?&dependency:nullptr,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob,error);
  if(dependency.unknown){NativeShaderRelease(*blob);if(error)NativeShaderRelease(*error);return compiler.compileFile(path.c_str(),macros,D3D_COMPILE_STANDARD_FILE_INCLUDE,entry,target,D3DCOMPILE_OPTIMIZATION_LEVEL3,0,blob,error);}
  if(state.compiles<=8||FAILED(hr))std::fprintf(stderr,"lmxxf_shader cache=miss effective=%s hr=0x%08x entry=%s\n",target,unsigned(hr),entry);
  if(SUCCEEDED(hr)&&*blob){
   auto* begin=static_cast<const unsigned char*>((*blob)->GetBufferPointer());std::vector<unsigned char> bytes(begin,begin+(*blob)->GetBufferSize());
   if(disk)NativeWriteShaderCache(diskPath,key,bytes);state.entries.emplace(key,std::move(bytes));
  }
  return hr;
 };
 return NativeCompileShaderPolicy(attempt,"cs_5_1",allow50,code,errors);
}
inline HRESULT CompileNativeShader(const std::wstring& path,const D3D_SHADER_MACRO* macros,const char* entry,ID3DBlob** code,ID3DBlob** errors){
 return NativeCompileShaderWithCompiler(NativePrivateShaderCompiler(),path,macros,entry,code,errors);
}
