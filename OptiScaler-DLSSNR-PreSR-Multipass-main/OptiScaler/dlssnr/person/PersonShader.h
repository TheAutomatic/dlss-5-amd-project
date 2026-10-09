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
Texture2D<float> warpedMask:register(t31);
RWBuffer<float> capture:register(u0);
RWTexture2D<float4> nextGuide:register(u1);
RWTexture2D<float> nextMask:register(u2);
RWTexture2D<float4> output:register(u3);
cbuffer Params:register(b0){
 uint width,height,mvWidth,mvHeight;
 float2 mvScale,jitterStep;
 float preExposure;uint captureEnabled,age,maskEnabled;
 float2 contentScale,contentOffset;
 uint inverted;float3 reserved;
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
float Warp(float2 uv){
 if(!maskEnabled||!executionValid||age>24)return 0;
 float4 g=GuideAt(0,uv);if(g.w<.5)return 0;
 [loop]for(uint i=0;i<age;++i){
  uv+=g.xy;
  if(any(uv<0)||any(uv>=1))return 0;
  float4 prev=GuideAt(i+1,uv);
  if(prev.w<.5||abs(g.z-prev.z)>.08*max(max(g.z,prev.z),1e-6))return 0;
  g=prev;
 }
 float2 p=(uv*contentScale+contentOffset)*160-.5;
 int2 lo=clamp(int2(floor(p)),0,159),hi=min(lo+1,159);float2 f=frac(p);
 float value=lerp(lerp(rawMask[lo.y*160+lo.x],rawMask[lo.y*160+hi.x],f.x),
                  lerp(rawMask[hi.y*160+lo.x],rawMask[hi.y*160+hi.x],f.x),f.y);
 return isfinite(value)?smoothstep(.4,.8,value):0;
}
[numthreads(8,8,1)]
void warp_main(uint3 id:SV_DispatchThreadID){
 if(any(id.xy>=160))return;
 nextMask[id.xy]=Warp((float2(id.xy)+.5)/160);
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
  float w=max(0,1.5-abs(p.x-q.x))*max(0,1.5-abs(p.y-q.y));
  if(g.w>.5&&key>=0&&abs(g.z-key)<=.08*max(max(g.z,key),1e-6)){mask+=warpedMask.Load(int3(q,0))*w;total+=w;}
 }
 mask=total>0?saturate(mask/total):0;
 output[id.xy]=float4(lerp(final.rgb,first.rgb,mask),isfinite(base.a)?base.a:0);
}
)";
struct Constants {
 unsigned width,height,mvWidth,mvHeight;
 float mvScaleX,mvScaleY,jitterX,jitterY;
 float preExposure; unsigned captureEnabled,age,maskEnabled;
 float scaleX,scaleY,offsetX,offsetY;
 unsigned inverted;float reserved[3]{};
};
static_assert(sizeof(Constants)==80);
}

