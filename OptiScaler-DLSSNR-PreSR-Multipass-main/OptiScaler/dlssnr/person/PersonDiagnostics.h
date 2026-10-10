#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <sstream>
#include <string>

namespace DlssNr::Person
{
// Only enabled with the existing mask overlay. All access is serialized by
// RecordingMutex; diagnostics never wait for the GPU or retain an extra frame.
enum class MaskDecision { Accepted, Missing, Epoch, Size, Expired, FrameAge, Guides, Count };
enum class ResetReason { External, Guides, Resize, Device, Replay, Epoch, Order, Delayed,
                         Continuation, Error, Count };
struct DiagnosticMetric
{
    uint64_t count=0, sum=0, minimum=0, maximum=0;
    void Add(uint64_t value) { minimum=count?(std::min)(minimum,value):value;++count;sum+=value;maximum=(std::max)(maximum,value); }
    uint64_t Mean() const { return count?sum/count:0; }
};
struct PersonDiagnostics
{
    bool enabled=false;
    uint64_t generation=0, start=0, lastSample=0, rawEpoch=0, rawFrame=0;
    std::array<uint64_t,size_t(MaskDecision::Count)> masks{};
    std::array<uint64_t,size_t(ResetReason::Count)> resets{};
    uint64_t executed=0, executionExpired=0, captureSubmitted=0, captureDiscarded=0;
    DiagnosticMetric maskAge, arrivalAge, submitDelay, captureAge, rawCoverage, warpedCoverage, warpValid;
    void ClearWindow(uint64_t now)
    {
        start=now;masks={};resets={};executed=executionExpired=captureSubmitted=captureDiscarded=0;
        maskAge={};arrivalAge={};submitDelay={};captureAge={};rawCoverage={};warpedCoverage={};warpValid={};
    }
    void Enable(bool value,uint64_t now)
    {
        if(value==enabled)return;
        enabled=value;++generation;lastSample=0;rawEpoch=rawFrame=0;ClearWindow(now);
    }
    void Reset(ResetReason reason) { if(enabled)++resets[size_t(reason)]; }
    void Mask(MaskDecision reason) { if(enabled)++masks[size_t(reason)]; }
    bool Sample(uint64_t now)
    {
        if(!enabled||(lastSample&&now-lastSample<100))return false;
        lastSample=now;return true;
    }
    std::string Report(uint64_t now)
    {
        if(!enabled||now-start<2000)return {};
        std::ostringstream out;
        out<<"Person mask diagnostic ("<<now-start<<" ms): record[ok/missing/epoch/size/expired/frame/guides]=";
        for(size_t i=0;i<masks.size();++i)out<<(i?"/":"")<<masks[i];
        out<<" reset[external/guides/resize/device/replay/epoch/order/delayed/continuation/error]=";
        for(size_t i=0;i<resets.size();++i)out<<(i?"/":"")<<resets[i];
        out<<" execute="<<executed<<" execute_expired="<<executionExpired
           <<" capture[sent/discarded]="<<captureSubmitted<<"/"<<captureDiscarded;
        auto metric=[&](const char* name,const DiagnosticMetric& v){
            out<<" "<<name<<"[n/min/mean/max]="<<v.count<<"/"<<v.minimum<<"/"<<v.Mean()<<"/"<<v.maximum;
        };
        metric("mask_age_ms",maskAge);metric("arrival_age_ms",arrivalAge);
        metric("record_to_submit_ms",submitDelay);metric("capture_to_cpu_ms",captureAge);
        // Coverage is the fraction of the 160x160 grid with mask > 0.5, in
        // ten-thousandths (10000 = the entire image), not person confidence.
        metric("raw_coverage_1e4",rawCoverage);metric("warped_coverage_1e4",warpedCoverage);
        metric("warp_valid_1e4",warpValid);
        ClearWindow(now);return out.str();
    }
};
}
