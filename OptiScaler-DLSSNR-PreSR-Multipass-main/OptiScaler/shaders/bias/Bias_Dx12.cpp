#include "pch.h"
#include "Bias_Dx12.h"

#include "Bias_Common.h"
#include "precompile/Bias_Shader.h"

#include <Config.h>

bool Bias_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InSource, D3D12_RESOURCE_STATES InState,
                                    ID3D12GraphicsCommandList* InCommandList)
{
    auto resourceFlags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS |
                         D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS;

    auto result = Shader_Dx12::CreateBufferResource(InDevice, InSource, InState, &_buffer, resourceFlags,
                                                   0, 0, DXGI_FORMAT_UNKNOWN, InCommandList);

    if (result)
    {
        _buffer->SetName(L"Bias_Buffer");
        _bufferState = _recordedLifetime ? D3D12_RESOURCE_STATE_COMMON : InState;
    }

    return result;
}

void Bias_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState)
{
    return Shader_Dx12::SetBufferState(InCommandList, InState, _buffer, &_bufferState);
}

bool Bias_Dx12::Dispatch(ID3D12GraphicsCommandList* InCmdList, ID3D12Resource* InResource, float InBias,
                         ID3D12Resource* OutResource)
{
    if (!_init || _device == nullptr || InCmdList == nullptr || InResource == nullptr || OutResource == nullptr)
        return false;

    LOG_DEBUG("[{0}] Start!", _name);

    std::unique_lock<std::recursive_mutex> recordingLock(DlssNr::Submission::RecordingMutex(), std::defer_lock);
    if (_recordedLifetime) recordingLock.lock();
    auto lease = AcquireDispatchLease(InCmdList, _pipelineState, {InResource, OutResource});
    if (_recordedLifetime && !lease) return false;
    _activeTiming = lease ? lease->timing.get() : GpuTime.get();
    ScopedGpuTime_Dx12 scopedGpuTime(_activeTiming, InCmdList);
    _counter = (_counter + 1) % BIAS_NUM_OF_HEAPS;
    FrameDescriptorHeap& currentHeap = lease ? lease->heap : _frameHeaps[_counter];
    auto* constantBuffer = lease ? lease->constants.Get() : _constantBuffer;

    CreateShaderResourceView(_device, InResource, currentHeap.GetSrvCPU(0));
    CreateUnorderedAccessView(_device, OutResource, currentHeap.GetUavCPU(0), 0);

    InternalConstants constants {};
    constants.Bias = std::clamp(InBias, 0.0f, 0.9f);

    if (!CreateConstantsBuffer(_device, constantBuffer, constants, currentHeap.GetCbvCPU(0)))
    {
        LOG_ERROR("[{0}] Failed to create a constants buffer", _name);
        return false;
    }

    ID3D12DescriptorHeap* heaps[] = { currentHeap.GetHeapCSU() };
    InCmdList->SetDescriptorHeaps(_countof(heaps), heaps);

    InCmdList->SetComputeRootSignature(_rootSignature);
    InCmdList->SetPipelineState(_pipelineState);

    InCmdList->SetComputeRootDescriptorTable(0, currentHeap.GetTableGPUStart());

    UINT dispatchWidth = 0;
    UINT dispatchHeight = 0;

    auto inDesc = InResource->GetDesc();
    dispatchWidth = static_cast<UINT>((inDesc.Width + InNumThreadsX - 1) / InNumThreadsX);
    dispatchHeight = (inDesc.Height + InNumThreadsY - 1) / InNumThreadsY;

    InCmdList->Dispatch(dispatchWidth, dispatchHeight, 1);

    return true;
}

Bias_Dx12::Bias_Dx12(std::string InName, ID3D12Device* InDevice) : Shader_Dx12(InName, InDevice)
{
    if (InDevice == nullptr)
    {
        LOG_ERROR("InDevice is nullptr!");
        return;
    }

    LOG_DEBUG("{0} start!", _name);

    if (!SetupRootSignature(InDevice, 1, 1, 1))
    {
        LOG_ERROR("Failed to setup root signature");
        return;
    }

    D3D12_RESOURCE_DESC desc = CD3DX12_RESOURCE_DESC::Buffer(sizeof(InternalConstants));
    auto heapProps = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);

    auto result =
        InDevice->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_GENERIC_READ,
                                          nullptr, IID_PPV_ARGS(&_constantBuffer));

    if (result != S_OK)
    {
        LOG_ERROR("[{0}] CreateCommittedResource error {1:x}", _name, (unsigned int) result);
        return;
    }

    if (!CreateComputePipeline(InDevice, &_pipelineState, bias_cso, sizeof(bias_cso), biasShader.c_str()))
    {
        LOG_ERROR("[{0}] Failed to create compute pipeline", _name);
        return;
    }

    _init = InitHeaps(InDevice, _frameHeaps, BIAS_NUM_OF_HEAPS);
}

Bias_Dx12::~Bias_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    for (int i = 0; i < BIAS_NUM_OF_HEAPS; i++)
    {
        _frameHeaps[i].ReleaseHeaps();
    }

    DeferredRelease::Release(_buffer);
}
