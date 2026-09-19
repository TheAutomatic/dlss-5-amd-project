#pragma once
#include <cstdlib>
#include <cwchar>
#include <cstring>
#include <stdexcept>

struct NativeNetworkGeometry {
 unsigned valid_width,valid_height,processing_width,processing_height;
 unsigned VitTokens()const{return valid_height==720?240u:valid_height==900?400u:640u;}
 static NativeNetworkGeometry FromHeight(unsigned height){
  if(height==720)return {1280,720,1280,768};
  if(height==900)return {1600,900,1600,960}; /* 2026-09-17: the 900 tier pads to 960 rows (60 reflected rows + one zero token row, as the 1080 tier pads 30x18 -> 32x20 tokens); 1024 rows (124 reflected rows) was the 0.20/0.21 layout, -0.9 ms (6%) for the same kernels */
  if(height==1024)return {1600,900,1600,1024}; /* "900w": the 0.21 layout, kept for the bench whose bit-exact goldens were recorded on it */
  if(height==1080)return {1920,1080,1920,1152};
  throw std::runtime_error("unsupported network height");
 }
 /* DLSS5_NETWORK_HEIGHT=auto (2026-09-17): the smallest tier the input fits in — <=1280x720 -> 720, <=1600x900 -> 900, else 1080 (inputs beyond 1920x1080 are rejected before this). */
 static NativeNetworkGeometry ForInput(unsigned width,unsigned height){
  if(width<=1280&&height<=720)return FromHeight(720);
  if(width<=1600&&height<=900)return FromHeight(900);
  return FromHeight(1080);
 }
};
/* The geometry selected for the current frame set-up: written by NativeResolveNetworkGeometry (frame Create, once the input size is
   known) and read by every stage through NativeCurrentNetworkGeometry. Fixed tiers resolve to themselves; "auto" needs the input. */
inline NativeNetworkGeometry*NativeNetworkGeometrySlot(){static NativeNetworkGeometry g{};return &g;}
inline bool&NativeNetworkGeometryResolved(){static bool resolved=false;return resolved;}
inline int NativeNetworkHeightFlag(){ /* 720/900/1080 (1024 = "900w", the 0.21 layout of the 900 tier; "900s" is an alias of 900), 0 = auto, -1 = unset. Narrow getenv on every platform: the bench's flag loader
 only refreshes the narrow CRT environment, and every other DLSS5_HIP_* flag is read the same way. */
 if(const char*height=std::getenv("DLSS5_NETWORK_HEIGHT")){
  if(!std::strcmp(height,"720"))return 720;if(!std::strcmp(height,"900"))return 900;if(!std::strcmp(height,"1080"))return 1080;if(!std::strcmp(height,"900s"))return 900;if(!std::strcmp(height,"900w"))return 1024;if(!std::strcmp(height,"auto"))return 0;
  throw std::runtime_error("DLSS5_NETWORK_HEIGHT must be 720, 900, 1080 or auto");
 }
 const char*value=std::getenv("DLSS5_NETWORK_720P");
 if(value&&!(value[0]=='0'&&!value[1])&&!(value[0]=='1'&&!value[1]))throw std::runtime_error("invalid DLSS5_NETWORK_720P flag");
 return value&&value[0]=='1'?720:-1;
}
inline NativeNetworkGeometry NativeResolveNetworkGeometry(unsigned input_width,unsigned input_height){
 int flag=NativeNetworkHeightFlag();
 *NativeNetworkGeometrySlot()=flag==0?NativeNetworkGeometry::ForInput(input_width,input_height):NativeNetworkGeometry::FromHeight(flag<0?1080u:unsigned(flag));
 NativeNetworkGeometryResolved()=true;return *NativeNetworkGeometrySlot();
}
inline NativeNetworkGeometry NativeCurrentNetworkGeometry(){
 if(NativeNetworkGeometryResolved())return *NativeNetworkGeometrySlot();
 int flag=NativeNetworkHeightFlag();
 if(flag==0)throw std::runtime_error("DLSS5_NETWORK_HEIGHT=auto needs the input size first (NativeResolveNetworkGeometry)");
 return NativeNetworkGeometry::FromHeight(flag<0?1080u:unsigned(flag));
}
