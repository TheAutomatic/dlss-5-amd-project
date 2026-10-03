#pragma once
#include "native_lab_paths.h"
#include <cstdio>
#include <cstring>
#include <cwchar>
/* Colour-format fallback table (0.38, 2026-09-30; after mochizuki 0.0.2.4's NR_FORMAT_FALLBACK).
   Consulted ONLY where NativeIsGameColor() says no, so every format accepted before keeps exactly its old code path.
   Each entry is an uncompressed RGB DXGI format the GPU decodes to float through a typed SRV; the returned value is the
   SRV view format (typeless -> its UNORM/FLOAT member). DXGI_FORMAT_UNKNOWN = not in the table.
   R9G9B9E5_SHAREDEXP (RE Engine: RE9, MH Wilds) is linear HDR like R11G11B10. The add-on converts a fallback colour into a
   private RGBA16F texture (native_format_convert.h) and feeds that into the unchanged RGBA16F route; the RE9 runtime
   reads it through the encoder's SRV and keeps its private FP16 output (the route RGB9E5 already used there).
   Switch: DLSS5_FORMAT_FALLBACK=0 (environment, else the flags file) keeps only the original table. Default 1. */
inline DXGI_FORMAT NativeFallbackColorView(DXGI_FORMAT f){
 switch(f){
 case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:case DXGI_FORMAT_B8G8R8X8_UNORM:case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
 case DXGI_FORMAT_R10G10B10A2_UNORM:case DXGI_FORMAT_R32G32B32A32_FLOAT:case DXGI_FORMAT_R32G32B32_FLOAT:
 case DXGI_FORMAT_R16G16B16A16_SNORM:case DXGI_FORMAT_R8G8B8A8_SNORM:
 case DXGI_FORMAT_B5G6R5_UNORM:case DXGI_FORMAT_B5G5R5A1_UNORM:case DXGI_FORMAT_B4G4R4A4_UNORM:return f;
 case DXGI_FORMAT_B8G8R8X8_TYPELESS:return DXGI_FORMAT_B8G8R8X8_UNORM;
 case DXGI_FORMAT_R10G10B10A2_TYPELESS:return DXGI_FORMAT_R10G10B10A2_UNORM;
 case DXGI_FORMAT_R32G32B32A32_TYPELESS:return DXGI_FORMAT_R32G32B32A32_FLOAT;
 case DXGI_FORMAT_R32G32B32_TYPELESS:return DXGI_FORMAT_R32G32B32_FLOAT;
 default:return DXGI_FORMAT_UNKNOWN;
 }
}
/* formats without a meaningful alpha: the conversion writes alpha 1 */
inline bool NativeFallbackOpaque(DXGI_FORMAT view){return view==DXGI_FORMAT_R9G9B9E5_SHAREDEXP||view==DXGI_FORMAT_B8G8R8X8_UNORM||view==DXGI_FORMAT_B8G8R8X8_UNORM_SRGB||view==DXGI_FORMAT_R32G32B32_FLOAT||view==DXGI_FORMAT_B5G6R5_UNORM;}
inline bool NativeFormatFallbackOn(){
 static const bool on=[]{
  if(const wchar_t*v=_wgetenv(L"DLSS5_FORMAT_FALLBACK"))return wcscmp(v,L"0")!=0;
  unsigned x=1;for(const std::string&cfg_line:NativeConfigFileLines()){const char*line=cfg_line.c_str();sscanf(line,"DLSS5_FORMAT_FALLBACK=%u",&x);}
  return x!=0;}();
 return on;
}
/* The format of a fallback colour this build takes (after the switch), else UNKNOWN. */
inline DXGI_FORMAT NativeFallbackColor(DXGI_FORMAT f){return NativeIsGameColor(f)||!NativeFormatFallbackOn()?DXGI_FORMAT_UNKNOWN:NativeFallbackColorView(f);}
/* Name for logs (rejections must say which format). */
inline const char*NativeDxgiFormatName(DXGI_FORMAT f){
 switch(f){
#define N(x) case DXGI_FORMAT_##x:return #x;
 N(UNKNOWN)N(R32G32B32A32_TYPELESS)N(R32G32B32A32_FLOAT)N(R32G32B32A32_UINT)N(R32G32B32A32_SINT)N(R32G32B32_TYPELESS)N(R32G32B32_FLOAT)
 N(R16G16B16A16_TYPELESS)N(R16G16B16A16_FLOAT)N(R16G16B16A16_UNORM)N(R16G16B16A16_UINT)N(R16G16B16A16_SNORM)N(R16G16B16A16_SINT)
 N(R32G32_TYPELESS)N(R32G32_FLOAT)N(R10G10B10A2_TYPELESS)N(R10G10B10A2_UNORM)N(R10G10B10A2_UINT)N(R11G11B10_FLOAT)
 N(R8G8B8A8_TYPELESS)N(R8G8B8A8_UNORM)N(R8G8B8A8_UNORM_SRGB)N(R8G8B8A8_UINT)N(R8G8B8A8_SNORM)N(R8G8B8A8_SINT)
 N(R16G16_TYPELESS)N(R16G16_FLOAT)N(R32_TYPELESS)N(R32_FLOAT)N(R16_FLOAT)N(R8_UNORM)N(R9G9B9E5_SHAREDEXP)
 N(B5G6R5_UNORM)N(B5G5R5A1_UNORM)N(B8G8R8A8_UNORM)N(B8G8R8X8_UNORM)N(R10G10B10_XR_BIAS_A2_UNORM)N(B8G8R8A8_TYPELESS)
 N(B8G8R8A8_UNORM_SRGB)N(B8G8R8X8_TYPELESS)N(B8G8R8X8_UNORM_SRGB)N(B4G4R4A4_UNORM)N(BC1_UNORM)N(BC7_UNORM)
#undef N
 default:return "DXGI_FORMAT_?";
 }
}
