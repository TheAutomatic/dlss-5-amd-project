#pragma once
/* Layered configuration (2026-10-03). One implementation shared by the add-on (regular + Magpie), the hot reload and the RE9 runtime.

   Files, all optional, in the DLSS5-AMD folder, read in this order; a later file overrides a key of an earlier one, a key it does not
   mention keeps the earlier value:
     1. default-config.txt     shipped by the package; an upgrade may overwrite it
     2. custom-config.txt      the user's own changes; never written by an install or upgrade
     3. native-game-flags.txt  the old single file, same meaning as before (an existing one keeps working and wins over 1 and 2)
   On top of the three files: a DLSS5_* variable already present in the process environment when the configuration is first read
   wins over all of them.

   Line rules (unchanged from the old reader): trailing CR/LF/spaces are cut; a line counts when it starts with "DLSS5_", contains '='
   and is at least 8 characters long; the key is everything before the first '=', the value everything after it. Everything else
   (comments '#', blank lines, UTF-8 text) is ignored. A UTF-8 BOM at the start of a file is skipped. Lines have no length limit.
   Same key twice in one file: the last line wins (as the add-on's environment loader always did).
   Empty value ("DLSS5_SKIP_BLOCKS="): overrides the earlier layers with "empty"; applied to the environment it removes the variable
   (_putenv semantics), i.e. the program's built-in default is used. */
#include <string>
#include <vector>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cwchar>
#ifdef _WIN32
#include <windows.h>
#endif

struct NativeConfigEntry{std::string key,value;};
using NativeConfig=std::vector<NativeConfigEntry>; /* keys in order of first appearance, each key once */

inline bool NativeConfigParseLine(std::string line,std::string&key,std::string&value){
 if(line.size()>=3&&(unsigned char)line[0]==0xEF&&(unsigned char)line[1]==0xBB&&(unsigned char)line[2]==0xBF)line.erase(0,3);
 while(!line.empty()&&(line.back()=='\n'||line.back()=='\r'||line.back()==' '))line.pop_back();
 if(line.size()<8||line.compare(0,6,"DLSS5_"))return false;
 const size_t eq=line.find('=');if(eq==std::string::npos)return false;
 key=line.substr(0,eq);value=line.substr(eq+1);return true;
}
inline const NativeConfigEntry*NativeConfigFind(const NativeConfig&c,const std::string&key){for(const auto&e:c)if(e.key==key)return &e;return nullptr;}
inline void NativeConfigSet(NativeConfig&c,const std::string&key,const std::string&value){for(auto&e:c)if(e.key==key){e.value=value;return;}c.push_back({key,value});}
inline void NativeConfigOverlay(NativeConfig&c,const NativeConfig&top){for(const auto&e:top)NativeConfigSet(c,e.key,e.value);}
/* One file's text: last occurrence of a key wins. */
inline NativeConfig NativeConfigParseText(const std::string&text){
 NativeConfig c;size_t at=0;std::string k,v;
 while(at<=text.size()){size_t nl=text.find('\n',at);if(nl==std::string::npos)nl=text.size();
  if(NativeConfigParseLine(text.substr(at,nl-at),k,v))NativeConfigSet(c,k,v);at=nl+1;}
 return c;
}
inline std::vector<std::string> NativeConfigLines(const NativeConfig&c){std::vector<std::string>l;l.reserve(c.size());for(const auto&e:c)l.push_back(e.key+"="+e.value);return l;}

#ifdef _WIN32
using NativeConfigPath=std::wstring;
inline FILE*NativeConfigOpen(const std::wstring&p){return _wfopen(p.c_str(),L"rb");}
inline std::wstring NativeConfigJoin(const std::wstring&dir,const wchar_t*name){return dir+L"\\"+name;}
#else
using NativeConfigPath=std::string;
inline FILE*NativeConfigOpen(const std::string&p){return std::fopen(p.c_str(),"rb");}
inline std::string NativeConfigJoin(const std::string&dir,const wchar_t*name){std::string n;for(const wchar_t*q=name;*q;++q)n+=char(*q);return dir+"/"+n;}
#endif
static const wchar_t*const NativeConfigLayerNames[3]={L"default-config.txt",L"custom-config.txt",L"native-game-flags.txt"};
inline bool NativeConfigReadFile(const NativeConfigPath&p,std::string&out){
 FILE*f=NativeConfigOpen(p);if(!f)return false;out.clear();char buf[4096];size_t n;while((n=std::fread(buf,1,sizeof buf,f))>0)out.append(buf,n);std::fclose(f);return true;
}
/* The three files of one folder merged (no environment). layers_found (optional) gets one bit per file that exists: 1 default, 2 custom, 4 native. */
inline NativeConfig NativeConfigLoadDir(const NativeConfigPath&dir,unsigned*layers_found=nullptr){
 NativeConfig c;unsigned found=0;std::string text;
 for(unsigned i=0;i<3;i++)if(NativeConfigReadFile(NativeConfigJoin(dir,NativeConfigLayerNames[i]),text)){found|=1u<<i;NativeConfigOverlay(c,NativeConfigParseText(text));}
 if(layers_found)*layers_found=found;return c;
}
inline bool NativeConfigDirHasAny(const NativeConfigPath&dir){
 for(unsigned i=0;i<3;i++)if(FILE*f=NativeConfigOpen(NativeConfigJoin(dir,NativeConfigLayerNames[i]))){std::fclose(f);return true;}return false;
}
/* Every DLSS5_* variable of the current process environment (process block, UTF-8). */
inline NativeConfig NativeConfigEnvironmentNow(){
 NativeConfig c;std::string k,v;
#ifdef _WIN32
 if(wchar_t*block=GetEnvironmentStringsW()){
  for(const wchar_t*s=block;*s;s+=wcslen(s)+1){const int n=WideCharToMultiByte(CP_UTF8,0,s,-1,nullptr,0,nullptr,nullptr);if(n<=1)continue;
   std::string u(size_t(n-1),'\0');WideCharToMultiByte(CP_UTF8,0,s,-1,&u[0],n,nullptr,nullptr);if(NativeConfigParseLine(u,k,v))NativeConfigSet(c,k,v);}
  FreeEnvironmentStringsW(block);}
#else
 extern char**environ;for(char**e=environ;e&&*e;++e)if(NativeConfigParseLine(*e,k,v))NativeConfigSet(c,k,v);
#endif
 return c;
}
/* Snapshot of the environment taken the first time any reader asks, i.e. before the add-on / runtime puts file values into it,
   so the hot reload still sees "what the user set" and not what we applied. */
inline const NativeConfig&NativeConfigSystemEnvironment(){static const NativeConfig e=NativeConfigEnvironmentNow();return e;}
/* Effective configuration of a folder: three files, then the environment snapshot on top. */
inline NativeConfig NativeConfigEffective(const NativeConfigPath&dir,unsigned*layers_found=nullptr){
 NativeConfig c=NativeConfigLoadDir(dir,layers_found);NativeConfigOverlay(c,NativeConfigSystemEnvironment());return c;
}
