#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonInference.h"
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonSettings.h"
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonDiagnostics.h"
#include <cstdio>
int wmain(int argc,wchar_t**argv){
 try {
  using DlssNr::Person::Settings;
  using DlssNr::Person::MaskFreshness;
  if(Settings{}.ChangesSinglePass()||!Settings{.5f,1.f}.ChangesSinglePass())return 5;
  auto bounded=Settings{std::numeric_limits<float>::quiet_NaN(),-1}.Bounded();
  if(bounded.strength!=1||bounded.detail!=0)return 6;
  if(MaskFreshness(200)!=1||MaskFreshness(225)!=.5f||MaskFreshness(250)!=0||
     MaskFreshness(UINT64_MAX)!=0)return 7;
  DlssNr::Person::PersonDiagnostics diagnostic;
  diagnostic.Enable(true,1000);
  diagnostic.Mask(DlssNr::Person::MaskDecision::Expired);
  diagnostic.maskAge.Add(270);
  if(!diagnostic.Sample(1000)||diagnostic.Sample(1050)||!diagnostic.Sample(1100)||
     !diagnostic.Report(2999).empty())return 10;
  auto report=diagnostic.Report(3000);
  if(report.find("=0/0/0/0/1/0/0")==std::string::npos||
     report.find("mask_age_ms[n/min/mean/max]=1/270/270/270")==std::string::npos||
     diagnostic.maskAge.count||!diagnostic.Report(3001).empty())return 11;
  const auto generation=diagnostic.generation;
  diagnostic.Enable(false,3001);
  if(diagnostic.Sample(6000)||!diagnostic.Report(6000).empty()||diagnostic.generation==generation)return 12;
  std::vector<float>d(116*8400),p(32*160*160,1.f);
  d[0]=d[8400]=320;d[2*8400]=d[3*8400]=160;d[4*8400]=.9f;d[84*8400]=5;
  auto mask=DlssNr::Person::Decode(d.data(),p.data());
  if(mask[80*160+80]<.9f||mask[0]!=0)return 2;
  d[4*8400]=std::numeric_limits<float>::quiet_NaN();
  mask=DlssNr::Person::Decode(d.data(),p.data());if(mask[80*160+80]!=0)return 3;
  if(argc>1){
   DlssNr::Person::Inference model;model.Open(argv[1]);
   DlssNr::Person::Image image{1,1,GetTickCount64(),1920,1080,std::vector<float>(3*640*640,.5f)};
   auto output=model.Run(image);
   if(output.values.size()!=160*160)return 4;
   std::printf("actual CPU model: %.0f ms, finite mask %zu\n",output.milliseconds,output.values.size());
   DlssNr::Person::Provider provider;
   provider.Configure(true,argv[1]);
   auto start=GetTickCount64();
   while(!provider.Ready()){
    if(GetTickCount64()-start>10000)throw std::runtime_error(provider.Status());
    Sleep(1);
   }
   image.tick=GetTickCount64();
   if(!provider.Submit(std::make_shared<DlssNr::Person::Image>(image)))return 8;
   while(!provider.Latest()){
    if(GetTickCount64()-start>15000)throw std::runtime_error(provider.Status());
    Sleep(1);
   }
   auto remote=provider.Latest();
   if(remote->epoch!=image.epoch||remote->frame!=image.frame||remote->values!=output.values)return 9;
   std::printf("isolated real worker: %.0f ms; direct/IPC output identical\n",remote->milliseconds);
   provider.Configure(false,argv[1]);
  }
  puts("person inference: PASS");return 0;
 }catch(const std::exception&e){std::fprintf(stderr,"%s\n",e.what());return 1;}
}

