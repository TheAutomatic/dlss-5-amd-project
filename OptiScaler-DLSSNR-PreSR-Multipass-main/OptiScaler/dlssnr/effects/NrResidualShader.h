#pragma once

namespace DlssNr::Effects
{
// Independently implemented spatial residual shaping. The caller supplies
// ReadCorrection/WriteHistory; temporal callers supply already-stabilized
// RGB, and publish their unscaled history BEFORE any display-only adjustment.
// All math stays in the input resource's RGB domain (no assumed SDR Oklab).
inline constexpr char ResidualComposeShader[] = R"(
float Peak(float3 v) { return max(max(abs(v.x),abs(v.y)),abs(v.z)); }

float3 ComposeIntensity(float3 b,float3 r) {
 float3 c=lerp(b,r,min(intensity,1));
 if(intensity>1){
  float3 limit=.5*max(max(abs(b),abs(r)),.001);
  c=r+clamp(r-b,-limit,limit)*(intensity-1);
 }
 return clamp(c,-65504,65504);
}

// A soft colour heuristic, NOT a face/person mask. Reject signed RGB instead
// of interpreting negative scRGB primaries as skin. Ratios are exposure-invariant.
float SkinConfidence(float3 c) {
 float total=c.x+c.y+c.z;
 float3 n=c/max(total,1e-6);
 float rg=n.r-n.g,gb=n.g-n.b;
 float weight=smoothstep(.015,.075,rg)*(1-smoothstep(.20,.35,rg))*
              smoothstep(-.01,.035,gb)*(1-smoothstep(.15,.28,gb));
 return all(c>=0)&&total>1e-6?weight:0;
}

groupshared float4 originalTile[100];
groupshared float3 correctedTile[100];

float3 ShapeResidual(uint centre) {
 float3 b=originalTile[centre].rgb,r=correctedTile[centre],delta=r-b;
 if(lowGain==1&&detailGain==1&&skinProtection==0&&edgeProtection==0)return r;
 float scale=max(Peak(b),1e-6),total=0,edge=0;
 float3 low=0,baseLow=0;
 // A 3x3 binomial kernel guided by the ORIGINAL image; this does not blur
 // original detail and does not classify a new NR halo as a real boundary.
 [unroll]for(int y=-1;y<=1;++y)[unroll]for(int x=-1;x<=1;++x){
  uint tap=uint(int(centre)+y*10+x);
  float3 qb=originalTile[tap].rgb,qr=correctedTile[tap];
  float difference=Peak(qb-b);
  float relative=difference/max(max(scale,Peak(qb)),1e-6);
  float weight=(x==0?2.:1.)*(y==0?2.:1.)/(1+64*relative*relative);
  low+=(qr-qb)*weight;baseLow+=qb*weight;total+=weight;
  edge=max(edge,relative);
 }
 low/=total;baseLow/=total;
 float3 high=(delta-low)*detailGain;
 float magnitude=Peak(high);
 // Both protections scale the RGB residual together, preserving its direction.
 // Keep the original image intact; constrain only introduced fine-scale detail.
 float skinLimit=.25*Peak(b-baseLow)+.015*scale;
 float edgeLimit=.125*Peak(b-baseLow)+.02*scale;
 float skin=skinProtection*SkinConfidence(b);
 float boundary=edgeProtection*smoothstep(.08,.30,edge);
 float retain=min(lerp(1,min(1,skinLimit/max(magnitude,1e-6)),skin),
                  lerp(1,min(1,edgeLimit/max(magnitude,1e-6)),boundary));
 return b+lowGain*low+retain*high;
}

// Separate entrypoint: neutral/uniform gains compile without LDS or a barrier.
[numthreads(8,8,1)]void point_main(uint3 id:SV_DispatchThreadID) {
 if(id.x>=width||id.y>=height)return;
 float4 b,h;float3 r=ReadCorrection(int2(id.xy),b,h);
 WriteHistory(int2(id.xy),h);
 if(lowGain!=1)r=b.rgb+lowGain*(r-b.rgb);
 output[id.xy]=float4(ComposeIntensity(b.rgb,r),b.a);
}

[numthreads(8,8,1)]
void main(uint3 id:SV_DispatchThreadID,uint3 group:SV_GroupID,
          uint3 local:SV_GroupThreadID,uint lane:SV_GroupIndex) {
 // Every lane, including out-of-bounds lanes, reaches the barrier. The 1-pixel
 // halo uses the same temporal correction as the centre, never raw noisy NR.
 for(uint i=lane;i<100;i+=64){
  int2 tile=int2(i%10,i/10),pixel=int2(group.xy*8)+tile-1;
  int2 p=clamp(pixel,0,int2(width-1,height-1));
  float4 b,h;float3 r=ReadCorrection(p,b,h);
  originalTile[i]=float4(clamp(b.rgb,-65504,65504),b.a);
  correctedTile[i]=clamp(r,-65504,65504);
  if(all(tile>=1)&&all(tile<=8)&&all(pixel<int2(width,height)))WriteHistory(p,h);
 }
 GroupMemoryBarrierWithGroupSync();
 if(id.x>=width||id.y>=height)return;
 uint centre=(local.y+1)*10+local.x+1;
 float4 b=originalTile[centre];
 output[id.xy]=float4(ComposeIntensity(b.rgb,ShapeResidual(centre)),b.a);
}
)";
}
