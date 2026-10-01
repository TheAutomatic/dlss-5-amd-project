#pragma once
#include "native_lab_paths.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwchar>
#include <mutex>
/* Hot reload of the flags file (0.38, 2026-09-30; after mochizuki 0.0.2.4's ini reload).
   DLSS5_HOT_RELOAD=1 (default; environment, else the flags file at start): at most once a second the add-on compares the
   last-write time of <lab>\native-game-flags.txt; when it changed, it re-reads ONLY these keys and applies them from the next frame:
     DLSS5_STRENGTH   transfer,colour (0..3) or auto  -- decoder blend after the network, not the network itself
     DLSS5_NOTICE     on-screen status line (0/1/2)
     DLSS5_SHOW_FPS   FPS text in that line (0/1)
   Everything else (network height, skip blocks, HIP kernels/modules, FP8/wave/tiling switches, DIRECT_IO, PRE_UPSCALE, FIT_*, ASYNC,
   FRAME_STATS, FORMAT_FALLBACK) stays as read at start: those build buffers, modules or pipelines once, or change the network's
   numbers; editing them needs a game restart as before. The first poll only records the time stamp, so an unedited file changes
   nothing (bit-exact). A reload is logged to logs\native-game-oneshot.txt. Cost: one GetTickCount64 per call, one stat per second.
   0 = never poll (previous behaviour). */
struct NativeHotFlagValues{bool strength=false;float transfer=1.f,color=1.f;int notice=-1,fps=-1;unsigned generation=0;};
class NativeHotFlags{
 std::mutex mutex;NativeHotFlagValues values;ULONGLONG last_poll=0;FILETIME stamp{};bool stamped=false;
 static bool read_stamp(FILETIME&t){WIN32_FILE_ATTRIBUTE_DATA a{};if(!GetFileAttributesExW(NativeLabPath(L"native-game-flags.txt").c_str(),GetFileExInfoStandard,&a))return false;t=a.ftLastWriteTime;return true;}
 void reload(){
  NativeHotFlagValues v;v.generation=values.generation+1;char strength[48]="";
  if(FILE*f=_wfopen(NativeLabPath(L"native-game-flags.txt").c_str(),L"rb")){char line[256];while(fgets(line,sizeof line,f)){unsigned n;
   if(sscanf(line,"DLSS5_NOTICE=%u",&n)==1)v.notice=int(n);if(sscanf(line,"DLSS5_SHOW_FPS=%u",&n)==1)v.fps=int(n);sscanf(line,"DLSS5_STRENGTH=%47s",strength);}fclose(f);}
  else return; /* mid-save by an editor: keep the previous values, retry on the next stamp change */
  float a=1.f,b=1.f;if(sscanf(strength,"%f,%f",&a,&b)==2&&a>=0.f&&a<=3.f&&b>=0.f&&b<=3.f){v.strength=true;v.transfer=a;v.color=b;}
  values=v;
  if(FILE*f=_wfopen(NativeLabPath(L"logs\\native-game-oneshot.txt").c_str(),L"ab")){fprintf(f,"pid=%lu tick=%llu event=hot_reload detail=generation %u strength=%s notice=%d show_fps=%d (other keys need a restart)\n",GetCurrentProcessId(),GetTickCount64(),v.generation,v.strength?strength:"startup",v.notice,v.fps);fclose(f);}
 }
public:
 static bool Enabled(){
  static const bool on=[]{if(const wchar_t*e=_wgetenv(L"DLSS5_HOT_RELOAD"))return wcscmp(e,L"0")!=0;
   unsigned x=1;if(FILE*f=_wfopen(NativeLabPath(L"native-game-flags.txt").c_str(),L"rb")){char line[256];while(fgets(line,sizeof line,f))sscanf(line,"DLSS5_HOT_RELOAD=%u",&x);fclose(f);}return x!=0;}();
  return on;
 }
 /* Current values (defaults = "use what was read at start"). Polls the file stamp at most once per second. */
 NativeHotFlagValues Get(){
  if(!Enabled())return {};
  std::lock_guard<std::mutex>lock(mutex);const ULONGLONG now=GetTickCount64();
  if(!last_poll||now-last_poll>=1000){last_poll=now;FILETIME t{};
   if(read_stamp(t)){if(!stamped){stamp=t;stamped=true;}else if(CompareFileTime(&t,&stamp)!=0){stamp=t;reload();}}}
  return values;
 }
 static NativeHotFlags&Instance(){static NativeHotFlags*h=new NativeHotFlags;return *h;}
};
