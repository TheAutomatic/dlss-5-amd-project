#include "temporal_fixture.h"

int main()
try
{
    Gpu g; constexpr UINT w=8,h=8,n=w*h;
    auto motion=g.Texture(w,h,DXGI_FORMAT_R32G32_FLOAT),depth=g.Texture(w,h,DXGI_FORMAT_R32_FLOAT);
    auto raw=g.Buffer(n*16),output=g.Buffer(n*12);
    std::vector<float> pixels(n*4,.4f),model(n*3,.55f),vectors(n*2,0),depths(n,.5f);
    for(UINT i=0;i<n;++i){pixels[i*4+3]=1;for(UINT c=0;c<3;++c)model[i*3+c]+=.001f*float(i);}
    g.Upload(raw.Get(),pixels);g.Upload(output.Get(),model);g.Upload(motion.Get(),vectors);g.Upload(depth.Get(),depths);
    LmxxfTemporal::History history;history.Create(g.device.Get(),w,h,h);history.PrepareBinding(g.device.Get(),motion.Get(),depth.Get());
    LmxxfTemporal::Parameters p{};p.width=w;p.height=h;p.processingHeight=h;p.viewWidth=w;p.viewHeight=h;
    p.renderWidth=w;p.renderHeight=h;p.motionWidth=w;p.motionHeight=h;p.scaleX=1.f/w;p.scaleY=1.f/h;
    const auto inputs=[&]{g.Run([&](auto *cmd){history.RecordInputs(cmd,raw.Get(),output.Get(),motion.Get(),depth.Get(),ReadState,ReadState,p);});};
    const auto finish=[&]{g.Run([&](auto *cmd){history.RecordOutputs(cmd,raw.Get(),output.Get(),depth.Get(),ReadState,p);});};
    const auto rejected=[&](const char *reason){auto v=g.Read(history.Warped());for(UINT i=0;i<n;++i)
        Require(v[i*4+3]==0 && std::abs(v[i*4]-.4f)<1e-6f,reason);};
    inputs();rejected("first frame must use current input");finish();
    auto unchanged=g.Read(output.Get());Require(unchanged==model,"smoothing off must be bit-exact");
    p.useHistory=1;inputs();auto v=g.Read(history.Warped());
    for(UINT i=0;i<n;++i)Require(v[i*4+3]==1 && std::abs(v[i*4]-model[i*3])<1e-6f,"identity reprojection");
    // Known one-pixel translation, then a jitter-only shift with the same result.
    for(UINT i=0;i<n;++i)vectors[i*2]=1;
    g.Upload(motion.Get(),vectors);inputs();v=g.Read(history.Warped());
    Require(std::abs(v[3*4]-model[4*3])<1e-6f && v[7*4+3]==0,"motion pixels and out-of-bounds rejection");
    std::fill(vectors.begin(),vectors.end(),0);g.Upload(motion.Get(),vectors);p.jitterX=1.f/w;
    inputs();v=g.Read(history.Warped());Require(std::abs(v[3*4]-model[4*3])<1e-6f,"jitter coordinate adjustment");p.jitterX=0;
    // Render vectors and display vectors encode the same displacement in UV.
    auto displayMotion=g.Texture(w*2,h*2,DXGI_FORMAT_R32G32_FLOAT);std::vector<float> displayVectors(n*8,0);
    for(UINT i=0;i<n*4;++i)displayVectors[i*2]=2;
    g.Upload(displayMotion.Get(),displayVectors);history.PrepareBinding(g.device.Get(),displayMotion.Get(),depth.Get());
    p.motionWidth=w*2;p.motionHeight=h*2;p.scaleX=1.f/(w*2);p.scaleY=1.f/(h*2);
    g.Run([&](auto *cmd){history.RecordInputs(cmd,raw.Get(),output.Get(),displayMotion.Get(),depth.Get(),ReadState,ReadState,p);});
    v=g.Read(history.Warped());Require(std::abs(v[3*4]-model[4*3])<1e-6f,"display-grid motion normalization");
    history.PrepareBinding(g.device.Get(),motion.Get(),depth.Get());p.motionWidth=w;p.motionHeight=h;p.scaleX=1.f/w;p.scaleY=1.f/h;
    // Half-pixel jitter: interpolation is expected, not nearest-frame hold.
    p.jitterX=.5f/w;inputs();v=g.Read(history.Warped());
    Require(std::abs(v[3*4]-(model[3*3]+model[4*3])*.5f)<1e-6f,"fractional reprojection");p.jitterX=0;
    std::fill(depths.begin(),depths.end(),.9f);g.Upload(depth.Get(),depths);inputs();rejected("disocclusion depth guard");
    std::fill(depths.begin(),depths.end(),.5f);g.Upload(depth.Get(),depths);
    for(UINT i=0;i<n;++i)vectors[i*2]=std::numeric_limits<float>::quiet_NaN();
    g.Upload(motion.Get(),vectors);inputs();rejected("NaN motion rejection");
    std::fill(vectors.begin(),vectors.end(),0);g.Upload(motion.Get(),vectors);
    // Change the raw input, independently of the model output: reject history.
    for(UINT i=0;i<n;++i)for(UINT c=0;c<3;++c)pixels[i*4+c]=.1f;
    g.Upload(raw.Get(),pixels);inputs();v=g.Read(history.Warped());Require(v[3]==0 && std::abs(v[0]-.1f)<1e-6f,"raw disagreement");
    for(UINT i=0;i<n;++i)for(UINT c=0;c<3;++c)pixels[i*4+c]=.4f;
    g.Upload(raw.Get(),pixels);inputs();
    std::vector<float> next=model;for(auto &f:next)f+=.01f;g.Upload(output.Get(),next);p.smoothStrength=.5f;
    finish();auto smoothed=g.Read(output.Get());const float weight=.5f*(1-.01f/(8.f/255.f));
    Require(std::abs(smoothed[0]-(next[0]+weight*(model[0]-next[0])))<2e-6f,"bounded output smoothing");
    // Black history must not become a self-sustaining dark trail.
    p.smoothStrength=0;g.Upload(output.Get(),std::vector<float>(n*3,.001f));finish();inputs();rejected("black history guard");
    // Invalid network output is not replaced by a previous bright frame.
    g.Upload(output.Get(),model);finish();inputs();p.smoothStrength=.5f;
    g.Upload(output.Get(),std::vector<float>(n*3,0));finish();auto zero=g.Read(output.Get());
    for(auto f:zero)Require(f==0,"zero fallback stays zero");inputs();rejected("zero history invalid");
    p.useHistory=0;inputs();rejected("reset ignores previous frame");
    // A fitted viewport has padding; never sample history from that padding.
    p.viewX=2;p.viewWidth=4;g.Upload(output.Get(),model);finish();p.useHistory=1;inputs();v=g.Read(history.Warped());
    Require(v[3]==0 && v[2*4+3]==1 && v[6*4+3]==0,"fitted viewport padding");
    // Read actual game-style formats, including the depth plane of a depth/stencil
    // resource. Extra motion channels and unused mips must not affect XY sampling.
    p.viewX=0;p.viewWidth=w;p.smoothStrength=0;
    g.Upload(raw.Get(),pixels);g.Upload(output.Get(),model);
    for(auto mf:{DXGI_FORMAT_R16G16B16A16_FLOAT,DXGI_FORMAT_R16G16B16A16_TYPELESS,
                 DXGI_FORMAT_R32G32B32A32_FLOAT,DXGI_FORMAT_R32G32B32A32_TYPELESS})
    for(auto df:{DXGI_FORMAT_R32G8X24_TYPELESS,DXGI_FORMAT_R24G8_TYPELESS})
    {
        auto mv=g.Texture(w,h,mf,2);
        auto dz=g.Texture(w,h,df,2,D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        Require(!LmxxfTemporal::TextureIssue(mv->GetDesc())&&!LmxxfTemporal::TextureIssue(dz->GetDesc()),"mip-zero guide admission");
        const bool half=mf==DXGI_FORMAT_R16G16B16A16_FLOAT||mf==DXGI_FORMAT_R16G16B16A16_TYPELESS;
        std::vector<float> data(n*(half?2:4),0);
        if(half) {
            for(UINT i=0;i<n;++i) {
                const UINT16 xyzw[]={0x3c00,0,0x7bff,0xfbff}; // 1, 0, large unused Z/W
                std::memcpy(reinterpret_cast<char*>(data.data())+i*8,xyzw,sizeof(xyzw));
            }
        } else for(UINT i=0;i<n;++i) {
            data[i*4]=1;data[i*4+2]=data[i*4+3]=std::numeric_limits<float>::quiet_NaN();
        }
        g.Upload(mv.Get(),data);
        const auto dsv=df==DXGI_FORMAT_R32G8X24_TYPELESS?DXGI_FORMAT_D32_FLOAT_S8X24_UINT:DXGI_FORMAT_D24_UNORM_S8_UINT;
        g.ClearDepth(dz.Get(),dsv,.5f);
        const auto ms=D3D12_RESOURCE_STATE_COPY_SOURCE,ds=D3D12_RESOURCE_STATE_DEPTH_READ;
        const auto guideStates=[&](bool enter){g.Run([&](auto *cmd){
            LmxxfTemporal::Transition(cmd,mv.Get(),enter?ReadState:ms,enter?ms:ReadState,0);
            LmxxfTemporal::Transition(cmd,dz.Get(),enter?ReadState:ds,enter?ds:ReadState,0);
            // Mip 1 is deliberately in a different state and is never sampled.
            for(auto *r:{mv.Get(),dz.Get()})
                LmxxfTemporal::Transition(cmd,r,enter?ReadState:D3D12_RESOURCE_STATE_COPY_DEST,
                                         enter?D3D12_RESOURCE_STATE_COPY_DEST:ReadState,1);
        });};
        guideStates(true);
        LmxxfTemporal::History test;test.Create(g.device.Get(),w,h,h);test.PrepareBinding(g.device.Get(),mv.Get(),dz.Get());
        p.useHistory=0;
        const auto prepare=[&]{g.Run([&](auto *cmd){test.RecordInputs(cmd,raw.Get(),output.Get(),mv.Get(),dz.Get(),ms,ds,p);});};
        prepare();
        g.Run([&](auto *cmd){test.RecordOutputs(cmd,raw.Get(),output.Get(),dz.Get(),ds,p);});
        p.useHistory=1;prepare();v=g.Read(test.Warped());
        Require(v[3*4+3]==1 && std::abs(v[3*4]-model[4*3])<1e-6f,"RGBA XY and depth/stencil plane-zero reprojection");
        Require(v[7*4+3]==0,"RGBA motion offscreen rejection");
        guideStates(false);g.ClearDepth(dz.Get(),dsv,.9f);guideStates(true);prepare();v=g.Read(test.Warped());
        Require(v[3*4+3]==0 && std::abs(v[3*4]-pixels[3*4])<1e-6f,"depth/stencil disocclusion rejection");
        guideStates(false);
        std::printf("temporal guide readback: motion=%u depth=%u mips=2 PASS\n",unsigned(mf),unsigned(df));
    }
    g.NoErrors();std::puts("temporal WARP: PASS (identity, motion, display grid, jitter, depth/raw/black guards, smoothing, zero recovery, reset, fit viewport)");return 0;
}
catch(const std::exception &e){std::fprintf(stderr,"FAIL: %s\n",e.what());return 1;}
