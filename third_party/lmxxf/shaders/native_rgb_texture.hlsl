// Processing RGB1920x1152 -> valid RGBA16_FLOAT1920x1080.
// No tone mapping/normalization: output codec owns those operations.
StructuredBuffer<float> rgb : register(t0);
RWTexture2D<float4> output : register(u0);
[numthreads(16,16,1)]
void main(uint3 id:SV_DispatchThreadID) {
 uint width,height;output.GetDimensions(width,height);
 if(id.x>=width||id.y>=height)return;
 uint p=(id.y*width+id.x)*3;
 output[id.xy]=float4(rgb[p],rgb[p+1],rgb[p+2],1);
}
