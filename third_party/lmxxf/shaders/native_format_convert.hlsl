// Colour-format fallback (native_format_convert.h): any float-readable RGB game format -> RGBA16F.
// The typed SRV does the decode (RGB9E5 shared exponent, 10:10:10:2, 5:6:5, sRGB views, R32G32B32...).
// Out-of-range floats clamp to the half range and NaN becomes 0 so the network never sees Inf/NaN.
Texture2D<float4> source:register(t0);
RWTexture2D<float4> target:register(u0);
cbuffer Constants:register(b0){uint width;uint height;uint opaque;};
[numthreads(8,8,1)]void main(uint3 id:SV_DispatchThreadID){
 if(id.x>=width||id.y>=height)return;
 float4 v=source.Load(int3(id.xy,0));
 v=clamp(v,-65504.0,65504.0);v=v==v?v:0.0;
 if(opaque)v.a=1.0;
 target[id.xy]=v;
}
