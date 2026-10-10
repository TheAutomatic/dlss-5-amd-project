#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonInference.h"
#include <fstream>
#include <cstdio>
namespace Person=DlssNr::Person;
static void Require(bool v,const char* s){if(!v)throw std::runtime_error(s);}
template<class F>static void Until(F f){auto start=GetTickCount64();while(!f()){Require(GetTickCount64()-start<30000,"IPC timeout/deadlock");Sleep(1);}}
int wmain(int argc,wchar_t**argv){
 try{
  Require(argc==2,"fixture directory");const std::filesystem::path dir=argv[1];
  Person::Provider p;
  auto configure=[&](const char* mode){p.Configure(false,dir);std::ofstream(dir/L"mode.txt")<<mode;p.Configure(true,dir);};
  auto input=[](unsigned f){return std::make_shared<Person::Image>(Person::Image{9,f,GetTickCount64(),1920,1080,std::vector<float>(3*640*640,.5f)});};
  Require(Person::WorkerThreads(16)==4&&Person::WorkerThreads(32)==4&&Person::WorkerThreads(8)==2,"CPU budget");
  configure("normal");Until([&]{return p.Ready();});
  for(unsigned f=1;f<=40;++f){Require(p.Submit(input(f)),"submit");Until([&]{return p.Ready();});auto m=p.Latest();Require(m&&m->frame==f&&m->epoch==9&&m->values[0]==float(f%2),"response must match submitted frame");}
  for(auto mode:{"offset","frame","version","legacy","exit"}){
   configure(mode);
   if(std::string(mode)=="offset"||std::string(mode)=="frame"){Until([&]{return p.Ready();});Require(p.Submit(input(1)),"bad peer submit");}
   Until([&]{return p.Status().find("mismatch")!=std::string::npos||p.Status().find("exited")!=std::string::npos;});
   const auto launches=std::filesystem::file_size(dir/L"launches.txt");
   for(unsigned i=0;i<100;++i)p.Configure(true,dir);
   Require(!p.Available()&&!p.Latest()&&std::filesystem::file_size(dir/L"launches.txt")==launches,"failure must latch without process respawn");
  }
  for(unsigned i=0;i<20;++i){configure("slow");Until([&]{return p.Ready();});Require(p.Submit(input(i+1)),"shutdown submit");Sleep(i%4*10);p.Configure(false,dir);}
  configure("normal");Until([&]{return p.Ready();});p.Configure(false,dir);
  Require(!p.Available(),"disabled provider");puts("person worker IPC: PASS (frame ownership, startup, repeated stop/response races, malformed peer, failure latch)");return 0;
 }catch(const std::exception&e){fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
}
