#pragma once
#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <vector>
#include <cmath>
#include <utility>
namespace hip_reference {
// Lossless encoding only: reject weights requiring quantization or saturation.
inline uint8_t ExactWeightFp8(float value){
 uint32_t bits;std::memcpy(&bits,&value,4);uint32_t a=bits&0x7fffffffu;uint8_t sign=uint8_t((bits>>24)&128u);
 if(!a)return sign;
 if(a>=0x7f800000u)throw std::runtime_error("nonfinite FP8 matrix weight");
 float magnitude;std::memcpy(&magnitude,&a,4);
 if(a<0x3c800000u){float q=magnitude*512.f;if(q<1||q>7||q!=float(uint32_t(q)))throw std::runtime_error("matrix weight not exact FP8 subnormal");return uint8_t(sign|uint8_t(q));}
 int exponent=int(a>>23)-127+7;uint32_t mantissa=(a>>20)&7;
 if(exponent<1||exponent>15||(a&0xfffffu)||(exponent==15&&mantissa==7))throw std::runtime_error("matrix weight not exact finite FP8");
 return uint8_t(sign|(uint8_t(exponent)<<3)|mantissa);
}
// Lossless binary16 encoding; reject any weight that would require rounding.
inline uint16_t ExactWeightHalf(float v){uint32_t b;std::memcpy(&b,&v,4);uint32_t a=b&0x7fffffffu;uint16_t sign=uint16_t((b>>16)&0x8000u);if(!a)return sign;if(a>=0x7f800000u)throw std::runtime_error("nonfinite half weight");int e=int(a>>23)-127;if(e>15)throw std::runtime_error("half weight overflow");if(e>=-14){if(a&8191u)throw std::runtime_error("weight not exact half");return uint16_t(sign|((e+15)<<10)|((a>>13)&1023u));}float x=v<0?-v:v,q=x*16777216.f;if(q<1||q>1023||q!=float(uint32_t(q)))throw std::runtime_error("weight not exact half subnormal");return uint16_t(sign|uint16_t(q));}
inline void PackHalfMatrix(std::vector<float>&v,size_t count){if(count>v.size())throw std::runtime_error("half matrix shape");auto*bytes=reinterpret_cast<uint8_t*>(v.data());for(size_t i=0;i<count;i++){uint16_t h=ExactWeightHalf(v[i]);std::memcpy(bytes+i*2,&h,2);}}
// Round-to-nearest-even binary16 (with subnormals), matching the device (_Float16) cast of an arbitrary f32 weight.
inline uint16_t RoundWeightHalf(float x){uint32_t b;std::memcpy(&b,&x,4);uint32_t a=b&0x7fffffffu;uint16_t s=uint16_t((b>>16)&0x8000u);int e=int(a>>23)-127;
 if(a>=0x7f800000u)return uint16_t(s|0x7c00u|((a&0x7fffffu)?0x200u:0u));if(e<-25)return s;
 if(e<-14){uint32_t m=(a&0x7fffffu)|0x800000u;unsigned n=unsigned(-e-1);uint32_t q=m>>n,mask=(1u<<n)-1u,r=m&mask,mid=1u<<(n-1);q+=uint32_t(r>mid||(r==mid&&(q&1u)));return uint16_t(s|q);}
 if(e>15)return uint16_t(s|0x7c00u);uint32_t h=((a+0xfffu+((a>>13)&1u))>>13)-0x1c000u;return uint16_t(s|(h>=0x7c00u?0x7c00u:h));}
inline void PackHalfMatrixRounded(std::vector<float>&v,size_t begin,size_t count){if(begin+count>v.size())throw std::runtime_error("half matrix shape");auto*bytes=reinterpret_cast<uint8_t*>(v.data()+begin);std::vector<uint16_t>h(count);for(size_t i=0;i<count;i++)h[i]=RoundWeightHalf(v[begin+i]);std::memcpy(bytes,h.data(),count*2);}
// C32 chain residual diagonals (kernel macro HIP_C32_DIAG_WEIGHTS): the per-channel residual scale fw[8704+c] is split
// into three E4M3 pieces exactly as the device scale_piece() chain does, and each (part, column half, K half) B fragment
// tile is stored as 512 bytes in the A/B lane order the kernel loads with matrix8 (lane gr*16+rc, 8 bytes = k gr*8..+7).
inline float HostF(float x){uint32_t b;std::memcpy(&b,&x,4);uint32_t a=b&0x7fffffffu,sg=b&0x80000000u;if(!a)return 0.f;if(a>=0x43e00000u){uint32_t r=sg|0x43e00000u;float f;std::memcpy(&f,&r,4);return f;}
 if(a<0x3c800000u){float m;std::memcpy(&m,&a,4);float scaled=m*512.f;uint32_t q=uint32_t(scaled);float r=scaled-float(q);q+=uint32_t(r>.5f||(r==.5f&&(q&1u)));float f=float(q)/512.f;return sg?-f:f;}
 uint32_t r=(a+0x7ffffu+((a>>20)&1u))&0xfff00000u;if(r>0x43e00000u)r=0x43e00000u;r|=sg;float f;std::memcpy(&f,&r,4);return f;}
inline float HostScalePiece(float v){float m=std::fabs(v);if(m<.015625f){float x=m*512.f;uint32_t q=uint32_t(x);float r=x-float(q);q+=uint32_t(r>.5f||(r==.5f&&(q&1u)));if(q>7)q=7;float f=float(q)/512.f;uint32_t b,fb;std::memcpy(&b,&v,4);std::memcpy(&fb,&f,4);fb|=b&0x80000000u;std::memcpy(&f,&fb,4);return f;}return HostF(v);}
inline void AppendC32ResidualDiagonals(std::vector<float>&v){if(v.size()!=8736)throw std::runtime_error("C32 FFN weight shape for diagonals");v.resize(8736+1536,0.f);uint8_t*d=reinterpret_cast<uint8_t*>(v.data())+34944;
 for(unsigned part=0;part<3;part++)for(unsigned ci=0;ci<2;ci++)for(unsigned kt=0;kt<2;kt++){uint8_t*tile=d+((part*2+ci)*2+kt)*512;
  for(unsigned gr=0;gr<2;gr++)for(unsigned rc=0;rc<16;rc++){unsigned c=ci*16+rc;float remaining=v[8704+c],piece=0.f;for(unsigned j=0;j<=part;j++){piece=HostScalePiece(remaining);remaining-=piece;}
   for(unsigned e=0;e<8;e++){unsigned k=kt*16+gr*8+e;tile[(gr*16+rc)*8+e]=k==c?ExactWeightFp8(piece):uint8_t(0);}}}}
// MH attention residual scales (floats at 4cc+heads*4096+heads+col) as three diagonal fragment B tiles per 16-column tile:
// tile (part*(c/16)+ct) byte (gr*16+rc)*8+e holds piece[part] of column ct*16+rc at k=gr*8+e==rc, zero elsewhere.
inline void AppendMhResidualDiagonals(std::vector<float>&v,unsigned c){size_t cc=size_t(c)*c,heads=c/32;if(v.size()!=4*cc+heads*4096+heads+c)throw std::runtime_error("MH attention weight shape for diagonals");size_t base=v.size();v.resize(base+size_t(c)*24,0.f);uint8_t*d=reinterpret_cast<uint8_t*>(v.data())+base*4;
 for(unsigned part=0;part<3;part++)for(unsigned ct=0;ct<c/16;ct++){uint8_t*tile=d+(part*(c/16)+ct)*512;
  for(unsigned gr=0;gr<2;gr++)for(unsigned rc=0;rc<16;rc++){unsigned col=ct*16+rc;float remaining=v[4*cc+heads*4096+heads+col],piece=0.f;for(unsigned j=0;j<=part;j++){piece=HostScalePiece(remaining);remaining-=piece;}
   for(unsigned e=0;e<8;e++){unsigned k=gr*8+e;tile[(gr*16+rc)*8+e]=k==rc?ExactWeightFp8(piece):uint8_t(0);}}}}
// Preserve float-region offsets for scales/bias and separate matrix regions.
// Each matrix becomes row-major FP8 at its original starting byte address.
inline void PackWeightRegions(std::vector<float>&values,const std::vector<std::pair<size_t,size_t>>&regions){
 size_t end=0;for(auto r:regions){if(r.first<end||r.first>values.size()||r.second>values.size()-r.first)throw std::runtime_error("invalid packed matrix region");end=r.first+r.second;}
 auto*bytes=reinterpret_cast<uint8_t*>(values.data());
 for(auto r:regions)for(size_t i=0;i<r.second;i++){
  // Forward packing only overwrites coefficients that have already been read.
  uint8_t encoded=ExactWeightFp8(values[r.first+i]);bytes[r.first*4+i]=encoded;
 }
}
// Exact structural precondition for the grouped MH contraction fast path.
inline void ValidateGroupedMhContract(const std::vector<float>&v,unsigned c){
 if((c!=64&&c!=128&&c!=256)||v.size()<size_t(8)*c*c)throw std::runtime_error("grouped contract shape");
 for(unsigned row=0;row<c;row++)for(unsigned k=0;k<4*c;k++)if(k/128!=row/32&&v[size_t(4)*c*c+size_t(row)*4*c+k]!=0.f)throw std::runtime_error("nonzero outside grouped contraction");
}
// Reorder packed row-major B into [N/16][K/32][K32][N16], preserving bytes.
// Fragment-native tiles (vit_expand_blocked_fp8_frag_*): same 512-byte (16 rows x 32 k) tiles as TilePackedMatrix, but
// inside a tile the bytes are ordered [k/16 within tile][lane half gr][row%16][8 consecutive k], so the WMMA B operand of
// lane (row%16, gr) at K16 step j is one 8-byte load at ((j*2+gr)*16+row%16)*8. Pure permutation of the same bytes.
inline void FragmentPackedMatrix(std::vector<float>&v,size_t start,size_t rows,size_t columns){
 if(rows%16||columns%32||start>v.size()||rows*columns>(v.size()-start)*4)throw std::runtime_error("packed fragment shape");
 auto*dst=reinterpret_cast<uint8_t*>(v.data()+start);std::vector<uint8_t>src(dst,dst+rows*columns);
 for(size_t n=0;n<rows;n++)for(size_t k=0;k<columns;k++)dst[((n/16)*(columns/32)+k/32)*512+(((k%32)/16*2+(k%16)/8)*16+n%16)*8+k%8]=src[n*columns+k];
}
inline void TilePackedMatrix(std::vector<float>&v,size_t start,size_t rows,size_t columns){
 if(rows%16||columns%32||start>v.size()||rows*columns>(v.size()-start)*4)throw std::runtime_error("packed tile shape");
 auto*dst=reinterpret_cast<uint8_t*>(v.data()+start);std::vector<uint8_t>src(dst,dst+rows*columns);
 for(size_t n=0;n<rows;n++)for(size_t k=0;k<columns;k++)dst[((n/16)*(columns/32)+k/32)*512+(k%32)*16+n%16]=src[n*columns+k];
}

}
