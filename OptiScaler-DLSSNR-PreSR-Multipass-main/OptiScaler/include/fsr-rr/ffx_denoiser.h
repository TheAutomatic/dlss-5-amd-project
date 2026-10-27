// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2026 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#include <ffx_api.h>
#include <ffx_api_types.h>

#include <stdint.h>

// SDK 1.1.4's shared API types predate these RR 1.2 additions. Keep the new
// denoiser ABI local instead of changing the API headers used by every effect.
typedef struct FfxApiFloatCoords3D
{
    float x;
    float y;
    float z;
} FfxApiFloatCoords3D;

typedef struct FfxApiFloat4
{
    float x;
    float y;
    float z;
    float w;
} FfxApiFloat4;

typedef struct FfxApiMatrix4x4
{
    FfxApiFloat4 rows[4];
} FfxApiMatrix4x4;

typedef struct FfxApiFloatBounds
{
    float min;
    float max;
} FfxApiFloatBounds;

#define FFX_DENOISER_VERSION_MAJOR 1
#define FFX_DENOISER_VERSION_MINOR 2
#define FFX_DENOISER_VERSION_PATCH 0

#define FFX_API_EFFECT_ID_DENOISER 0x00050000u
#define FFX_API_EFFECT_ID_RADIANCECACHE 0x00060000u

#define FFX_API_MAKE_EFFECT_SUB_ID(effectId, subversion)                                                               \
    (((effectId) & FFX_API_EFFECT_MASK) | ((subversion) & ~FFX_API_EFFECT_MASK))
#define FFX_DENOISER_MAKE_VERSION(major, minor, patch) (((major) << 22) | ((minor) << 12) | (patch))
#define FFX_DENOISER_VERSION                                                                                           \
    FFX_DENOISER_MAKE_VERSION(FFX_DENOISER_VERSION_MAJOR, FFX_DENOISER_VERSION_MINOR, FFX_DENOISER_VERSION_PATCH)

#define FFX_API_DISPATCH_DESC_TYPE_RADIANCECACHE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_RADIANCECACHE, 0x04)

#ifdef __cplusplus
extern "C"
{
#endif

#define FFX_API_CREATE_CONTEXT_DESC_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x01)
typedef struct ffxCreateContextDescDenoiser
{
    ffxCreateContextDescHeader header;
    uint32_t version;
    struct FfxApiDimensions2D maxRenderSize;
    uint32_t signalFlags;
    uint32_t checkerboardSignalFlags;
    uint32_t flags;
} ffxCreateContextDescDenoiser;

#define FFX_API_CONFIGURE_DESC_TYPE_DENOISER_KEYVALUE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x21)
typedef struct ffxConfigureDescDenoiserKeyValue
{
    ffxConfigureDescHeader header;
    uint64_t key;
    uint64_t count;
    const void* data;
} ffxConfigureDescDenoiserKeyValue;

typedef struct FfxApiDenoiserSignal
{
    struct FfxApiResource input;
    struct FfxApiResource output;
    uint32_t checkerboardOrigin;
} FfxApiDenoiserSignal;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x41)
typedef struct ffxDispatchDescDenoiser
{
    ffxDispatchDescHeader header;
    void* commandList;
    struct FfxApiResource linearDepth;
    struct FfxApiResource motionVectors;
    struct FfxApiResource normals;
    struct FfxApiResource specularAlbedo;
    struct FfxApiResource diffuseAlbedo;
    struct FfxApiFloatCoords3D motionVectorScale;
    struct FfxApiFloatCoords2D jitterOffsets;
    struct FfxApiFloatCoords3D cameraPositionDelta;
    FfxApiMatrix4x4 view;
    FfxApiMatrix4x4 projection;
    FfxApiFloatBounds linearDepthBounds;
    struct FfxApiDimensions2D renderSize;
    uint32_t frameIndex;
    uint32_t flags;
} ffxDispatchDescDenoiser;

#define FFX_API_DENOISER_DEBUG_VIEW_MAX_VIEWPORTS 12
#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DEBUG_VIEW FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x42)
typedef struct ffxDispatchDescDenoiserDebugView
{
    ffxDispatchDescHeader header;
    struct FfxApiResource output;
    struct FfxApiDimensions2D outputSize;
    uint32_t mode;
    uint32_t viewportIndex;
} ffxDispatchDescDenoiserDebugView;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_AMBIENT_OCCLUSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x43)
typedef struct ffxDispatchDescDenoiserAmbientOcclusion
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserAmbientOcclusion;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_DIFFUSE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x44)
typedef struct ffxDispatchDescDenoiserDirectDiffuse
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserDirectDiffuse;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DIRECT_SPECULAR FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x45)
typedef struct ffxDispatchDescDenoiserDirectSpecular
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserDirectSpecular;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_DOMINANT_LIGHT FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x46)
typedef struct ffxDispatchDescDenoiserDominantLight
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
    struct FfxApiFloatCoords3D direction;
    struct FfxApiFloatCoords3D emission;
    float angularRadius;
} ffxDispatchDescDenoiserDominantLight;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_DIFFUSE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x47)
typedef struct ffxDispatchDescDenoiserIndirectDiffuse
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserIndirectDiffuse;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_INDIRECT_SPECULAR FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x48)
typedef struct ffxDispatchDescDenoiserIndirectSpecular
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserIndirectSpecular;

#define FFX_API_DISPATCH_DESC_TYPE_DENOISER_SPECULAR_OCCLUSION FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x49)
typedef struct ffxDispatchDescDenoiserSpecularOcclusion
{
    ffxDispatchDescHeader header;
    struct FfxApiDenoiserSignal signal;
} ffxDispatchDescDenoiserSpecularOcclusion;

#define FFX_API_QUERY_DESC_TYPE_DENOISER_GET_DEFAULT_KEYVALUE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x81)
typedef struct ffxQueryDescDenoiserGetDefaultKeyValue
{
    ffxQueryDescHeader header;
    uint64_t key;
    uint64_t count;
    void* data;
} ffxQueryDescDenoiserGetDefaultKeyValue;

#define FFX_API_QUERY_DESC_TYPE_DENOISER_GPU_MEMORY_USAGE FFX_API_MAKE_EFFECT_SUB_ID(FFX_API_EFFECT_ID_DENOISER, 0x82)
typedef struct ffxQueryDescDenoiserGetGPUMemoryUsage
{
    ffxQueryDescHeader header;
    void* device;
    struct FfxApiDimensions2D maxRenderSize;
    uint32_t signalFlags;
    uint32_t checkerboardSignalFlags;
    uint32_t flags;
    struct FfxApiEffectMemoryUsage* gpuMemoryUsage;
} ffxQueryDescDenoiserGetGPUMemoryUsage;

typedef enum FfxApiCreateContextDenoiserFlags
{
    FFX_DENOISER_ENABLE_DEBUGGING = (1 << 0),
    FFX_DENOISER_ENABLE_VALIDATION = (1 << 1),
} FfxApiCreateContextDenoiserFlags;

typedef enum FfxApiConfigureDenoiserKey
{
    FFX_API_CONFIGURE_DENOISER_KEY_CROSS_BILATERAL_NORMAL_STRENGTH = 1,
    FFX_API_CONFIGURE_DENOISER_KEY_STABILITY_BIAS = 2,
    FFX_API_CONFIGURE_DENOISER_KEY_MAX_RADIANCE = 3,
    FFX_API_CONFIGURE_DENOISER_KEY_RADIANCE_CLIP_STD_K = 4,
    FFX_API_CONFIGURE_DENOISER_KEY_GAUSSIAN_KERNEL_RELAXATION = 5,
    FFX_API_CONFIGURE_DENOISER_KEY_DISOCCLUSION_THRESHOLD = 6,
    FFX_API_CONFIGURE_DENOISER_KEY_DEBUG_VIEW_LINEAR_DEPTH_BOUNDS = 7,
} FfxApiConfigureDenoiserKey;

typedef enum FfxApiDenoiserDebugViewMode
{
    FFX_API_DENOISER_DEBUG_VIEW_MODE_OVERVIEW = 0,
    FFX_API_DENOISER_DEBUG_VIEW_MODE_FULLSCREEN_VIEWPORT = 1,
} FfxApiDenoiserDebugViewMode;

typedef enum FfxApiDenoiserSignalFlags
{
    FFX_DENOISER_SIGNAL_NONE = 0,
    FFX_DENOISER_SIGNAL_AMBIENT_OCCLUSION = (1 << 0),
    FFX_DENOISER_SIGNAL_DIRECT_DIFFUSE = (1 << 1),
    FFX_DENOISER_SIGNAL_DIRECT_SPECULAR = (1 << 2),
    FFX_DENOISER_SIGNAL_DOMINANT_LIGHT_VISIBILITY = (1 << 3),
    FFX_DENOISER_SIGNAL_INDIRECT_DIFFUSE = (1 << 4),
    FFX_DENOISER_SIGNAL_INDIRECT_SPECULAR = (1 << 5),
    FFX_DENOISER_SIGNAL_SPECULAR_OCCLUSION = (1 << 6),
} FfxApiDenoiserSignalFlags;

typedef enum FfxApiDispatchDenoiserFlags
{
    FFX_DENOISER_DISPATCH_RESET = (1 << 0),
    FFX_DENOISER_DISPATCH_NON_GAMMA_ALBEDO = (1 << 1),
} FfxApiDispatchDenoiserFlags;

#ifdef __cplusplus
}
#endif
