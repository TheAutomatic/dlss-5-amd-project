#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonInference.h"
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonSettings.h"
#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/person/PersonDiagnostics.h"
#include <cstdio>
static void TestFaceDecode() {
 using namespace DlssNr::Person;
 if(ModelFileName(0)!=L"pphumanseg.onnx"||ModelFileName(1)!=L"yolo11n-seg.onnx"||ModelFileName(2)!=L"yunet.onnx")throw std::runtime_error("model selection");
 std::array<std::vector<float>,3> cls,obj,box;
 std::array<Face::Head,3> heads;
 for(unsigned l=0;l<3;++l){const unsigned side=Face::InputSize/(8u<<l),n=side*side;
  cls[l].resize(n);obj[l].resize(n);box[l].resize(n*4);heads[l]={cls[l],obj[l],box[l]};}
 auto face=[&](unsigned index,float score){cls[0][index]=obj[0][index]=score;
  box[0][4*index+2]=std::log(10.f);box[0][4*index+3]=std::log(12.f);};
 constexpr unsigned center=20*40+20;face(center,.9f);
 auto mask=Face::Decode(heads);
 if(mask[80*160+80]<.99f||mask[120*160+80]!=0||mask[80*160+110]!=0)throw std::runtime_error("face mask extent");
 // A duplicate box cannot broaden or darken the selected face.
 face(center+1,.8f);box[0][4*(center+1)]=-1;
 if(Face::Decode(heads)!=mask)throw std::runtime_error("face NMS");
 cls[0][center+1]=0;
 cls[0][center]=std::numeric_limits<float>::quiet_NaN();
 auto invalid=Face::Decode(heads);if(std::any_of(invalid.begin(),invalid.end(),[](float v){return v!=0;}))throw std::runtime_error("nonfinite face score");
 cls[0][center]=.9f;box[0][4*center+2]=10000;
 invalid=Face::Decode(heads);if(std::any_of(invalid.begin(),invalid.end(),[](float v){return v!=0;}))throw std::runtime_error("unbounded face box");
 heads[0].box=heads[0].box.first(1);bool rejected=false;
 try{Face::Decode(heads);}catch(const std::runtime_error&){rejected=true;}
 if(!rejected)throw std::runtime_error("face output size guard");
 std::vector<float> rgb(3*640*640),input(3*320*320);
 std::fill_n(rgb.data(),640*640,1.f);std::fill_n(rgb.data()+640*640,640*640,.5f);
 Face::Prepare(rgb.data(),input);
 if(input[0]!=0||input[320*320]!=127.5f||input[2*320*320]!=255)throw std::runtime_error("face BGR scale");
 for(unsigned size:{320u,384u,416u}) {
  std::array<std::vector<float>,3> cs,os,bs;std::array<Face::Head,3> hs;
  for(unsigned l=0;l<3;++l){const unsigned side=size/(8u<<l),n=side*side;cs[l].resize(n);os[l].resize(n);bs[l].resize(n*4);hs[l]={cs[l],os[l],bs[l]};}
  const unsigned side=size/8,idx=(side/2)*side+side/2;cs[0][idx]=os[0][idx]=.9f;
  bs[0][4*idx+2]=std::log(float(size)/32);bs[0][4*idx+3]=std::log(float(size)/32);
  auto m=Face::Decode(hs,size);if(m[80*160+80]<.99f||m[0]!=0)throw std::runtime_error("face size mapping");
  std::vector<float> prep(3*size*size);Face::Prepare(rgb.data(),prep,size);
  if(prep[0]!=0||prep[size*size]!=127.5f||prep[2*size*size]!=255)throw std::runtime_error("face resize colours");
 }
 if(Face::BoundedSize(400)!=320||Face::BoundedSize(UINT_MAX)!=320)throw std::runtime_error("invalid size default");
 puts("face model: PASS (selection, oval, NMS, invalid tensors, BGR)");
}
int wmain(int argc,wchar_t**argv){
 try {
  TestFaceDecode();
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
   const std::wstring file=argc>2?argv[2]:L"";
   DlssNr::Person::Inference model;const unsigned size=argc>3?std::stoul(argv[3]):320;
   model.Open(argv[1],2,file,size);
   DlssNr::Person::Image image{1,1,GetTickCount64(),1920,1080,std::vector<float>(3*640*640,.5f)};
   auto output=model.Run(image);
   if(output.values.size()!=160*160)return 4;
   std::printf("actual CPU model: %.0f ms, finite mask %zu\n",output.milliseconds,output.values.size());
   DlssNr::Person::Provider provider;
   provider.Configure(true,argv[1],file,size);
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

