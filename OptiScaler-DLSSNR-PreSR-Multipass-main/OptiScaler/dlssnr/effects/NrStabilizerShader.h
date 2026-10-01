#pragma once

namespace DlssNr::Effects
{
// Adapts the residual-filtering algorithm from MatheusFerreiraS/neural-amd-opti,
// commit 2f39f1908b6b03ae16be79d4b072ef6261c666bf,
// OptiScaler/dlssnr/amd/ResidualStabilizer.h (GPL-3.0).
// Local shader changes add depth-weighted bilinear history, invalid-guide
// rejection, and Overall Intensity after storing the unscaled residual.
// NrOutputEffects owns product recording lifetimes and history publication.
inline constexpr char StabilizerShader[] = R"(
Texture2D<float4> original:register(t0),result:register(t1);
Texture2D<float2> motion:register(t2);
Texture2D<float> depth:register(t3);
Texture2D<float4> history:register(t4);
RWTexture2D<float4> output:register(u0),nextHistory:register(u1);
cbuffer Params:register(b0){
 uint width,height,mvWidth,mvHeight;
 float2 mvScale,jitterStep;
 float alpha,threshold,preExposure;uint flags;
 float intensity;float3 padding;
}
float3 Encode(float3 c){
 float3 t=clamp(c,0,65504)/preExposure;t=t/(1+t);
 return t<=.0031308?12.92*t:1.055*pow(t,1.0/2.4)-.055;
}
float3 Decode(float3 e){
 float3 t=clamp(e,0,.99999326);t=t<=.04045?t/12.92:pow((t+.055)/1.055,2.4);
 return min(t/max(1-t,1e-7)*preExposure,65504);
}
float Key(int2 p){
 float d=depth.Load(int3(p,0));
 if(!isfinite(d)||d<0||d>1)return -1;
 return (flags&2)?d:1-d;
}
void Accumulate(int2 p,float weight,float key,inout float3 sum,inout float total){
 float4 h=history.Load(int3(p,0));
 if(all(isfinite(h))&&h.a>=0&&abs(h.a-key)<=.1*max(max(h.a,key),1e-6)){
  sum+=h.rgb*weight;total+=weight;
 }
}
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=width||id.y>=height)return;
 int2 p=int2(id.xy),tap=p;
 float4 b=original.Load(int3(p,0)),r=result.Load(int3(p,0));
 bool colourValid=all(isfinite(b))&&all(isfinite(r))&&all(b.rgb>=0)&&all(r.rgb>=0);
 if(!all(isfinite(b)))b=0;
 if(!all(isfinite(r)))r=b;
 float3 e=Encode(b.rgb),residual=Encode(r.rgb)-e,filtered=residual;
 float key=Key(p),best=key;
 for(int y=-1;y<=1;++y)for(int x=-1;x<=1;++x){
  int2 q=clamp(p+int2(x,y),0,int2(width-1,height-1));float k=Key(q);
  if(k>best){best=k;tap=q;}
 }
 uint2 m=min(uint2((float2(tap)+.5)*float2(mvWidth,mvHeight)/float2(width,height)),uint2(mvWidth-1,mvHeight-1));
 float2 mv=motion.Load(int3(m,0));
 float2 prev=float2(p)+mv*mvScale+jitterStep;
 bool valid=(flags&1)&&colourValid&&key>=0&&all(isfinite(mv))&&all(isfinite(prev))&&
  all(prev>=0)&&all(prev<=float2(width-1,height-1));
 if(valid){
  int2 lo=int2(floor(prev)),hi=min(lo+1,int2(width-1,height-1));float2 f=frac(prev);
  float3 sum=0;float total=0;
  Accumulate(lo,(1-f.x)*(1-f.y),key,sum,total);
  Accumulate(int2(hi.x,lo.y),f.x*(1-f.y),key,sum,total);
  Accumulate(int2(lo.x,hi.y),(1-f.x)*f.y,key,sum,total);
  Accumulate(hi,f.x*f.y,key,sum,total);
  valid=total>1e-6;
  if(valid)filtered=lerp(residual,clamp(sum/total,residual-threshold,residual+threshold),alpha);
 }
 // Invalid motion/colour cannot seed a usable history on the next frame.
 nextHistory[p]=float4(filtered,colourValid&&all(isfinite(mv))?key:-1);
 float3 corrected=valid?Decode(e+filtered):r.rgb;
 float3 c=lerp(b.rgb,corrected,min(intensity,1));
 if(intensity>1){float3 limit=.5*max(max(abs(b.rgb),abs(corrected)),.001);
  c=corrected+clamp(corrected-b.rgb,-limit,limit)*(intensity-1);}
 output[p]=float4(clamp(c,-65504,65504),b.a);
}
)";
struct StabilizerConstants
{
    unsigned width, height, mvWidth, mvHeight;
    float mvScaleX, mvScaleY, jitterX, jitterY;
    float alpha, threshold, preExposure;
    unsigned flags;
    float intensity, padding[3] {};
};
static_assert(sizeof(StabilizerConstants) == 64);
}
