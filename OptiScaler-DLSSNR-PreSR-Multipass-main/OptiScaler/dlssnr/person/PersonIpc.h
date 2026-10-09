#pragma once
#include <cstdint>
#include <string>

namespace DlssNr::Person::Ipc
{
constexpr unsigned ModelWidth = 640;
constexpr unsigned ModelHeight = 640;
constexpr unsigned ModelChannels = 3;
constexpr unsigned MaskWidth = 160;
constexpr unsigned MaskHeight = 160;

// Input float RGB buffer: 1 * 3 * 640 * 640 * sizeof(float) = 4,915,200 bytes
constexpr size_t InputRgbSize = ModelChannels * ModelWidth * ModelHeight * sizeof(float);
// Output float Mask buffer: 160 * 160 * sizeof(float) = 102,400 bytes
constexpr size_t OutputMaskSize = MaskWidth * MaskHeight * sizeof(float);

constexpr uint32_t ShmMagic = 0x50455253; // 'PERS'
constexpr uint32_t ShmVersion = 2;

enum class WorkerState : uint32_t
{
    Uninitialized = 0,
    Ready = 1,
    Processing = 2,
    Error = 3,
    Terminating = 4
};

#pragma pack(push, 8)
struct ShmHeader
{
    uint32_t magic;           // ShmMagic
    uint32_t version;         // ShmVersion
    uint32_t state;           // WorkerState
    uint32_t flags;           // Worker acknowledges ShmVersion before first response.

    // Parent -> Worker request
    uint64_t reqEpoch;
    uint64_t reqFrame;
    uint64_t reqTick;
    uint32_t reqWidth;
    uint32_t reqHeight;
    uint32_t reqRgbOffset;    // Byte offset in SHM
    uint32_t reqRgbBytes;

    // Worker -> Parent response
    uint64_t respEpoch;
    uint64_t respFrame;
    uint64_t respTick;
    uint32_t respWidth;
    uint32_t respHeight;
    uint32_t respMaskOffset;  // Byte offset in SHM
    uint32_t respMaskBytes;
    double   computeMilliseconds;

    char statusMessage[256];
};
#pragma pack(pop)

constexpr size_t HeaderOffset = 0;
constexpr size_t InputRgbOffset = 4096;
constexpr size_t OutputMaskOffset = 4096 + InputRgbSize; // 4,919,296
constexpr size_t TotalShmSize = OutputMaskOffset + OutputMaskSize; // 5,021,696 bytes
static_assert(sizeof(ShmHeader) <= InputRgbOffset);
inline bool ValidHeader(const ShmHeader& h)
{
    return h.magic==ShmMagic && h.version==ShmVersion &&
        h.reqRgbOffset==InputRgbOffset && h.reqRgbBytes==InputRgbSize &&
        h.respMaskOffset==OutputMaskOffset && h.respMaskBytes==OutputMaskSize;
}

inline std::wstring ShmName(uint32_t pid)
{
    return L"Local\\OptScaler_Person_SHM_" + std::to_wstring(pid);
}

inline std::wstring ReqEventName(uint32_t pid)
{
    return L"Local\\OptScaler_Person_REQ_" + std::to_wstring(pid);
}

inline std::wstring RespEventName(uint32_t pid)
{
    return L"Local\\OptScaler_Person_RESP_" + std::to_wstring(pid);
}

inline std::wstring StopEventName(uint32_t pid)
{
    return L"Local\\OptScaler_Person_STOP_" + std::to_wstring(pid);
}
}
