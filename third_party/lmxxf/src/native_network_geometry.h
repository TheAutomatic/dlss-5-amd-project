#pragma once
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <cstdio>
#include <stdexcept>

struct NativeNetworkGeometry {
 unsigned valid_width,valid_height,processing_width,processing_height;
 /* DLSS5_NETWORK_FREE_RES=1 (2026-10-02): a free geometry carries its own ViT token grid (vit_w x vit_h); 0 = one of the fixed
    tiers, whose grid follows from valid_height exactly as before. */
 unsigned vit_w=0,vit_h=0;
 bool Free()const{return vit_w!=0;}
 unsigned VitTokens()const{if(vit_w)return vit_w*vit_h;return valid_height==720?240u:valid_height==900?400u:640u;}
 static NativeNetworkGeometry FromHeight(unsigned height){
  if(height==720)return {1280,720,1280,768};
  if(height==900)return {1600,900,1600,960}; /* 2026-09-17: the 900 tier pads to 960 rows (60 reflected rows + one zero token row, as the 1080 tier pads 30x18 -> 32x20 tokens); 1024 rows (124 reflected rows) was the 0.20/0.21 layout, -0.9 ms (6%) for the same kernels */
  if(height==1024)return {1600,900,1600,1024}; /* "900w": the 0.21 layout, kept for the bench whose bit-exact goldens were recorded on it */
  if(height==1080)return {1920,1080,1920,1152};
  if(height==1088)return {1920,1080,1920,1088}; /* 2026-09-30 compact 1080 tier (opt-in, LOSSY, not NVIDIA's geometry): 1080 rows + 8 reflected rows = 1088 (2^6*17, as Daniel/mochizuki);
                                                   the ViT head keeps the 32x20 token grid with 30x17 valid tokens (1152 has 30x18) */
  throw std::runtime_error("unsupported network height");
 }
 /* DLSS5_NETWORK_HEIGHT=auto (2026-09-17): the smallest tier the input fits in — <=1280x720 -> 720, <=1600x900 -> 900, else 1080 (inputs beyond 1920x1080 are rejected before this).
    2026-09-26: an input at most 10% beyond a tier on both axes is fitted DOWN into it instead of UP into the next one. 2K quality
    (1707x961) went to 1080 = bilinear upscale into 1920x1152, 35% more pixels than the input carries; fitted into 900 (1599x900) it
    costs a 6% downscale and the Stellar Blade frame went 49-50 -> 60 (capped). 1080 inputs still take 1080. */
 /* DLSS5_NETWORK_1080_ROWS=1088 (default 1152): the 1080 tier -- fixed or chosen by auto -- runs on 1920x1088 instead of NVIDIA's 1920x1152. */
 static bool Compact1080(){const char*v=std::getenv("DLSS5_NETWORK_1080_ROWS");if(!v||!*v||!std::strcmp(v,"1152"))return false;if(!std::strcmp(v,"1088"))return true;throw std::runtime_error("DLSS5_NETWORK_1080_ROWS must be 1152 or 1088");}
 static bool Near(unsigned width,unsigned height,unsigned tw,unsigned th){return width*10u<=tw*11u&&height*10u<=th*11u;}
 /* Free geometry (DLSS5_NETWORK_FREE_RES=1), results/free-res-20261002:
    rows/columns: NVIDIA's plan walk as mochizuki reproduces it from nvngx_dlssnr 310.8 (nr_native_plan.cpp; the host function is
    0x18003c580, results/fma-vs-nvidia-20260928/geometry-audit.md): each axis up to a multiple of 64 (at least 320), and when both
    are multiples of 256 the width takes 64 more. One exception: a 1088-row result is taken to 1152 rows, NVIDIA's captured 1080
    call (in-game capture, and against NVIDIA's NGX output 1152 measures 47.43 dB vs 45.76 for 1088); so a 1920x1080 input runs
    exactly the 1080 tier. Against NGX at 1440p this rule's 2560x1472 measures 46.41 dB, the step-128 candidates 42.4..43.8.
    The "both multiples of 256" case is not cosmetic: unpadded it leaves no zero ViT tokens and the picture shifts (1440p
    2560x1536: 30.8 dB; the 720 tier 1280x768 is that case too, 28 dB from both padded alternatives).
    The input sits at the top-left; columns past it are mirrored by the input codec, rows by the RGB input pass (2*extent-2-x).
    ViT grid: each axis /64 rounded up to 4 (NVIDIA's 1920x1152: 30x18 -> 32x20), so the token count is a multiple of 16.
    Deeper NVIDIA levels also round to 4 (e.g. 1472/32 = 46 -> 48 rows); we run the unrounded raster there (as the 900 tier does).
    valid = the network surface the D3D side sees (padded width x input height); processing = padded width x padded height. */
 static constexpr unsigned free_step=64,free_min=320,free_max_axis=8192;
 static constexpr unsigned long long free_max_pixels=3840ull*2176ull; /* 4K processing surface */
 static unsigned FreePad(unsigned v){unsigned p=(v+free_step-1)/free_step*free_step;return p<free_min?free_min:p;}
 /* Diagnostics (results/free-res-20261002): DLSS5_NETWORK_FREE_PAD=WxH forces the processing size (multiples of 64);
    DLSS5_NETWORK_FREE_EXTRA=w|h|0 (default w) picks the axis of the "both multiples of 256" step. */
 static void FreeProcessing(unsigned width,unsigned height,unsigned&w,unsigned&h){
  if(const char*f=std::getenv("DLSS5_NETWORK_FREE_PAD")){unsigned fw=0,fh=0;if(std::sscanf(f,"%ux%u",&fw,&fh)!=2||fw<width||fh<height||fw%64||fh%64||fw+2>2*width||fh+2>2*height)throw std::runtime_error("DLSS5_NETWORK_FREE_PAD must be WxH, multiples of 64, at least the input and within one mirror");w=fw;h=fh;return;}
  w=FreePad(width);h=FreePad(height);if(h==1088)h=1152;const char*e=std::getenv("DLSS5_NETWORK_FREE_EXTRA");const char axis=e&&*e?e[0]:'w';
  if(axis!='w'&&axis!='h'&&axis!='0')throw std::runtime_error("DLSS5_NETWORK_FREE_EXTRA must be w, h or 0");
  if(axis!='0'&&w%(4*free_step)==0&&h%(4*free_step)==0)(axis=='w'?w:h)+=free_step;
 }
 static bool FreeFits(unsigned width,unsigned height){
  if(width<free_min||height<free_min||width>free_max_axis||height>free_max_axis)return false;
  unsigned w,h;FreeProcessing(width,height,w,h);
  return w<=free_max_axis&&h<=free_max_axis&&1ull*w*h<=free_max_pixels;
 }
 static NativeNetworkGeometry Free(unsigned width,unsigned height){
  if(!FreeFits(width,height))throw std::runtime_error("free network geometry out of range");
  unsigned w,h;FreeProcessing(width,height,w,h);
  NativeNetworkGeometry g{w,height,w,h};g.vit_w=(w/64+3)/4*4;g.vit_h=(h/64+3)/4*4;
  if(w==1600&&(h==960||h==1024)){g.vit_w=25;g.vit_h=16;} /* a processing size that is a tier keeps the tier's grid (the HIP host decides by size: TierGeometry) */
  return g;
 }
 static NativeNetworkGeometry ForInput(unsigned width,unsigned height){
  if(Near(width,height,1280,720))return FromHeight(720);
  if(Near(width,height,1600,900))return FromHeight(900);
  return FromHeight(Compact1080()?1088u:1080u);
 }
};
/* The geometry selected for the current frame set-up: written by NativeResolveNetworkGeometry (frame Create, once the input size is
   known) and read by every stage through NativeCurrentNetworkGeometry. Fixed tiers resolve to themselves; "auto" needs the input. */
/* DLSS5_FIT_LARGE=1: fit (downsample) inputs larger than 1920x1080 instead of rejecting them (issue #6). The environment is only
   populated by the background initializer, but the pre-upscale hook decides "supported" before initialization (phase 0) and that
   decision arms the initializer -- so callers that run early set the override from native-game-flags.txt. Not cached. */
inline bool&NativeFitLargeInputOverride(){static bool v=false;return v;}
inline bool NativeFitLargeInput(){if(const char*e=std::getenv("DLSS5_FIT_LARGE"))return e[0]=='1'&&!e[1];return NativeFitLargeInputOverride();}
/* DLSS5_NETWORK_FREE_RES=0/1 (default 0): 1 = run the network on the input's own size (NativeNetworkGeometry::Free) instead of
   snapping to the 720/900/1080 tiers; overrides DLSS5_NETWORK_HEIGHT. Inputs outside FreeFits (below 320 or beyond the 4K
   surface) keep the tier selection. The override is set by callers that decide before the flags reach the environment. */
inline bool&NativeNetworkFreeResOverride(){static bool v=false;return v;}
inline bool NativeNetworkFreeRes(){if(const char*e=std::getenv("DLSS5_NETWORK_FREE_RES")){if(!std::strcmp(e,"0"))return false;if(!std::strcmp(e,"1"))return true;throw std::runtime_error("DLSS5_NETWORK_FREE_RES must be 0 or 1");}return NativeNetworkFreeResOverride();}
/* Admission of inputs beyond the 1920x1080 budget: FIT_LARGE (downsample) or FREE_RES (native, within FreeFits). */
inline bool NativeAdmitLargeInput(){return NativeFitLargeInput()||NativeNetworkFreeRes();}
inline NativeNetworkGeometry*NativeNetworkGeometrySlot(){static NativeNetworkGeometry g{};return &g;}
inline bool&NativeNetworkGeometryResolved(){static bool resolved=false;return resolved;}
inline int NativeNetworkHeightFlag(){ /* 720/900/1080 (1024 = "900w", the 0.21 layout of the 900 tier; "900s" is an alias of 900), 0 = auto, -1 = unset. Narrow getenv on every platform: the bench's flag loader
 only refreshes the narrow CRT environment, and every other DLSS5_HIP_* flag is read the same way. */
 if(const char*height=std::getenv("DLSS5_NETWORK_HEIGHT")){
  if(!std::strcmp(height,"720"))return 720;if(!std::strcmp(height,"900"))return 900;if(!std::strcmp(height,"1080"))return NativeNetworkGeometry::Compact1080()?1088:1080;if(!std::strcmp(height,"1088"))return 1088;if(!std::strcmp(height,"900s"))return 900;if(!std::strcmp(height,"900w"))return 1024;if(!std::strcmp(height,"auto"))return 0;
  throw std::runtime_error("DLSS5_NETWORK_HEIGHT must be 720, 900, 1080, 1088 or auto");
 }
 const char*value=std::getenv("DLSS5_NETWORK_720P");
 if(value&&!(value[0]=='0'&&!value[1])&&!(value[0]=='1'&&!value[1]))throw std::runtime_error("invalid DLSS5_NETWORK_720P flag");
 return value&&value[0]=='1'?720:-1;
}
inline NativeNetworkGeometry NativeResolveNetworkGeometry(unsigned input_width,unsigned input_height){
 if(NativeNetworkFreeRes()&&NativeNetworkGeometry::FreeFits(input_width,input_height)){*NativeNetworkGeometrySlot()=NativeNetworkGeometry::Free(input_width,input_height);NativeNetworkGeometryResolved()=true;return *NativeNetworkGeometrySlot();}
 int flag=NativeNetworkHeightFlag();
 *NativeNetworkGeometrySlot()=flag==0?NativeNetworkGeometry::ForInput(input_width,input_height):NativeNetworkGeometry::FromHeight(flag<0?(NativeNetworkGeometry::Compact1080()?1088u:1080u):unsigned(flag));
 NativeNetworkGeometryResolved()=true;return *NativeNetworkGeometrySlot();
}
inline NativeNetworkGeometry NativeCurrentNetworkGeometry(){
 if(NativeNetworkGeometryResolved())return *NativeNetworkGeometrySlot();
 int flag=NativeNetworkHeightFlag();
 if(flag==0)throw std::runtime_error("DLSS5_NETWORK_HEIGHT=auto needs the input size first (NativeResolveNetworkGeometry)");
 return NativeNetworkGeometry::FromHeight(flag<0?(NativeNetworkGeometry::Compact1080()?1088u:1080u):unsigned(flag));
}
