#pragma once
// DLSS5_FRAME_STATS=<seconds> (2026-09-28): every <seconds>, append one line with the frame-interval distribution and the
// reasons the network did or did not run to DLSS5-AMD\logs\frame-stats.txt. 0 or absent = off: Frame() then returns on
// the first comparison and makes no call at all. On: per frame one QueryPerformanceCounter (user mode on current Windows)
// and O(1) counters in a fixed 0.25 ms histogram up to 100 ms; the file is written once per window. The interval is the
// time between successive calls of the host hook (one per upscaler dispatch = one per game frame); it is not a GPU time.
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>
#include <cstdlib>
#include <stdexcept>

// DLSS5_FRAME_STATS=<seconds> : shared by the add-on (which reads it from the flags file, before the
// background initializer exports the flags) and the RE9 runtime (environment). 0..3600; anything else is refused.
inline unsigned NativeFrameStatsSeconds(const char*v){
  if(!v||!*v)return 0;char*end=nullptr;const unsigned long s=strtoul(v,&end,10);
  if(*end&&*end!='\r'&&*end!='\n'&&*end!=' ')throw std::runtime_error("DLSS5_FRAME_STATS must be seconds");
  if(s>3600)throw std::runtime_error("DLSS5_FRAME_STATS must be 0..3600");return unsigned(s);}


enum NativeFrameStatReason : unsigned { NFS_RUN, NFS_BYPASS, NFS_INIT, NFS_ERROR, NFS_UNSUPPORTED, NFS_IDLE, NFS_COUNT };

class NativeFrameStats {
 static constexpr unsigned kBuckets=400; // 0.25 ms each; index kBuckets = 100 ms and over
 unsigned window_s=0;std::wstring path;int64_t freq=0,last=0,start=0;
 uint32_t hist[kBuckets+1]{};uint32_t intervals=0,reasons[NFS_COUNT]{};double sum=0,max_ms=0;std::mutex m;
 double Quantile(double q)const{ // upper edge of the bucket holding the q-th interval
  if(!intervals)return 0;uint32_t want=uint32_t(q*intervals);if(want>=intervals)want=intervals-1;uint32_t acc=0;
  for(unsigned i=0;i<=kBuckets;i++){acc+=hist[i];if(acc>want)return i==kBuckets?max_ms:(i+1)*0.25;}return max_ms;}
 void Flush(int64_t now){
  if(intervals){
   const double avg=sum/intervals,p50=Quantile(0.5),p99=Quantile(0.99);
   SYSTEMTIME t;GetLocalTime(&t);
   if(FILE*f=_wfopen(path.c_str(),L"ab")){
    fprintf(f,"%04u-%02u-%02u %02u:%02u:%02u window=%.1fs frames=%u fps=%.1f frame_ms avg=%.2f p50=%.2f p99=%.2f max=%.2f low1=%.1ffps nr run=%u bypass=%u init=%u error=%u unsupported=%u idle=%u\n",
     t.wYear,t.wMonth,t.wDay,t.wHour,t.wMinute,t.wSecond,double(now-start)/freq,intervals,1000.0/avg,avg,p50,p99,max_ms,p99>0?1000.0/p99:0.0,
     reasons[NFS_RUN],reasons[NFS_BYPASS],reasons[NFS_INIT],reasons[NFS_ERROR],reasons[NFS_UNSUPPORTED],reasons[NFS_IDLE]);
    fclose(f);}
  }
  for(auto&h:hist)h=0;for(auto&r:reasons)r=0;intervals=0;sum=0;max_ms=0;start=now;
 }
public:
 void Configure(unsigned seconds,const std::wstring&file){
  std::lock_guard<std::mutex>g(m);window_s=seconds;path=file;last=start=0;
  if(seconds){LARGE_INTEGER f;QueryPerformanceFrequency(&f);freq=f.QuadPart;}}
 bool On()const{return window_s!=0;}
 void Frame(NativeFrameStatReason r){
  if(!window_s)return;
  LARGE_INTEGER c;QueryPerformanceCounter(&c);const int64_t now=c.QuadPart;
  std::lock_guard<std::mutex>g(m);
  if(last){const double ms=double(now-last)*1000.0/freq;unsigned b=unsigned(ms*4.0);if(b>kBuckets)b=kBuckets;
   hist[b]++;intervals++;sum+=ms;if(ms>max_ms)max_ms=ms;}
  else start=now;
  last=now;reasons[r<NFS_COUNT?r:NFS_IDLE]++;
  if(now-start>=int64_t(window_s)*freq)Flush(now);
 }
};
