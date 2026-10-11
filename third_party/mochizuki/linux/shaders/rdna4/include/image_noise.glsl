#ifndef NR_IMAGE_NOISE_GLSL
#define NR_IMAGE_NOISE_GLSL
// Same recovered PCG/Box-Muller stream as img_in.comp. Noise coordinates stay
// in the padded working domain; only texture coordinates reflect source edges.
float nr_g0,nr_g1,nr_g2;
uint nr_pcg(uint v) {
    v=(v>>((v>>28)+4u))^v;
    v*=0x108EF2D9u;
    return v;
}
float nr_u(uint stream) {
    const uint t=nr_pcg(stream);
    return float(((t>>30)^(t>>8))+1u)*5.9604644775390625e-08;
}
void nr_gauss3(uint x,uint y,uint seed) {
    const uint base=(x*0x8DA6B343u)^(seed*0x9E3779B9u)^(y*0xD8163841u)^0x243F6A88u;
    const uint t=nr_pcg(base),h=(t>>22)^t;
    const float uA=nr_u(h*0x2C9277B5u+0xAC564B05u);
    const float uB=nr_u(h*0xFA6DC5F9u+0x4712A88Eu);
    const float uC=nr_u(h*0xCAA5B80Du+0x21DD796Bu);
    const float uD=nr_u(h*0x83232C31u+0x3463E0ACu);
    const float rA=sqrt(-2.0*log(uA)),rC=sqrt(-2.0*log(uC));
    const float a1=uB*6.28318530718,a2=uD*6.28318530718;
    nr_g0=rA*cos(a1);nr_g1=rA*sin(a1);nr_g2=rC*cos(a2);
}
#endif
