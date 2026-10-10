#pragma once
namespace DlssNr::Person
{
inline constexpr unsigned HistoryCount=24;
inline const char* Shader=R"(
Texture2D<float4> original:register(t0), firstPass:register(t1), finalPass:register(t2);
Texture2D<float2> motion:register(t3);
Texture2D<float> depth:register(t4);
Texture2D<float4> guides[25]:register(t5);
Buffer<float> rawMask:register(t30);
Texture2D<float2> warpedMask:register(t31), previousMask:register(t32);
RWBuffer<float> capture:register(u0);
RWTexture2D<float4> nextGuide:register(u1);
RWTexture2D<float2> nextMask:register(u2);
RWTexture2D<float4> output:register(u3);
cbuffer Params:register(b0){
 uint width,height,mvWidth,mvHeight;
 float2 mvScale,jitterStep;
 float preExposure;uint captureEnabled,age,maskEnabled;
 float2 contentScale,contentOffset;
 uint inverted;float personStrength,personDetail,maskFreshness;
 uint historyEnabled;float riseBlend,fallBlend,debugMask;
}
cbuffer Execution:register(b1){uint executionValid;uint3 padding;}
float Key(float value){return isfinite(value)&&value>=0&&value<=1?(inverted?value:1-value):-1;}
float3 Encode(float3 value){
 value=saturate(value/max(preExposure,1e-6));
 return value<=.0031308?12.92*value:1.055*pow(value,1.0/2.4)-.055;
}
[numthreads(8,8,1)]
void capture_main(uint3 id:SV_DispatchThreadID){
 if(id.x<640&&id.y<640&&captureEnabled){
  float2 uv=((float2(id.xy)+.5)/640-contentOffset)/contentScale;
  float3 rgb=.447;
  if(all(uv>=0)&&all(uv<1)){
   int2 p=min(int2(uv*float2(width,height)),int2(width-1,height-1));
   rgb=Encode(original.Load(int3(p,0)).rgb);if(!all(isfinite(rgb)))rgb=0;
  }
  uint i=id.y*640+id.x;capture[i]=rgb.r;capture[i+640*640]=rgb.g;capture[i+2*640*640]=rgb.b;
 }
 if(id.x<160&&id.y<160){
  float2 uv=(float2(id.xy)+.5)/160;
  int2 p=min(int2(uv*float2(width,height)),int2(width-1,height-1));
  int2 m=min(int2(uv*float2(mvWidth,mvHeight)),int2(mvWidth-1,mvHeight-1));
  float2 mv=motion.Load(int3(m,0))*mvScale+jitterStep;
  float key=Key(depth.Load(int3(p,0)));
  nextGuide[id.xy]=float4(mv,key,all(isfinite(mv))&&key>=0?1:0);
 }
}
float4 GuideAt(uint index,float2 uv){
 int2 p=clamp(int2(uv*160),0,159);float4 value=0;
 // SM5 resource indices are compile-time constants.
 [unroll]for(uint i=0;i<25;++i)if(index==i)value=guides[i].Load(int3(p,0));
 return value;
}
float2 Warp(float2 uv){
 if(!maskEnabled||!executionValid||age>24)return 0;
 float4 g=GuideAt(0,uv);if(g.w<.5)return 0;
 [loop]for(uint i=0;i<age;++i){
  float2 nextUv=uv+g.xy;
  if(any(nextUv<0)||any(nextUv>=1))return 0;
  float4 prev=GuideAt(i+1,nextUv);
  if(prev.w<.5||abs(g.z-prev.z)>0.35*max(max(g.z,prev.z),1e-4)+0.05)return 0;
  uv=nextUv;
  g=prev;
 }
 float2 p=(uv*contentScale+contentOffset)*160-.5;
 int2 lo=clamp(int2(floor(p)),0,159),hi=min(lo+1,159);float2 f=frac(p);
 float value=lerp(lerp(rawMask[lo.y*160+lo.x],rawMask[lo.y*160+hi.x],f.x),
                  lerp(rawMask[hi.y*160+lo.x],rawMask[hi.y*160+hi.x],f.x),f.y);
 return isfinite(value)?float2(smoothstep(.2,.8,value),1):0;
}
[numthreads(8,8,1)]
void warp_main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=160))return;
 float2 uv=(float2(id.xy)+.5)/160;
 float2 current=Warp(uv);
 // Smooth only successful reprojections, never a disoccluded or expired mask.
 // The source is the previous submitted frame, not the last inference result.
 if(current.y>.5&&historyEnabled){
  float4 g=GuideAt(0,uv);float2 previousUv=uv+g.xy;
  if(all(previousUv>=0)&&all(previousUv<1)){
   float2 p=previousUv*160-.5;int2 lo=int2(floor(p));float2 f=frac(p);
   float value=0,weight=0;
   [unroll]for(int y=0;y<2;++y)[unroll]for(int x=0;x<2;++x){
    int2 q=clamp(lo+int2(x,y),0,159);
    float4 oldGuide=guides[1].Load(int3(q,0));float2 old=previousMask.Load(int3(q,0));
    float w=(x?f.x:1-f.x)*(y?f.y:1-f.y);
    if(old.y>.5&&oldGuide.w>.5&&abs(g.z-oldGuide.z)<=0.35*max(max(g.z,oldGuide.z),1e-4)+0.05){value+=old.x*w;weight+=w;}
   }
   if(weight>.5){value/=weight;current.x=lerp(value,current.x,current.x>value?riseBlend:fallBlend);}
  }
 }
 // Keep history unfaded; expiry fade is applied once in composition.
 nextMask[id.xy]=current;
}
// Reduce only the fine component of the person's NR correction. The original
// image supplies the edge weights and is never itself blurred.
float3 PersonColour(int2 pixel,float3 base,float3 nr) {
 float3 delta=nr-base;
 if(personDetail!=1) {
  float3 low=0;float total=0;
  float scale=max(max(abs(base.r),max(abs(base.g),abs(base.b))),1e-6);
  [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x) {
   int2 p=clamp(pixel+int2(x,y),0,int2(width-1,height-1));
   float3 b=original.Load(int3(p,0)).rgb,r=firstPass.Load(int3(p,0)).rgb;
   if(!all(isfinite(b))||!all(isfinite(r)))continue;
   float3 difference=abs(b-base);
   float denominator=max(scale,max(max(abs(b.r),abs(b.g)),abs(b.b)));
   float relative=max(max(difference.r,difference.g),difference.b)/denominator;
   float weight=(x==0?2.:1.)*(y==0?2.:1.)/(1+64*relative*relative);
   low+=(r-b)*weight;total+=weight;
  }
  low=total>0?low/total:delta;
  delta=low+personDetail*(delta-low);
 }
 // Preserve the legacy first-pass colour exactly at neutral settings.
 if(personStrength==1&&personDetail==1)return nr;
 return clamp(base+personStrength*delta,-65504,65504);
}
[numthreads(8,8,1)]
void compose_main(uint3 id:SV_DispatchThreadID){
 if(id.x>=width||id.y>=height)return;
 float4 final=finalPass.Load(int3(id.xy,0)), first=firstPass.Load(int3(id.xy,0));
 float4 base=original.Load(int3(id.xy,0));
 if(!all(isfinite(final)))final=base;
 if(!all(isfinite(first)))first=final;
 float2 uv=(float2(id.xy)+.5)/float2(width,height),p=uv*160-.5;
 int2 center=clamp(int2(round(p)),0,159);float key=Key(depth.Load(int3(id.xy,0)));
 float mask=0,total=0;
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  int2 q=clamp(center+int2(x,y),0,159);float4 g=guides[0].Load(int3(q,0));
  float spatialW=max(0.0,1.5-abs(p.x-q.x))*max(0.0,1.5-abs(p.y-q.y));
  float depthDiff=abs(g.z-key);
  float depthTol=0.35*max(max(g.z,key),1e-4)+0.05;
  float depthW=(g.w>.5&&key>=0)?saturate(1.0-depthDiff/depthTol):0.0;
  float w=spatialW*depthW;
  mask+=warpedMask.Load(int3(q,0)).x*w;total+=w;
 }
 float4 centerG=guides[0].Load(int3(center,0));
 float centerDepthDiff=abs(centerG.z-key);
 float centerDepthTol=0.35*max(max(centerG.z,key),1e-4)+0.05;
 float fallbackWeight=(key>=0&&centerG.w>.5)?saturate(1-centerDepthDiff/centerDepthTol):0;
 float fallbackMask=warpedMask.Load(int3(center,0)).x*fallbackWeight;
 mask=(total>0.01?saturate(mask/total):fallbackMask)*maskFreshness;
 float3 person=mask>0&&all(isfinite(base.rgb))?PersonColour(int2(id.xy),base.rgb,first.rgb):first.rgb;
 float3 composed=lerp(final.rgb,person,mask);
 if(debugMask>0.5){
  float3 tint=float3(0.08,0.92,0.33);
  composed=lerp(composed,tint,mask*0.7);
 }
 output[id.xy]=float4(composed,isfinite(base.a)?base.a:0);
}
)";
struct Constants {
 unsigned width,height,mvWidth,mvHeight;
 float mvScaleX,mvScaleY,jitterX,jitterY;
 float preExposure; unsigned captureEnabled,age,maskEnabled;
 float scaleX,scaleY,offsetX,offsetY;
 unsigned inverted;float personStrength=1.f,personDetail=1.f,maskFreshness=1.f;
 unsigned historyEnabled=0;float riseBlend=1.f,fallBlend=1.f,debugMask=0.f;
};
static_assert(sizeof(Constants)==96);
}

