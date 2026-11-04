#pragma once
#include <d3d12.h>
#include "Shader_Common.h"
#include <gpu_time/GpuTime_Dx12.h>
#include "RecordedShader_Dx12.h"
#include <functional>

class Shader_Dx12
{
  public:
    enum class Retirement { Deferred, Immediate };

  private:
    uint32_t _srcCount = 0;
    uint32_t _uavCount = 0;
    uint32_t _cbvCount = 0;
    uint32_t _rtvCount = 0;
    uint32_t _samplerCount = 0;

  protected:
    std::string _name = "";
    bool _init = false;
    int _counter = 0;
    Retirement _retirement;

    bool _recordedLifetime = false;
    GpuTime_Dx12* _activeTiming = nullptr;
    ID3D12GraphicsCommandList* _admissionFailedList = nullptr;
    std::vector<std::shared_ptr<RecordedShader::Dispatch>> _dispatchSlots;
    std::vector<std::shared_ptr<RecordedShader::Intermediate>> _recordedIntermediates;
    std::weak_ptr<RecordedShader::Intermediate> _recordedBuffer;
    std::shared_ptr<RecordedShader::Dispatch> AcquireDispatchLease(
        ID3D12GraphicsCommandList* list, ID3D12PipelineState* selectedPSO,
        std::initializer_list<ID3D12Resource*> resources);
    std::unique_ptr<GpuTime_Dx12> GpuTime = nullptr;

    ID3D12RootSignature* _rootSignature = nullptr;
    ID3D12PipelineState* _pipelineState = nullptr;

    ID3D12Device* _device = nullptr;
    ID3D12Resource* _constantBuffer = nullptr;

    std::vector<CD3DX12_DESCRIPTOR_RANGE1> _descriptorRanges;

    static DXGI_FORMAT TranslateTypelessFormats(DXGI_FORMAT format);
    static bool CreateComputeShader(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                    ID3D12PipelineState** pipelineState, ID3DBlob* shaderBlob,
                                    D3D12_SHADER_BYTECODE byteCode);
    bool CreateComputePipeline(ID3D12Device* device, ID3D12PipelineState** pipelineState, const void* bytecode,
                               size_t bytecodeSize, const char* source);
    bool CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InResource, D3D12_RESOURCE_STATES InState,
                              ID3D12Resource** OutResource, D3D12_RESOURCE_FLAGS ResourceFlags,
                              uint64_t InWidth = 0, uint32_t InHeight = 0,
                              DXGI_FORMAT InFormat = DXGI_FORMAT_UNKNOWN,
                              ID3D12GraphicsCommandList* InCommandList = nullptr);
    void SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState,
                        ID3D12Resource* Buffer, D3D12_RESOURCE_STATES* BufferState);

    void CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* tex, D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptor,
                                  DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN);

    void CreateUnorderedAccessView(ID3D12Device* device, ID3D12Resource* tex, D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptor,
                                   uint32_t mipLevel);

    void CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* tex, D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor,
                                uint32_t mipLevel);

    bool SetupRootSignature(ID3D12Device* InDevice, uint32_t srcCount, uint32_t uavCount, uint32_t cbvCount,
                            uint32_t rtvCount = 0, uint32_t samplerCount = 0, uint32_t staticSamplerCount = 0,
                            const D3D12_STATIC_SAMPLER_DESC* pStaticSamplers = nullptr,
                            D3D12_ROOT_SIGNATURE_FLAGS flags = D3D12_ROOT_SIGNATURE_FLAG_NONE);

    bool InitHeaps(ID3D12Device* InDevice, FrameDescriptorHeap* pHeaps, size_t numOFHeaps);

  public:
    // RR opts in; legacy Immediate/Deferred callers retain their existing behavior.
    void SetRecordedLifetimeEnabled(bool enabled) { _recordedLifetime = enabled; }
    static bool RecordedLifetimeSupported(ID3D12GraphicsCommandList* list) noexcept
    { return RecordedShader::Supported(list); }
    std::function<void()> RecordedBufferCleanup(ID3D12GraphicsCommandList* list);
    bool IsInit() const { return _init; }
    std::string Name() const { return _name; }
    std::optional<double> ReadGpuTime(ID3D12CommandQueue* commandQueue)
    {
        if (_recordedLifetime && _activeTiming)
            return _activeTiming->ReadGpuTime(commandQueue);
        return GpuTime->ReadGpuTime(commandQueue);
    }

    Shader_Dx12(std::string InName, ID3D12Device* InDevice, Retirement retirement = Retirement::Deferred);

    ~Shader_Dx12();
};
