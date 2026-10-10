#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonCapture.h"
#include <algorithm>
#include <cstdio>
#include <stdexcept>
#include <vector>
using DlssNr::Person::CaptureSchedule;
static void Require(bool value,const char* message){if(!value)throw std::runtime_error(message);}
struct Result {unsigned expired=0,faded=0;uint64_t maxAge=0;};
// Delayed end-of-frame fences and one asynchronous CPU worker, measured in
// render-frame steps. Reported game intervals are approximately 38/34/25 ms;
// source-age clock is quantized too, as GetTickCount64 is on the affected machine.
// Capture scheduling uses a separate high-resolution monotonic clock.
static Result Simulate(bool pipelined,unsigned frameMs,unsigned gpuMs,unsigned cpuMs=9)
{
 CaptureSchedule schedule;std::vector<uint64_t> gpu;
 uint64_t working=0,done=0,mask=0;Result result;
 for(unsigned f=0;f<400;++f){
  const uint64_t now=1000+uint64_t(f)*frameMs,clock=now/16*16;
  if(working&&now>=done){mask=working;working=0;}
  uint64_t newest=0;
  for(auto source:gpu)if(now>=source+gpuMs)newest=(std::max)(newest,source);
  std::erase_if(gpu,[&](auto source){return now>=source+gpuMs;});
  if(newest&&!working){working=newest;done=now+cpuMs;}
  bool capture=pipelined?schedule.Request(now,gpu.size(),true):gpu.empty()&&!working;
  if(capture)gpu.push_back(now);
  Require(gpu.size()<=CaptureSchedule::MaxPending,"GPU capture bound");
  if(f>20&&mask){auto age=clock-mask/16*16;result.expired+=age>250;result.faded+=age>200;result.maxAge=(std::max)(result.maxAge,age);}
 }
 return result;
}
int main(){try{
 CaptureSchedule schedule;
 Require(schedule.Request(1000,0,true),"first capture");
 Require(!schedule.Request(1010,0,true)&&!schedule.Request(1100,4,true)&&!schedule.Request(1100,0,false),"rate/capacity/availability");
 Require(schedule.Request(1100,3,true),"capture while other frames are in flight");
 schedule.Submitted(9);
 Require(!schedule.Newer(8)&&!schedule.Newer(9)&&schedule.Newer(10),"late fence observation must not regress submitted source");
 for(unsigned frameMs:{16u,34u,38u,45u,50u}){
  CaptureSchedule cadence;unsigned count=0;
  for(uint64_t now=0;now<10000;now+=frameMs)count+=cadence.Request(now,0,true);
  Require(count>=190&&count<=200,"20 Hz cadence must not fall to every other render frame");
  std::printf("frame=%u ms: %u captures/10s (20 Hz target)\n",frameMs,count);
 }
 CaptureSchedule stalled;
 Require(stalled.Request(0,0,true)&&!stalled.Request(5000,4,true),"stall retains capture bound");
 Require(stalled.Request(5000,0,true)&&!stalled.Request(5000,0,true)&&!stalled.Request(5010,0,true),"missed slots do not cause catch-up burst");
 const auto old2=Simulate(false,38,110),old3=Simulate(false,34,98);
 Require(old2.expired>0&&old3.faded>0,"reported latency reproduces serial expiry/fade");
 for(auto pair:{std::pair{38u,110u},std::pair{34u,98u},std::pair{25u,74u}}){
  auto result=Simulate(true,pair.first,pair.second);
  Require(result.expired==0&&result.maxAge<=208,"pipelining avoids expiry under reported latency");
  std::printf("frame=%u gpu=%u ms: pipelined max source age=%llu, expired=%u\n",pair.first,pair.second,result.maxAge,result.expired);
 }
 for(auto pair:{std::pair{45u,131u},std::pair{50u,144u}}){
  auto result=Simulate(true,pair.first,pair.second);
  Require(result.expired==0,"20-22 FPS cadence must not regularly expire masks");
  std::printf("slow frame=%u gpu=%u ms: max source age=%llu, fade frames=%u, expired=%u\n",pair.first,pair.second,result.maxAge,result.faded,result.expired);
 }
 Simulate(true,16,400,90); // GPU stall/slow CPU remains bounded; no unbounded request queue.
 puts("person capture schedule: PASS");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}}
