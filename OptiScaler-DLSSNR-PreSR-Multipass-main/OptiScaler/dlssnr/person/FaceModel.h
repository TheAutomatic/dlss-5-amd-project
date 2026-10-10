#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <stdexcept>
#include <vector>

namespace DlssNr::Person::Face
{
inline constexpr unsigned InputSize = 320, OutputSize = 160;
inline unsigned BoundedSize(unsigned size) { return size==384||size==416?size:320; }
// YuNet FP32: BGR [0,255], from the same centered 640-square RGB capture as PP/YOLO.
// Keep letterboxing: stretching faces changes their geometry and the decoded mask grid.
inline void Prepare(const float* rgb, std::span<float> input, unsigned size = InputSize)
{
    if(size!=BoundedSize(size)) throw std::runtime_error("unsupported face input size");
    constexpr unsigned captureSize = 640, capturePixels = captureSize * captureSize;
    if (!rgb || input.size() != 3 * size * size) throw std::runtime_error("invalid face input buffer");
    for (unsigned c = 0; c < 3; ++c) {
        const float* source = rgb + (2 - c) * capturePixels;
        float* target = input.data() + c * size * size;
        for (unsigned y = 0; y < size; ++y) for (unsigned x = 0; x < size; ++x) {
            if(size!=320) {
                const float px=(x+.5f)*640/size-.5f,py=(y+.5f)*640/size-.5f;
                const int x0=std::clamp(int(std::floor(px)),0,639),y0=std::clamp(int(std::floor(py)),0,639);
                const int x1=(std::min)(639,x0+1),y1=(std::min)(639,y0+1);
                const float fx=px-std::floor(px),fy=py-std::floor(py);
                const float value=((source[y0*640+x0]*(1-fx)+source[y0*640+x1]*fx)*(1-fy)+(source[y1*640+x0]*(1-fx)+source[y1*640+x1]*fx)*fy)*255.f;
                target[y*size+x]=std::isfinite(value)?std::clamp(value,0.f,255.f):0.f;
                continue;
            }
            const unsigned at = (2 * y) * captureSize + 2 * x;
            float value = (source[at] + source[at + 1] + source[at + captureSize] + source[at + captureSize + 1]) * 63.75f;
            target[y * size + x] = std::isfinite(value) ? std::clamp(value, 0.f, 255.f) : 0.f;
        }
    }
}
struct Head { std::span<const float> score, objectness, box; };
struct Box { float x0, y0, x1, y1, score; };
inline float IoU(const Box& a, const Box& b)
{
    const float area = (std::max)(0.f, (std::min)(a.x1,b.x1)-(std::max)(a.x0,b.x0)) *
                       (std::max)(0.f, (std::min)(a.y1,b.y1)-(std::max)(a.y0,b.y0));
    return area / (std::max)(1e-6f, (a.x1-a.x0)*(a.y1-a.y0)+(b.x1-b.x0)*(b.y1-b.y0)-area);
}
inline std::vector<float> Decode(const std::array<Head, 3>& heads, unsigned size = InputSize)
{
    if(size!=BoundedSize(size)) throw std::runtime_error("unsupported face input size");
    std::vector<Box> candidates;
    // Supported input sizes bound this to at most 3549 anchors, then 256 candidates / 16 faces.
    for (unsigned level = 0; level < heads.size(); ++level) {
        const unsigned stride = 8u << level, side = size / stride, count = side * side;
        const auto& h = heads[level];
        if (h.score.size()!=count || h.objectness.size()!=count || h.box.size()!=count*4)
            throw std::runtime_error("unsupported face output dimensions");
        for (unsigned i = 0; i < count; ++i) {
            const float cls = h.score[i], obj = h.objectness[i];
            if (!std::isfinite(cls) || !std::isfinite(obj) || cls<0 || cls>1 || obj<0 || obj>1) continue;
            const float score = std::sqrt(cls * obj);
            if (score < .6f) continue;
            const float* b = h.box.data() + 4*i;
            if (!std::all_of(b,b+4,[](float v){return std::isfinite(v) && std::abs(v)<=10.f;})) continue;
            const float w = std::exp(b[2])*stride, height = std::exp(b[3])*stride;
            if (w<1 || height<1 || w>size*2 || height>size*2) continue;
            const float cx = (float(i%side)+b[0])*stride, cy = (float(i/side)+b[1])*stride;
            if (cx<0 || cy<0 || cx>size || cy>size) continue;
            candidates.push_back({cx-w*.5f,cy-height*.5f,cx+w*.5f,cy+height*.5f,score});
        }
    }
    const auto keep = (std::min)(size_t(256), candidates.size());
    std::partial_sort(candidates.begin(),candidates.begin()+keep,candidates.end(),
                      [](const Box& a,const Box& b){return a.score>b.score;});
    candidates.resize(keep);
    std::vector<Box> selected;
    for (const auto& b : candidates) {
        if (std::any_of(selected.begin(),selected.end(),[&](const Box& a){return IoU(a,b)>.3f;})) continue;
        selected.push_back(b);
        if (selected.size()==16) break;
    }
    std::vector<float> mask(OutputSize*OutputSize,0.f);
    for (const auto& b : selected) {
        const float cx=(b.x0+b.x1)*.5f,cy=(b.y0+b.y1)*.5f,rx=(b.x1-b.x0)*.5f,ry=(b.y1-b.y0)*.5f;
        const int x0=std::clamp(int(std::floor(b.x0*OutputSize/size)),0,int(OutputSize)),x1=std::clamp(int(std::ceil(b.x1*OutputSize/size)),0,int(OutputSize));
        const int y0=std::clamp(int(std::floor(b.y0*OutputSize/size)),0,int(OutputSize)),y1=std::clamp(int(std::ceil(b.y1*OutputSize/size)),0,int(OutputSize));
        // A soft ellipse inside the detected face box, not semantic skin/hair segmentation.
        // Confidence selects detections only; it must not modulate the whole face every frame.
        for (int y=y0;y<y1;++y) for (int x=x0;x<x1;++x) {
            const float dx=((x+.5f)*size/OutputSize-cx)/rx,dy=((y+.5f)*size/OutputSize-cy)/ry;
            const float t=std::clamp((1-std::sqrt(dx*dx+dy*dy))*4,0.f,1.f);
            auto& value=mask[y*OutputSize+x];value=(std::max)(value,t*t*(3-2*t));
        }
    }
    return mask;
}
}
