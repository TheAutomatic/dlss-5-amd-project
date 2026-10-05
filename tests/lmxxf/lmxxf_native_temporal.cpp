#include "temporal_fixture.h"
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/dlssnr/backend/lmxxf_runtime/NativeTemporalHistory.h"

// Independent double-precision five-tap reference. Deliberately samples a
// nonlinear field; an ordinary bilinear warp cannot pass the fractional test.
static double Reference5(const std::vector<float>& rgb,unsigned w,unsigned h,double x,double y)
{
    struct Axis { double p0,pm,p2,a,b,c; };
    const auto axis=[](double x) {double base=std::floor(x-.5)+.5,t=x-base;
        double a=-.5*t*(1-t)*(1-t),c=-.5*t*t*(1-t),b=1-a-c;
        double w2=.5*t+2*t*t-1.5*t*t*t;
        return Axis{base-1,base+w2/b,base+2,a,b,c};};
    const auto sample=[&](double px,double py){px=std::clamp(px-.5,0.,double(w-1));py=std::clamp(py-.5,0.,double(h-1));
        unsigned x0=unsigned(px),y0=unsigned(py),x1=std::min(x0+1,w-1),y1=std::min(y0+1,h-1);
        double fx=px-x0,fy=py-y0;
        return (1-fy)*((1-fx)*rgb[(y0*w+x0)*3]+fx*rgb[(y0*w+x1)*3])+fy*((1-fx)*rgb[(y1*w+x0)*3]+fx*rgb[(y1*w+x1)*3]);};
    auto a=axis(x),b=axis(y);double weights[]={a.a*b.b,b.a*a.b,a.b*b.b,b.c*a.b,a.c*b.b};
    double values[]={sample(a.p0,b.pm),sample(a.pm,b.p0),sample(a.pm,b.pm),sample(a.pm,b.p2),sample(a.p2,b.pm)};
    double sum=0,total=0;for(int i=0;i<5;++i){sum+=weights[i]*values[i];total+=weights[i];}return sum/total;
}
int main()try
{
    Gpu g;constexpr unsigned w=8,h=8,n=w*h;
    auto motion=g.Texture(w,h,DXGI_FORMAT_R32G32_FLOAT),depth=g.Texture(w,h,DXGI_FORMAT_R32_FLOAT);
    auto raw=g.Buffer(n*16),output=g.Buffer(n*20);
    std::vector<float> colour(n*4,.4f),network(n*5,0),rgb(n*3),vectors(n*2,0),depths(n,.5f);
    for(unsigned i=0;i<n;++i){colour[i*4+3]=1;for(unsigned c=0;c<3;++c)rgb[i*3+c]=.3f+.002f*(i%w)*(i%w)+.02f*std::sin(float(i));}
    std::copy(rgb.begin(),rgb.end(),network.begin());
    g.Upload(raw.Get(),colour);g.Upload(output.Get(),network);g.Upload(motion.Get(),vectors);g.Upload(depth.Get(),depths);
    LmxxfNativeTemporal::History hist;hist.Create(g.device.Get(),w,h,h);hist.PrepareBinding(g.device.Get(),motion.Get(),depth.Get());
    LmxxfNativeTemporal::Parameters p{};p.width=w;p.height=h;p.processingHeight=h;p.viewWidth=w;p.viewHeight=h;
    p.renderWidth=w;p.renderHeight=h;p.motionWidth=w;p.motionHeight=h;p.scaleX=1.f/w;p.scaleY=1.f/h;p.logitOffset=n*3;
    const auto inputs=[&]{g.Run([&](auto*cmd){hist.RecordInputs(cmd,raw.Get(),output.Get(),motion.Get(),depth.Get(),ReadState,ReadState,p);});};
    const auto finish=[&]{g.Run([&](auto*cmd){hist.RecordOutputs(cmd,raw.Get(),output.Get(),depth.Get(),ReadState,p);});};
    const auto reset=[&]{p.useHistory=0;p.jitterX=p.jitterY=0;g.Upload(output.Get(),network);inputs();finish();p.useHistory=1;};
    inputs();auto pre=g.Read(hist.Warped());Require(pre[3]==0,"first frame history gated");finish();
    Require(g.Read(output.Get())==network,"first frame RGB and logit buffer unchanged");
    p.useHistory=1;inputs();pre=g.Read(hist.Warped());auto post=g.Read(hist.PostWarped());
    for(unsigned i=0;i<n;++i)Require(pre[i*4+3]==1&&std::abs(pre[i*4]-rgb[i*3])<2e-6f&&std::abs(post[i*4]-rgb[i*3])<2e-6f,"pre/post identity");
    p.jitterX=.35f/w;p.jitterY=.65f/h;inputs();pre=g.Read(hist.Warped());
    for(unsigned y=0;y<h;++y)for(unsigned x=0;x<w;++x)
        Require(std::abs(pre[(y*w+x)*4]-Reference5(rgb,w,h,x+.85,y+1.15))<3e-6,"fractional five-tap reference and tap clamp");
    // The two native paths sample motion differently at a silhouette.
    p.jitterX=p.jitterY=0;depths[4*w+4]=.1f;vectors[(4*w+4)*2]=1;
    g.Upload(depth.Get(),depths);g.Upload(motion.Get(),vectors);inputs();pre=g.Read(hist.Warped());post=g.Read(hist.PostWarped());
    const unsigned target=3*w+3;
    Require(std::abs(pre[target*4]-rgb[(target+1)*3])<2e-6,"pre selects nearest-depth diagonal motion");
    Require(std::abs(post[target*4]-rgb[target*3])<2e-6,"post keeps centre motion");
    depths[4*w+4]=.9f;p.depthInverted=1;g.Upload(depth.Get(),depths);inputs();pre=g.Read(hist.Warped());
    Require(std::abs(pre[target*4]-rgb[(target+1)*3])<2e-6,"reversed depth selection");
    p.hasDepth=0;inputs();pre=g.Read(hist.Warped());Require(std::abs(pre[target*4]-rgb[target*3])<2e-6,"optional depth disabled");p.hasDepth=1;
    // Integration must retain the existing disocclusion guards even though the
    // native model itself can assign a large weight to an incompatible sample.
    depths[target]=.95f;g.Upload(depth.Get(),depths);inputs();post=g.Read(hist.PostWarped());
    Require(post[target*4+3]==0,"newly revealed depth rejects post history");
    depths[target]=.5f;g.Upload(depth.Get(),depths);
    for(unsigned c=0;c<3;++c)colour[target*4+c]=.7f;
    g.Upload(raw.Get(),colour);inputs();post=g.Read(hist.PostWarped());
    Require(post[target*4+3]==0,"changed raw colour rejects post history");
    for(unsigned c=0;c<3;++c)colour[target*4+c]=.4f;
    g.Upload(raw.Get(),colour);
    // Offscreen history uses per-tap clamp, matching the model's addressing.
    std::fill(vectors.begin(),vectors.end(),0);vectors[target*2]=-100;g.Upload(motion.Get(),vectors);p.hasDepth=0;inputs();post=g.Read(hist.PostWarped());
    Require(post[target*4+3]==1&&std::abs(post[target*4]-rgb[(3*w)*3])<2e-6,"offscreen per-tap clamp");
    vectors[target*2]=std::numeric_limits<float>::quiet_NaN();g.Upload(motion.Get(),vectors);inputs();post=g.Read(hist.PostWarped());
    Require(post[target*4+3]==0,"nonfinite motion rejected");
    std::fill(vectors.begin(),vectors.end(),0);g.Upload(motion.Get(),vectors);inputs();
    // Predictable model logit: sigmoid(0) * exact stored model blend scale.
    std::fill(network.begin(),network.begin()+n*3,.8f);g.Upload(output.Get(),network);finish();auto mixed=g.Read(output.Get());
    const float weight=.73974609375f*.5f;
    for(unsigned i=0;i<n;++i)Require(std::abs(mixed[i*3]-(.8f+weight*(rgb[i*3]-.8f)))<2e-6,"native post blend");
    inputs();post=g.Read(hist.PostWarped());for(unsigned i=0;i<n;++i)Require(std::abs(post[i*4]-mixed[i*3])<2e-6,"feedback stores model blended output");
    // Display smoothing must change the delivered pixels without changing the
    // next model input. Use a small change that passes its difference gate.
    auto smoothedInput=network;
    for(unsigned i=0;i<n*3;++i)smoothedInput[i]=mixed[i]+.01f;
    p.smoothStrength=.25f;g.Upload(output.Get(),smoothedInput);finish();auto display=g.Read(output.Get());
    inputs();post=g.Read(hist.PostWarped());
    for(unsigned i=0;i<n;++i){
        float model=mixed[i*3]+.01f*(1-weight);
        float displayWeight=.25f*(1-.01f*(1-weight)/(8.f/255.f));
        Require(std::abs(post[i*4]-model)<2e-6,"display smoothing excluded from model feedback");
        Require(std::abs(display[i*3]-(model+displayWeight*(mixed[i*3]-model)))<2e-6,"explicit display smoothing applied after model storage");
    }
    p.smoothStrength=0;
    p.historyStrength=0;g.Upload(output.Get(),network);finish();Require(g.Read(output.Get())==network,"zero history strength bit identity");p.historyStrength=1;
    inputs();std::fill(network.begin(),network.begin()+n*3,0);g.Upload(output.Get(),network);finish();Require(g.Read(output.Get())==network,"failed zero neural output cannot resurrect history");
    inputs();post=g.Read(hist.PostWarped());Require(post[target*4+3]==0,"failed output not valid next history");
    // A reset bypasses old model pixels even with a stale nonzero logit buffer.
    std::fill(network.begin(),network.begin()+n*3,.6f);reset();inputs();post=g.Read(hist.PostWarped());Require(std::abs(post[target*4]-.6f)<2e-6,"reset primes fresh model");
    // A fitted viewport: boundaries must not read letterbox pixels.
    p.viewX=2;p.viewWidth=4;reset();inputs();pre=g.Read(hist.Warped());Require(pre[3]==0&&pre[2*4+3]==1&&pre[6*4+3]==0,"fit viewport validity");
    g.NoErrors();std::puts("native temporal WARP PASS: distinct pre/post motion, 5tap, edges, reversed depth, post model feedback, zero recovery/reset/fit");return 0;
}catch(const std::exception&e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
