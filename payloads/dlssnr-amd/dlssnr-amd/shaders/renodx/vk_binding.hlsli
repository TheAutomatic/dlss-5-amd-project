// Vulkan (vulkan_path.hpp): the vk_*.comp.slang wrappers define
// RENODX_VULKAN and compile the D3D12 codec shaders to SPIR-V.  Set 0: t0..t3
// at bindings 0..3, u0/u1 at 4/5; the cbuffer becomes the push constants
// (std430 keeps every member at its D3D12 cbuffer offset: no member of these
// cbuffers straddles a 16-byte register).
#ifndef RENODX_VK_BINDING_HLSLI
#define RENODX_VK_BINDING_HLSLI
#ifdef RENODX_VULKAN
#define VK_BINDING(n) [[vk::binding(n, 0)]]
#define PUSH_CONSTANTS(name) [[vk::push_constant]] cbuffer name
// Vulkan commits directly into the game's output view. Scratch reads stay
// zero-based; only the final write uses the DLSS output subrect base.
#define VK_OUTPUT_PIXEL(pixel) ((pixel) + SourceBase)
// Slang lowers f16tof32 / f32tof16 through 16-bit types, which declares the
// SPIR-V Float16 and Int16 capabilities - device features a game need not
// enable (VUID-VkShaderModuleCreateInfo-pCode-08740).  GLSL.std.450's half
// packing needs neither; the low half is the scalar, as in HLSL.
float VkF16ToF32(uint word) {
  const float2 pair = spirv_asm { result:$$float2 = OpExtInst glsl450 UnpackHalf2x16 $word };
  return pair.x;
}
uint VkF32ToF16(float value) {
  const float2 pair = float2(value, 0.f);
  return spirv_asm { result:$$uint = OpExtInst glsl450 PackHalf2x16 $pair };
}
#define f16tof32 VkF16ToF32
#define f32tof16 VkF32ToF16
#else
#define VK_BINDING(n)
#define PUSH_CONSTANTS(name) cbuffer name : register(b0)
#define VK_OUTPUT_PIXEL(pixel) (pixel)
#endif
#endif
