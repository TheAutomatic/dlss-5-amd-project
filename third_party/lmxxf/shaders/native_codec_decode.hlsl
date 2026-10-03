cbuffer CodecConstants : register(b0) {
 uint2 Size; uint2 SourceSize; uint2 SourceBase; uint2 ProxySize;
 float PaperWhiteScale; float TransferStrength; float ColorStrength; uint HdrMode;
 float4 Padding;
 uint OutputRowPitch; uint3 Reserved;
};
#ifndef NATIVE_CODEC_EXPOSURE
#define NATIVE_CODEC_EXPOSURE 0
#endif
#if NATIVE_CODEC_EXPOSURE
Texture2D<float> GameExposure : register(t4);
#endif
// Same meter as encode; sample the original game colour so both sides share the white point.
Texture2D<float4> Proxy : register(t1);
Texture2D<float4> Neural : register(t2);
Texture2D<float4> OutputOriginal : register(t3);
static const float kTargetEncodedMean = 0.45f;
float WhitePointForMean(float meanLuma) {
    float encoded = pow(kTargetEncodedMean, 2.2f);
    float ratio = encoded / (1.0 - encoded);
    float wp = meanLuma / ratio;
    return clamp(wp, 0.01f, 10000.0f);
}
float SampleMeanLuma() {
    uint w = max(SourceSize.x, 1u), h = max(SourceSize.y, 1u);
    float sum = 0.0;
    const int N = 5;
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            uint2 p = uint2(uint((x + 0.5) * (w - 1) / (N - 1)), uint((y + 0.5) * (h - 1) / (N - 1)));
            p = min(p, uint2(w, h) - 1);
            float3 c = max(OutputOriginal.Load(int3(p, 0)).rgb, 0.0);
            sum += dot(c, float3(0.2126, 0.7152, 0.0722));
        }
    }
    return max(sum / float(N * N), 1e-4);
}
float EffectivePaperWhite() {
#if NATIVE_CODEC_EXPOSURE
 float e=GameExposure.Load(int3(0,0,0));
 float pre=asfloat(Reserved.y),scale=asfloat(Reserved.z);
 float exposure=e*scale/pre;
 return PaperWhiteScale / ((isfinite(exposure)&&exposure>0)?exposure:1.0);
#else
 if ((Reserved.x & 0x10000u) != 0)
     return WhitePointForMean(SampleMeanLuma()) * PaperWhiteScale;
 float pre=asfloat(Reserved.y);
 if (isfinite(pre)&&pre>0)
     return PaperWhiteScale*pre;
 return PaperWhiteScale;
#endif
}
#ifndef NATIVE_CODEC_FIT
#define NATIVE_CODEC_FIT 0
#endif
uint ByteOffset(uint2 p,uint bpp) {
#if NATIVE_CODEC_FIT
 return p.y*OutputRowPitch+p.x*bpp;
#else
 return (p.y*1920+p.x)*bpp;
#endif
}
// Mode1 candidate. Oracle: captured codec-22724-36e36d370.dxbc.
// Host must reject other modes; GPU comparison required before integration.
#ifndef NATIVE_CODEC_UINT_OUT
#define NATIVE_CODEC_UINT_OUT 0
#endif
#ifndef NATIVE_CODEC_SRGB_IO
#define NATIVE_CODEC_SRGB_IO 0
#endif
#ifndef NATIVE_CODEC_UNORM8_OUT
#define NATIVE_CODEC_UNORM8_OUT 0
#endif
#ifndef NATIVE_CODEC_BGRA
#define NATIVE_CODEC_BGRA 0
#endif
#if NATIVE_CODEC_UNORM8_OUT
// 8-bit UNORM host textures (Magpie): UNORM8 bits in a raw buffer (row pitch 1920*4), copied into the texture by the frame; BGRA byte order when NATIVE_CODEC_BGRA.
RWByteAddressBuffer OutputBits : register(u0);
#ifndef NATIVE_CODEC_DEBUG_TINT
#define NATIVE_CODEC_DEBUG_TINT 0
#endif
void Store(uint2 p,float4 v){
#if NATIVE_CODEC_DEBUG_TINT
 v.g*=0.25;
#endif
 uint4 q=uint4(round(saturate(v)*255.0));
#if NATIVE_CODEC_BGRA
 OutputBits.Store(ByteOffset(p,4),q.z|(q.y<<8)|(q.x<<16)|(q.w<<24));
#else
 OutputBits.Store(ByteOffset(p,4),q.x|(q.y<<8)|(q.z<<16)|(q.w<<24));
#endif
}
#elif NATIVE_CODEC_R11_OUT
// R11G11B10_FLOAT game textures (UE5 scene colour, Black Myth: Wukong): the three small floats packed into one 32-bit word per pixel in a
// raw buffer (row pitch width*4), copied into the texture by the frame. f11 = half bits >> 4 (5-bit exponent, 6-bit mantissa), f10 = half
// bits >> 5 (5-bit mantissa); negatives clamp to 0 (the format is unsigned), the range is that of FP16, no saturation.
RWByteAddressBuffer OutputBits : register(u0);
void Store(uint2 p,float4 v){
 uint3 h=f32tof16(max(v.rgb,0.0));
 uint r=(h.x>>4)&0x7FFu,g=(h.y>>4)&0x7FFu,b=(h.z>>5)&0x3FFu;
 OutputBits.Store(ByteOffset(p,4),r|(g<<11)|(b<<22));
}
#elif NATIVE_CODEC_UINT_OUT
// Typeless UNORM16 game textures (Rise of the Ronin): this driver device-removes on any non-float RGBA16 typed UAV, so the
// UNORM bits go into a raw buffer (row pitch 1920*8) that the frame copies into the game texture with CopyTextureRegion.
RWByteAddressBuffer OutputBits : register(u0);
void Store(uint2 p,float4 v){uint4 q=uint4(round(saturate(v)*65535.0));OutputBits.Store2(ByteOffset(p,8),uint2(q.x|(q.y<<16),q.z|(q.w<<16)));}
#else
RWTexture2D<float4> Output : register(u0);
void Store(uint2 p,float4 v){Output[p]=v;}
#endif
float Luminance(float3 c) { return dot(c,float3(0.212639,0.715169,0.072192)); }
float3 Decode(float3 c) {
 c=saturate(c);
 return c<=0.04045 ? c/12.92 : pow((c+0.055)/1.055,2.4);
}
float3 ToLab(float3 c) {
 const float3x3 a={0.4122214708,0.5363325363,0.0514459929,
  0.2119034982,0.6806995451,0.1073969566,0.0883024619,0.2817188376,0.6299787005};
 const float3x3 b={0.2104542553,0.7936177850,-0.0040720468,
  1.9779984951,-2.4285922050,0.4505937099,0.0259040371,0.7827717662,-0.8086757660};
 float3 l=mul(a,c);return mul(b,sign(l)*pow(abs(l),1.0/3.0));
}
float3 FromLab(float3 c) {
 const float3x3 a={1,0.3963377774,0.2158037573,1,-0.1055613458,-0.0638541728,1,-0.0894841775,-1.2914855480};
 const float3x3 b={4.0767416621,-3.3077115913,0.2309699292,
  -1.2684380046,2.6097574011,-0.3413193965,-0.0041960863,-0.7034186147,1.7076147010};
 float3 l=mul(a,c);return mul(b,l*l*l);
}
float3 ClampAp1(float3 c) {
 const float3x3 a={0.613097,0.339523,0.047379,0.070194,0.916354,0.013452,0.020616,0.109570,0.869815};
 const float3x3 b={1.705051,-0.621792,-0.083259,-0.130256,1.140805,-0.010548,-0.024003,-0.128969,1.152972};
 return mul(b,max(0,mul(a,c)));
}
float3 Hue(float3 incorrect,float3 correct) {
 float3 a=ToLab(incorrect),b=ToLab(correct);
 float ca=length(a.yz),cb=length(b.yz);
 a.yz=b.yz*(cb==0?1:ca/cb);
 return ClampAp1(FromLab(a));
}
float3 Upgrade(float3 original,float3 proxy,float3 neural) {
 float oy=Luminance(original),py=Luminance(proxy),ny=Luminance(neural);
 float3 result=original;
 if(!(ny<=1e-5)) {
  float ratio=0;
  if(oy<py)ratio=oy/max(py,1e-6);
  else ratio=(ny+max(0,oy-py))/ny;
  result=lerp(original,Hue(neural*ratio,neural),TransferStrength);
 }
 return result;
}
#if NATIVE_CODEC_FIT
float3 ReadFitted(Texture2D<float4> image,float2 p) {
 p=clamp(p,Padding.xy,Padding.xy+Padding.zw-1);
 uint2 lo=uint2(floor(p)),hi=min(lo+1,uint2(Padding.xy+Padding.zw-1));float2 f=p-lo;
 return lerp(lerp(image.Load(int3(lo,0)).rgb,image.Load(int3(hi.x,lo.y,0)).rgb,f.x),
             lerp(image.Load(int3(lo.x,hi.y,0)).rgb,image.Load(int3(hi,0)).rgb,f.x),f.y);
}
#endif
// Test18 status uses a tiny embedded glyph font: no host-menu dependency.
bool Test18Glyph(uint ch,uint2 p){
 uint2 bits=uint2(0,0);switch(ch){
 case 65u: bits=uint2(1663026734u,4u);break;
 case 66u: bits=uint2(3809986095u,3u);break;
 case 67u: bits=uint2(2182120510u,7u);break;
 case 68u: bits=uint2(3810051631u,3u);break;
 case 69u: bits=uint2(3256321087u,7u);break;
 case 70u: bits=uint2(1108837439u,0u);break;
 case 71u: bits=uint2(2736686142u,7u);break;
 case 72u: bits=uint2(1663026737u,4u);break;
 case 73u: bits=uint2(3359772831u,7u);break;
 case 76u: bits=uint2(3255862305u,7u);break;
 case 78u: bits=uint2(1939525233u,4u);break;
 case 79u: bits=uint2(2736309806u,3u);break;
 case 80u: bits=uint2(1108854319u,0u);break;
 case 82u: bits=uint2(1381484079u,4u);break;
 case 83u: bits=uint2(3775333438u,3u);break;
 case 84u: bits=uint2(138547359u,1u);break;
 case 85u: bits=uint2(2736309809u,3u);break;
 case 86u: bits=uint2(353945137u,1u);break;
 case 87u: bits=uint2(2002437681u,4u);break;
 case 89u: bits=uint2(138553905u,1u);break;
 case 48u: bits=uint2(2738546222u,3u);break;
 case 49u: bits=uint2(2286031044u,3u);break;
 case 50u: bits=uint2(3292807726u,7u);break;
 case 56u: bits=uint2(2736211502u,3u);break;
 case 57u: bits=uint2(2702132782u,3u);break;
 }uint bit=p.y*5+p.x;return ((bit<32?bits.x>>bit:bits.y>>(bit-32))&1u)!=0;
}
float4 Test18Display(float4 value,uint2 p){
 uint state=min((Reserved.x>>20)&7u,5u),seconds=(Reserved.x>>24)&31u;
 float3 ink=state==3?float3(.05,1,.05):state>=4?float3(1,.1,.1):state==2?float3(1,.8,.05):float3(1,1,1);
 int2 d=abs(int2(p)-int2(SourceBase));
 bool corner=(d.x>=66&&d.x<=68&&d.y>=53&&d.y<=68)||(d.y>=66&&d.y<=68&&d.x>=53&&d.x<=68);
 if(corner)value.rgb=ink;
 if(p.y>=12&&p.y<18&&p.x>=12&&p.x<612)value.rgb=(p.x<12+seconds*30?ink:ink*.15);
 if(p.x>=8&&p.x<596&&p.y>=22&&p.y<58){
  value.rgb=float3(.015,.015,.015);
  if(p.x>=12&&p.y>=26){uint2 cell=(p-uint2(12,26))/4;uint column=cell.x/6;
   static const uint labels[144]={84u,69u,83u,84u,49u,56u,32u,82u,69u,65u,68u,89u,32u,70u,57u,32u,83u,84u,65u,82u,84u,32u,32u,32u,84u,69u,83u,84u,49u,56u,32u,67u,65u,80u,84u,85u,82u,73u,78u,71u,32u,32u,32u,32u,32u,32u,32u,32u,84u,69u,83u,84u,49u,56u,32u,83u,65u,86u,73u,78u,71u,32u,87u,65u,73u,84u,32u,32u,32u,32u,32u,32u,84u,69u,83u,84u,49u,56u,32u,83u,65u,86u,69u,68u,32u,32u,32u,32u,32u,32u,32u,32u,32u,32u,32u,32u,84u,69u,83u,84u,49u,56u,32u,70u,65u,73u,76u,69u,68u,32u,83u,69u,69u,32u,76u,79u,71u,32u,32u,32u,72u,73u,83u,84u,79u,82u,89u,32u,79u,70u,70u,32u,68u,69u,66u,85u,71u,32u,79u,70u,70u,32u,32u,32u};
   if(column<24&&cell.x%6<5&&cell.y<7&&Test18Glyph(labels[state*24+column],uint2(cell.x%6,cell.y)))value.rgb=ink;
  }
 }return value;
}

float4 Test17Display(float4 value, uint2 p) {
 if ((Reserved.x & 0x40000000u) != 0) return Test18Display(value,p);
 if ((Reserved.x & 0x80000u) == 0) return value;
 uint mode=(Reserved.x>>20)&3u;
 float3 ink=mode==1?float3(0.05,1,0.05):mode==2?float3(0.1,0.5,1):float3(1,1,1);
 // Corners lie outside the captured 128-square. Never feed markers to the model.
 int2 d=abs(int2(p)-int2(SourceBase));
 bool corner=(d.x>=66&&d.x<=68&&d.y>=53&&d.y<=68)||(d.y>=66&&d.y<=68&&d.x>=53&&d.x<=68);
 uint seconds=(Reserved.x>>24)&31u;
 bool bar=p.y>=12&&p.y<17&&p.x>=12&&p.x<612;
 if(corner) value.rgb=ink;
 if(bar)value.rgb=p.x<12+seconds*30?ink:ink*.15;
 return value;
}
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID) {
 if(any(id.xy>=Size))return;
 if(HdrMode!=1||PaperWhiteScale<=0){Store(id.xy,0);return;}
 uint2 extent=max(ProxySize,uint2(1,1));
 uint2 p=min(uint2((float2(id.xy)+0.5)*float2(extent)/float2(Size)),extent-1);
 float4 source=OutputOriginal.Load(int3(id.xy,0));
 // Same producer/inference/consumer path, but omit the model edit for a control.
 if ((Reserved.x & 0x40000u) != 0) { Store(id.xy,Test17Display(source,id.xy)); return; }
#if NATIVE_CODEC_SRGB_IO
 /* DLSS5_CODEC_SRGB (Magpie): the source is display-referred sRGB; linearize it for the blend and re-encode the result */
 float3 original=Decode(saturate(source.rgb));
#else
 float3 original=max(source.rgb,0)/EffectivePaperWhite();
#endif
 #if NATIVE_CODEC_FIT
 float2 network_p=Padding.xy+(float2(id.xy)+.5)*Padding.zw/float2(Size)-.5;
 float3 upgraded=Upgrade(original,Decode(ReadFitted(Proxy,network_p)),Decode(ReadFitted(Neural,network_p)));
#else
 float3 upgraded=Upgrade(original,Decode(Proxy.Load(int3(p,0)).rgb),Decode(Neural.Load(int3(id.xy,0)).rgb));
#endif
 float oy=Luminance(original),uy=Luminance(upgraded);
 float ratio=oy==0?1:clamp(uy/oy,0,4);
 float3 hueSafe=original*ratio;
 /* CS<=0: original; 0..1: toward network luma with game chroma; >1: toy toward network colour. */
 float3 result=lerp(original,hueSafe,clamp(ColorStrength,0.0,1.0));
 result=lerp(result,upgraded,max(ColorStrength-1.0,0.0));
 // Optional per-dispatch views; view 0 preserves the captured composition exactly.
 if(Reserved.x==1||Reserved.x==2){
#if NATIVE_CODEC_FIT
  result=Reserved.x==1?Decode(ReadFitted(Proxy,network_p)):Decode(ReadFitted(Neural,network_p));
#else
  result=Reserved.x==1?Decode(Proxy.Load(int3(p,0)).rgb):Decode(Neural.Load(int3(id.xy,0)).rgb);
#endif
 }else if(Reserved.x==3)result=saturate(0.5+(upgraded-original)*20.0);
 else if(Reserved.x==4)result*=float3(1.2,0.3,1.2);

#if NATIVE_CODEC_SRGB_IO
 result=saturate(result);result=result<=0.0031308?result*12.92:1.055*pow(result,1.0/2.4)-0.055;
  Store(id.xy,Test17Display(float4(result,source.a),id.xy));
#else
  Store(id.xy,Test17Display(float4(result*EffectivePaperWhite(),source.a),id.xy));
#endif
}
