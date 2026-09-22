#pragma once
// R0600 ABI types from ROCm/HIP rocm-7.1.1 include/hip/hip_runtime_api.h.
/*
Copyright (c) 2015 - 2023 Advanced Micro Devices, Inc. All rights reserved.

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/
#include <cstddef>
namespace hip_probe {
typedef struct {
  // 32-bit Atomics
  unsigned hasGlobalInt32Atomics : 1;     ///< 32-bit integer atomics for global memory.
  unsigned hasGlobalFloatAtomicExch : 1;  ///< 32-bit float atomic exch for global memory.
  unsigned hasSharedInt32Atomics : 1;     ///< 32-bit integer atomics for shared memory.
  unsigned hasSharedFloatAtomicExch : 1;  ///< 32-bit float atomic exch for shared memory.
  unsigned hasFloatAtomicAdd : 1;         ///< 32-bit float atomic add in global and shared memory.

  // 64-bit Atomics
  unsigned hasGlobalInt64Atomics : 1;  ///< 64-bit integer atomics for global memory.
  unsigned hasSharedInt64Atomics : 1;  ///< 64-bit integer atomics for shared memory.

  // Doubles
  unsigned hasDoubles : 1;  ///< Double-precision floating point.

  // Warp cross-lane operations
  unsigned hasWarpVote : 1;     ///< Warp vote instructions (__any, __all).
  unsigned hasWarpBallot : 1;   ///< Warp ballot instructions (__ballot).
  unsigned hasWarpShuffle : 1;  ///< Warp shuffle operations. (__shfl_*).
  unsigned hasFunnelShift : 1;  ///< Funnel two words into one with shift&mask caps.

  // Sync
  unsigned hasThreadFenceSystem : 1;  ///< __threadfence_system.
  unsigned hasSyncThreadsExt : 1;     ///< __syncthreads_count, syncthreads_and, syncthreads_or.

  // Misc
  unsigned hasSurfaceFuncs : 1;        ///< Surface functions.
  unsigned has3dGrid : 1;              ///< Grid and group dims are 3D (rather than 2D).
  unsigned hasDynamicParallelism : 1;  ///< Dynamic parallelism.
} hipDeviceArch_t;
typedef struct hipUUID_t {
  char bytes[16];
} hipUUID;
typedef struct DevicePropertiesR0600 {
  char name[256];                   ///< Device name.
  hipUUID uuid;                     ///< UUID of a device
  char luid[8];                     ///< 8-byte unique identifier. Only valid on windows
  unsigned int luidDeviceNodeMask;  ///< LUID node mask
  size_t totalGlobalMem;            ///< Size of global memory region (in bytes).
  size_t sharedMemPerBlock;         ///< Size of shared memory per block (in bytes).
  int regsPerBlock;                 ///< Registers per block.
  int warpSize;                     ///< Warp size.
  size_t memPitch;                  ///< Maximum pitch in bytes allowed by memory copies
                                    ///< pitched memory
  int maxThreadsPerBlock;           ///< Max work items per work group or workgroup max size.
  int maxThreadsDim[3];             ///< Max number of threads in each dimension (XYZ) of a block.
  int maxGridSize[3];               ///< Max grid dimensions (XYZ).
  int clockRate;                    ///< Max clock frequency of the multiProcessors in khz.
  size_t totalConstMem;             ///< Size of shared constant memory region on the device
                                    ///< (in bytes).
  int major;  ///< Major compute capability version.  This indicates the core instruction set
              ///< of the GPU architecture.  For example, a value of 11 would correspond to
              ///< Navi III (RDNA3).  See the arch feature flags for portable ways to query
              ///< feature caps.
  int minor;  ///< Minor compute capability version.  This indicates a particular configuration,
              ///< feature set, or variation within the group represented by the major compute
              ///< capability version.  For example, different models within the same major version
              ///< might have varying levels of support for certain features or optimizations.
              ///< See the arch feature flags for portable ways to query feature caps.
  size_t textureAlignment;       ///< Alignment requirement for textures
  size_t texturePitchAlignment;  ///< Pitch alignment requirement for texture references bound to
  int deviceOverlap;             ///< Deprecated. Use asyncEngineCount instead
  int multiProcessorCount;       ///< Number of multi-processors. When the GPU works in Compute
                                 ///< Unit (CU) mode, this value equals the number of CUs;
                                 ///< when in Workgroup Processor (WGP) mode, this value equels
                                 ///< half of CUs, because a single WGP contains two CUs.
  int kernelExecTimeoutEnabled;  ///< Run time limit for kernels executed on the device
  int integrated;                ///< APU vs dGPU
  int canMapHostMemory;          ///< Check whether HIP can map host memory
  int computeMode;               ///< Compute mode.
  int maxTexture1D;              ///< Maximum number of elements in 1D images
  int maxTexture1DMipmap;        ///< Maximum 1D mipmap texture size
  int maxTexture1DLinear;        ///< Maximum size for 1D textures bound to linear memory
  int maxTexture2D[2];  ///< Maximum dimensions (width, height) of 2D images, in image elements
  int maxTexture2DMipmap[2];   ///< Maximum number of elements in 2D array mipmap of images
  int maxTexture2DLinear[3];   ///< Maximum 2D tex dimensions if tex are bound to pitched memory
  int maxTexture2DGather[2];   ///< Maximum 2D tex dimensions if gather has to be performed
  int maxTexture3D[3];         ///< Maximum dimensions (width, height, depth) of 3D images, in image
                               ///< elements
  int maxTexture3DAlt[3];      ///< Maximum alternate 3D texture dims
  int maxTextureCubemap;       ///< Maximum cubemap texture dims
  int maxTexture1DLayered[2];  ///< Maximum number of elements in 1D array images
  int maxTexture2DLayered[3];  ///< Maximum number of elements in 2D array images
  int maxTextureCubemapLayered[2];  ///< Maximum cubemaps layered texture dims
  int maxSurface1D;                 ///< Maximum 1D surface size
  int maxSurface2D[2];              ///< Maximum 2D surface size
  int maxSurface3D[3];              ///< Maximum 3D surface size
  int maxSurface1DLayered[2];       ///< Maximum 1D layered surface size
  int maxSurface2DLayered[3];       ///< Maximum 2D layared surface size
  int maxSurfaceCubemap;            ///< Maximum cubemap surface size
  int maxSurfaceCubemapLayered[2];  ///< Maximum cubemap layered surface size
  size_t surfaceAlignment;          ///< Alignment requirement for surface
  int concurrentKernels;            ///< Device can possibly execute multiple kernels concurrently.
  int ECCEnabled;                   ///< Device has ECC support enabled
  int pciBusID;                     ///< PCI Bus ID.
  int pciDeviceID;                  ///< PCI Device ID
  int pciDomainID;                  ///< PCI Domain ID
  int tccDriver;                    ///< 1:If device is Tesla device using TCC driver, else 0
  int asyncEngineCount;             ///< Number of async engines
  int unifiedAddressing;            ///< Does device and host share unified address space
  int memoryClockRate;              ///< Max global memory clock frequency in khz.
  int memoryBusWidth;               ///< Global memory bus width in bits.
  int l2CacheSize;                  ///< L2 cache size.
  int persistingL2CacheMaxSize;     ///< Device's max L2 persisting lines in bytes
  int maxThreadsPerMultiProcessor;  ///< Maximum resident threads per multi-processor.
  int streamPrioritiesSupported;    ///< Device supports stream priority
  int globalL1CacheSupported;       ///< Indicates globals are cached in L1
  int localL1CacheSupported;        ///< Locals are cahced in L1
  size_t sharedMemPerMultiprocessor;  ///< Amount of shared memory available per multiprocessor.
  int regsPerMultiprocessor;          ///< registers available per multiprocessor
  int managedMemory;                  ///< Device supports allocating managed memory on this system
  int isMultiGpuBoard;                ///< 1 if device is on a multi-GPU board, 0 if not.
  int multiGpuBoardGroupID;  ///< Unique identifier for a group of devices on same multiboard GPU
  int hostNativeAtomicSupported;         ///< Link between host and device supports native atomics
  int singleToDoublePrecisionPerfRatio;  ///< Deprecated. CUDA only.
  int pageableMemoryAccess;              ///< Device supports coherently accessing pageable memory
                                         ///< without calling hipHostRegister on it
  int concurrentManagedAccess;  ///< Device can coherently access managed memory concurrently with
                                ///< the CPU
  int computePreemptionSupported;         ///< Is compute preemption supported on the device
  int canUseHostPointerForRegisteredMem;  ///< Device can access host registered memory with same
                                          ///< address as the host
  int cooperativeLaunch;                  ///< HIP device supports cooperative launch
  int cooperativeMultiDeviceLaunch;       ///< HIP device supports cooperative launch on multiple
                                          ///< devices
  size_t sharedMemPerBlockOptin;  ///< Per device m ax shared mem per block usable by special opt in
  int pageableMemoryAccessUsesHostPageTables;  ///< Device accesses pageable memory via the host's
                                               ///< page tables
  int directManagedMemAccessFromHost;  ///< Host can directly access managed memory on the device
                                       ///< without migration
  int maxBlocksPerMultiProcessor;      ///< Max number of blocks on CU
  int accessPolicyMaxWindowSize;       ///< Max value of access policy window
  size_t reservedSharedMemPerBlock;    ///< Shared memory reserved by driver per block
  int hostRegisterSupported;           ///< Device supports hipHostRegister
  int sparseHipArraySupported;         ///< Indicates if device supports sparse hip arrays
  int hostRegisterReadOnlySupported;   ///< Device supports using the hipHostRegisterReadOnly flag
                                       ///< with hipHostRegistger
  int timelineSemaphoreInteropSupported;  ///< Indicates external timeline semaphore support
  int memoryPoolsSupported;    ///< Indicates if device supports hipMallocAsync and hipMemPool APIs
  int gpuDirectRDMASupported;  ///< Indicates device support of RDMA APIs
  unsigned int gpuDirectRDMAFlushWritesOptions;  ///< Bitmask to be interpreted according to
                                                 ///< hipFlushGPUDirectRDMAWritesOptions
  int gpuDirectRDMAWritesOrdering;               ///< value of hipGPUDirectRDMAWritesOrdering
  unsigned int
      memoryPoolSupportedHandleTypes;    ///< Bitmask of handle types support with mempool based IPC
  int deferredMappingHipArraySupported;  ///< Device supports deferred mapping HIP arrays and HIP
                                         ///< mipmapped arrays
  int ipcEventSupported;                 ///< Device supports IPC events
  int clusterLaunch;                     ///< Device supports cluster launch
  int unifiedFunctionPointers;           ///< Indicates device supports unified function pointers
  int reserved[63];                      ///< CUDA Reserved.

  int hipReserved[32];  ///< Reserved for adding new entries for HIP/CUDA.

  /* HIP Only struct members */
  char gcnArchName[256];                    ///< AMD GCN Arch Name. HIP Only.
  size_t maxSharedMemoryPerMultiProcessor;  ///< Maximum Shared Memory Per CU. HIP Only.
  int clockInstructionRate;  ///< Frequency in khz of the timer used by the device-side "clock*"
                             ///< instructions.  New for HIP.
  hipDeviceArch_t arch;      ///< Architectural feature flags.  New for HIP.
  unsigned int* hdpMemFlushCntl;                ///< Addres of HDP_MEM_COHERENCY_FLUSH_CNTL register
  unsigned int* hdpRegFlushCntl;                ///< Addres of HDP_REG_COHERENCY_FLUSH_CNTL register
  int cooperativeMultiDeviceUnmatchedFunc;      ///< HIP device supports cooperative launch on
                                                ///< multiple
                                                /// devices with unmatched functions
  int cooperativeMultiDeviceUnmatchedGridDim;   ///< HIP device supports cooperative launch on
                                                ///< multiple
                                                /// devices with unmatched grid dimensions
  int cooperativeMultiDeviceUnmatchedBlockDim;  ///< HIP device supports cooperative launch on
                                                ///< multiple
                                                /// devices with unmatched block dimensions
  int cooperativeMultiDeviceUnmatchedSharedMem;  ///< HIP device supports cooperative launch on
                                                 ///< multiple
                                                 /// devices with unmatched shared memories
  int isLargeBar;                                ///< 1: if it is a large PCI bar device, else 0
  int asicRevision;                              ///< Revision of the GPU in this device
} DevicePropertiesR0600;
static_assert(sizeof(DevicePropertiesR0600)==1472 && offsetof(DevicePropertiesR0600,gcnArchName)==1160,"HIP R0600 x64 ABI");
}
