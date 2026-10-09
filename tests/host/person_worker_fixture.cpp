// Out-of-process IPC peer for lifecycle tests. No model or GPU dependency.
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonIpc.h"
#include <Windows.h>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <vector>
namespace Ipc=DlssNr::Person::Ipc;
int wmain(int argc,wchar_t**argv){
 if(argc<3)return 2;
 const auto pid=static_cast<unsigned>(_wtoi(argv[2]));
 wchar_t exe[32768];GetModuleFileNameW(nullptr,exe,32768);auto dir=std::filesystem::path(exe).parent_path();
 std::ofstream(dir/L"launches.txt",std::ios::app)<<"start\n";
 std::string mode;std::ifstream(dir/L"mode.txt")>>mode;
 HANDLE map=OpenFileMappingW(FILE_MAP_ALL_ACCESS,FALSE,Ipc::ShmName(pid).c_str());if(!map)return 3;
 auto*h=static_cast<Ipc::ShmHeader*>(MapViewOfFile(map,FILE_MAP_ALL_ACCESS,0,0,Ipc::TotalShmSize));if(!h)return 4;
 HANDLE request=OpenEventW(SYNCHRONIZE,FALSE,Ipc::ReqEventName(pid).c_str());
 HANDLE response=OpenEventW(EVENT_MODIFY_STATE,FALSE,Ipc::RespEventName(pid).c_str());
 HANDLE stop=OpenEventW(SYNCHRONIZE,FALSE,Ipc::StopEventName(pid).c_str());
 if(mode=="exit")return 7;
 if(mode=="version")h->version=Ipc::ShmVersion+1;
 if(mode!="legacy")h->flags=Ipc::ShmVersion;
 h->state=uint32_t(Ipc::WorkerState::Ready);SetEvent(response);
 HANDLE waits[]={stop,request};
 while(WaitForMultipleObjects(2,waits,FALSE,5000)==WAIT_OBJECT_0+1){
  h->state=uint32_t(Ipc::WorkerState::Processing);
  if(mode=="slow")Sleep(30);
  h->respFrame=h->reqFrame;h->respEpoch=h->reqEpoch;h->respTick=h->reqTick;
  h->respWidth=h->reqWidth;h->respHeight=h->reqHeight;h->computeMilliseconds=1;
  auto*mask=reinterpret_cast<float*>(reinterpret_cast<unsigned char*>(h)+Ipc::OutputMaskOffset);
  std::fill_n(mask,160*160,float(h->reqFrame%2));
  if(mode=="offset")h->respMaskOffset=0xffffffff;
  if(mode=="frame")++h->respFrame;
  h->state=uint32_t(Ipc::WorkerState::Ready);SetEvent(response);
 }
 CloseHandle(request);CloseHandle(response);CloseHandle(stop);UnmapViewOfFile(h);CloseHandle(map);return 0;
}
