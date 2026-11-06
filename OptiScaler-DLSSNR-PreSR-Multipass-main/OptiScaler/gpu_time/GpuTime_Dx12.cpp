#include "pch.h"
#include "GpuTime_Dx12.h"

#include <State.h>
#include <misc/DeferredRelease.h>

#include <include/d3dx/d3dx12.h>

GpuTime_Dx12::GpuTime_Dx12(ID3D12Device* device, bool deferredRetirement)
    : _deferredRetirement(deferredRetirement)
{
    if (device == nullptr)
    {
        LOG_ERROR("GpuTime_Dx12 created with nullptr device");
        return;
    }

    // Create query heap for Start and End timestamps per buffer
    D3D12_QUERY_HEAP_DESC queryHeapDesc = {};
    queryHeapDesc.Count = QUERY_BUFFER_COUNT * 2;
    queryHeapDesc.NodeMask = 0;
    queryHeapDesc.Type = D3D12_QUERY_HEAP_TYPE_TIMESTAMP;

    auto result = device->CreateQueryHeap(&queryHeapDesc, IID_PPV_ARGS(&_queryHeap));

    if (result != S_OK)
    {
        LOG_ERROR("CreateQueryHeap error: {:X}", (UINT) result);
        return;
    }

    // Create a readback buffer large enough for all frames
    D3D12_RESOURCE_DESC bufferDesc = CD3DX12_RESOURCE_DESC::Buffer(QUERY_BUFFER_COUNT * 2 * sizeof(UINT64));
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_READBACK;

    result = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                             D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&_readbackBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("CreateCommittedResource error: {:X}", (UINT) result);
        return;
    }

    _init = true;
}

GpuTime_Dx12::~GpuTime_Dx12()
{
    if (_deferredRetirement)
    {
        DeferredRelease::Release(_queryHeap);
        DeferredRelease::Release(_readbackBuffer);
    }
    else
    {
        SAFE_RELEASE(_queryHeap);
        SAFE_RELEASE(_readbackBuffer);
    }
}

void GpuTime_Dx12::Start(ID3D12GraphicsCommandList* cmdList)
{
    if (_init && _queryHeap != nullptr)
    {
        _currentFrameIndex = (_currentFrameIndex + 1) % QUERY_BUFFER_COUNT;

        cmdList->EndQuery(_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, _currentFrameIndex * 2);
    }
}

void GpuTime_Dx12::End(ID3D12GraphicsCommandList* cmdList)
{
    if (_init && _queryHeap != nullptr)
    {
        cmdList->EndQuery(_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, _currentFrameIndex * 2 + 1);

        cmdList->ResolveQueryData(_queryHeap, D3D12_QUERY_TYPE_TIMESTAMP, _currentFrameIndex * 2, 2, _readbackBuffer,
                                  _currentFrameIndex * 2 * sizeof(UINT64));

        _trigger[_currentFrameIndex] = true;
    }
}

std::optional<double> GpuTime_Dx12::ReadGpuTime(ID3D12CommandQueue* commandQueue)
{
    std::optional<double> elapsedTimeMs = std::nullopt;

    if (!_init || _queryHeap == nullptr || _readbackBuffer == nullptr)
        return elapsedTimeMs;

    // Try to read the previous frame's timestamps
    uint32_t previousFrameIndex = (_currentFrameIndex + 1) % QUERY_BUFFER_COUNT;

    if (!_trigger[previousFrameIndex])
        return elapsedTimeMs;

    UINT64* timestampData {};

    // Tell it which timestamps we will be reading
    D3D12_RANGE readRange = { previousFrameIndex * 2 * sizeof(UINT64), (previousFrameIndex * 2 + 2) * sizeof(UINT64) };
    _readbackBuffer->Map(0, &readRange, reinterpret_cast<void**>(&timestampData));

    // CPU doesn't write anything
    D3D12_RANGE writeRange = { 0, 0 };

    if (timestampData != nullptr)
    {
        // Get the GPU timestamp frequency (ticks per second)
        UINT64 gpuFrequency;
        commandQueue->GetTimestampFrequency(&gpuFrequency);

        // Calculate elapsed time in milliseconds
        UINT64 startTime = timestampData[previousFrameIndex * 2];
        UINT64 endTime = timestampData[previousFrameIndex * 2 + 1];

        if (endTime < startTime)
        {
            _readbackBuffer->Unmap(0, &writeRange);
            return elapsedTimeMs;
        }

        double rawMs = (endTime - startTime) / static_cast<double>(gpuFrequency) * 1000.0;

        // Filter out wall-clock timestamp inflation when a concurrent async-compute queue preempts between Start and End.
        if (rawMs > 0.0 && rawMs < 100.0)
        {
            if (_rawCount >= 2)
            {
                const double prevMs = _rawWindow[(_rawHead + RAW_WINDOW_SIZE - 1) % RAW_WINDOW_SIZE];
                double minWin = _rawWindow[0];
                for (size_t i = 1; i < _rawCount; ++i)
                    minWin = (std::min)(minWin, _rawWindow[i]);

                if ((prevMs > minWin * 2.0 && rawMs > minWin * 2.0) || rawMs < minWin * 0.5)
                {
                    _rawWindow[0] = prevMs;
                    _rawCount = 1;
                    _rawHead = 1;
                }
            }

            _rawWindow[_rawHead] = rawMs;
            _rawHead = (_rawHead + 1) % RAW_WINDOW_SIZE;
            if (_rawCount < RAW_WINDOW_SIZE)
                ++_rawCount;

            if (_rawCount >= 3)
            {
                std::array<double, RAW_WINDOW_SIZE> sorted {};
                for (size_t i = 0; i < _rawCount; ++i)
                    sorted[i] = _rawWindow[i];
                std::sort(sorted.begin(), sorted.begin() + _rawCount);

                const double base = sorted[_rawCount / 10];
                const double limit = base * 1.08;
                if (rawMs > limit)
                {
                    double sum = 0.0;
                    size_t cnt = 0;
                    for (size_t i = 0; i < _rawCount; ++i)
                    {
                        if (sorted[i] <= limit)
                        {
                            sum += sorted[i];
                            ++cnt;
                        }
                    }
                    rawMs = cnt > 0 ? (sum / static_cast<double>(cnt)) : base;
                }
            }
        }

        elapsedTimeMs = rawMs;
    }
    else
    {
        LOG_WARN("timestampData is null!");
    }

    _readbackBuffer->Unmap(0, &writeRange);
    _trigger[previousFrameIndex] = false;

    return elapsedTimeMs;
}
