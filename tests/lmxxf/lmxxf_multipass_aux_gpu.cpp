#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/LmxxfProductionOptions.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/NativePostWeights.h"
#include <cstdio>
using namespace hip_reference;
namespace hip_reference {
template<class Tag, typename Tag::type Member> struct AuxAccess { friend typename Tag::type Access(Tag){return Member;} };
struct GraphMember {using type=Tensor(Network::*)(Tensor,Tensor,Tensor,bool,void*,bool);friend type Access(GraphMember);};
template struct AuxAccess<GraphMember,&Network::RunGraph>;
}
static void Require(bool ok,const char* why){if(!ok)throw std::runtime_error(why);}
int main(int argc,char** argv)try{
 Require(argc==3,"weights, module root");
 _putenv_s("DLSS5_MULTI_PASS","3");_putenv_s("DLSS5_MULTI_PASS_PREDICT","0");
 _putenv_s("DLSS5_FAST_NUMERIC","1");_putenv_s("DLSS5_SKIP_BLOCKS","none");
 _putenv_s("DLSS5_HIP_GRAPH","0");_putenv_s("DLSS5_OVERLAP","0");
 const U w=1280,h=768;const size_t pixels=size_t(w)*h;
 Api probe(7);probe.Check(probe.hipInit(0),"HIP init");int count=0,device=-1;std::string arch;
 probe.Check(probe.hipGetDeviceCount(&count),"device count");
 for(int i=0;i<count;++i){auto props=probe.Properties(i);std::string name(props.gcnArchName);name=name.substr(0,name.find(':'));if(name=="gfx1200"||name=="gfx1201"){device=i;arch=name;break;}}
 Require(device>=0,"RDNA4 device");probe.Check(probe.hipSetDevice(device),"device");
 auto options=LmxxfProductionOptions(w,h,std::string(argv[2])+"/"+arch,argv[1]);options.device=device;Network n(options);n.SetNoise({});
 auto& api=n.Runtime();auto input=std::make_shared<Allocation>(api,pixels*16);
 auto auxiliary=std::make_shared<Allocation>(api,pixels*8);
 std::vector<float> image(pixels*4,.4f),sentinel(pixels*2,123456.f),read(sentinel.size());
 for(size_t i=0;i<pixels;++i){image[i*4]=.2f+.3f*float(i%w)/w;image[i*4+3]=1;}
 api.Check(api.hipMemcpy(input->ptr,image.data(),input->bytes,1),"input");
 api.Check(api.hipMemcpy(auxiliary->ptr,sentinel.data(),auxiliary->bytes,1),"sentinel");
 n.EnableNativePostHistory(LmxxfNativePostWeights(),{auxiliary->ptr,auxiliary->bytes,w,h,8});
 auto graph=Access(GraphMember{});
 auto intermediate=(n.*graph)(input,{}, {},true,nullptr,false);n.Synchronize();
 api.Check(api.hipMemcpy(read.data(),auxiliary->ptr,auxiliary->bytes,2),"read intermediate auxiliary");
 Require(read==sentinel,"intermediate RGBA pass must not write auxiliary output");
 intermediate.reset();
 for(U passes:{1u,2u,3u}){
  n.SetMultiPass(passes);
  api.Check(api.hipMemcpy(auxiliary->ptr,sentinel.data(),auxiliary->bytes,1),"reset sentinel");
  auto final=(n.*graph)(input,{}, {},false,nullptr,true);n.Synchronize();
  api.Check(api.hipMemcpy(read.data(),auxiliary->ptr,auxiliary->bytes,2),"read final auxiliary");
  Require(read!=sentinel,"final RGB pass exports auxiliary output");
  for(float x:read)Require(std::isfinite(x)&&x!=123456.f,"every final auxiliary pixel written and finite");
 }
 puts("PASS: intermediate RGBA keeps auxiliary sentinel; final RGB exports all pixels; live MP1/2/3 accepted");return 0;
}catch(const std::exception& e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
