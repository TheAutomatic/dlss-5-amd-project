#pragma once
#include "SysUtils.h"
#include <d3d12.h>

class GpuTime_Dx12
{
    static constexpr int QUERY_BUFFER_COUNT = 3;

    ID3D12QueryHeap* _queryHeap = nullptr;
    ID3D12Resource* _readbackBuffer = nullptr;
    std::array<bool, QUERY_BUFFER_COUNT> _trigger {};

    int _currentFrameIndex = 0;
    bool _init = false;
    bool _deferredRetirement = true;

    static constexpr size_t RAW_WINDOW_SIZE = 32;
    std::array<double, RAW_WINDOW_SIZE> _rawWindow {};
    size_t _rawCount = 0;
    size_t _rawHead = 0;
    double _baseMs = 0.0;
    double _cleanMs = 0.0;

  public:
    GpuTime_Dx12(ID3D12Device* device, bool deferredRetirement = true);
    ~GpuTime_Dx12();

    void Start(ID3D12GraphicsCommandList* cmdList);
    void End(ID3D12GraphicsCommandList* cmdList);

    std::optional<double> ReadGpuTime(ID3D12CommandQueue* commandQueue);
};

class ScopedGpuTime_Dx12
{
    GpuTime_Dx12* _gpuTime;
    ID3D12GraphicsCommandList* _cmdList;

  public:
    ScopedGpuTime_Dx12(GpuTime_Dx12* gpuTime, ID3D12GraphicsCommandList* cmdList) : _gpuTime(gpuTime), _cmdList(cmdList)
    {
        if (_gpuTime && _cmdList)
            _gpuTime->Start(_cmdList);
    }

    ~ScopedGpuTime_Dx12()
    {
        if (_gpuTime && _cmdList)
            _gpuTime->End(_cmdList);
    }
};
