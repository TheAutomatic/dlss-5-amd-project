RWByteAddressBuffer output : register(u0);
cbuffer Params : register(b0) { uint slot; uint value; };
[shader("raygeneration")]
void RayGen() { output.Store(slot * 4, value); }
