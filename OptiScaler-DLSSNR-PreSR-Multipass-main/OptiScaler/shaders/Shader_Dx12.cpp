#include "pch.h"
#include "Shader_Dx12.h"
#include <d3dx/d3dx12.h>
#include <misc/DeferredRelease.h>

using Microsoft::WRL::ComPtr;

Shader_Dx12::Shader_Dx12(std::string InName, ID3D12Device* InDevice, Retirement retirement)
    : _name(InName), _retirement(retirement), _device(InDevice)
{
    GpuTime = std::make_unique<GpuTime_Dx12>(InDevice, retirement == Retirement::Deferred);
}

Shader_Dx12::~Shader_Dx12()
{
    if (!_init || State::Instance().isShuttingDown)
        return;

    if (_retirement == Retirement::Deferred)
    {
        DeferredRelease::Release(_pipelineState);
        DeferredRelease::Release(_rootSignature);
        DeferredRelease::Release(_constantBuffer);
    }
    else
    {
        SAFE_RELEASE(_pipelineState);
        SAFE_RELEASE(_rootSignature);
        SAFE_RELEASE(_constantBuffer);
    }
}

DXGI_FORMAT Shader_Dx12::TranslateTypelessFormats(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        return DXGI_FORMAT_R32G32B32A32_FLOAT;

    case DXGI_FORMAT_R32G32B32_TYPELESS:
        return DXGI_FORMAT_R32G32B32_FLOAT;

    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;

    case DXGI_FORMAT_R10G10B10A2_TYPELESS:
        return DXGI_FORMAT_R10G10B10A2_UINT;

    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;

    case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        return DXGI_FORMAT_B8G8R8A8_UNORM;

    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;

    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;

    // Some shaders didn't have those conversions and I'm not 100% sure if it's fine to do for them
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;

    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;

    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_FLOAT;

    case DXGI_FORMAT_D16_UNORM:
        return DXGI_FORMAT_R16_UNORM;

    default:
        return format;
    }
}

bool Shader_Dx12::CreateComputeShader(ID3D12Device* device, ID3D12RootSignature* rootSignature,
                                      ID3D12PipelineState** pipelineState, ID3DBlob* shaderBlob,
                                      D3D12_SHADER_BYTECODE byteCode)
{
    D3D12_COMPUTE_PIPELINE_STATE_DESC psoDesc = {};
    psoDesc.pRootSignature = rootSignature;
    psoDesc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;

    if (shaderBlob != nullptr)
        psoDesc.CS = CD3DX12_SHADER_BYTECODE(shaderBlob->GetBufferPointer(), shaderBlob->GetBufferSize());
    else
        psoDesc.CS = byteCode;

    HRESULT hr = device->CreateComputePipelineState(&psoDesc, __uuidof(ID3D12PipelineState*), (void**) pipelineState);

    if (FAILED(hr))
    {
        LOG_ERROR("CreateComputePipelineState error {0:x}", hr);
        return false;
    }

    return true;
}

bool Shader_Dx12::CreateComputePipeline(ID3D12Device* device, ID3D12PipelineState** pipelineState, const void* bytecode,
                                        size_t bytecodeSize, const char* source)
{
    ComPtr<ID3DBlob> shaderBlob;

    // Compile if not using precompiled
    if (!Config::Instance()->UsePrecompiledShaders.value_or_default() && source)
        shaderBlob = CompileShader(source, "CSMain", "cs_5_0");

    return CreateComputeShader(device, _rootSignature, pipelineState, shaderBlob.Get(),
                               CD3DX12_SHADER_BYTECODE(bytecode, bytecodeSize));
}

bool Shader_Dx12::CreateBufferResource(ID3D12Device* InDevice, ID3D12Resource* InResource,
                                       D3D12_RESOURCE_STATES InState, ID3D12Resource** OutResource,
                                       D3D12_RESOURCE_FLAGS ResourceFlags, uint64_t InWidth, uint32_t InHeight,
                                       DXGI_FORMAT InFormat, ID3D12GraphicsCommandList* InCommandList)
{
    if (InDevice == nullptr || InResource == nullptr)
        return false;

    auto inDesc = InResource->GetDesc();

    if (InWidth != 0 && InHeight != 0)
    {
        inDesc.Width = InWidth;
        inDesc.Height = InHeight;
    }

    if (InFormat != DXGI_FORMAT_UNKNOWN)
        inDesc.Format = InFormat;

    if (_recordedLifetime)
    {
        std::lock_guard lock(DlssNr::Submission::RecordingMutex());
        _admissionFailedList = nullptr;
        if (!RecordedLifetimeSupported(InCommandList) ||
            !(ResourceFlags & D3D12_RESOURCE_FLAG_ALLOW_SIMULTANEOUS_ACCESS)) return false;
        try
        {
            // Drop references from retired dispatches before selecting an intermediate.
            for (auto& slot : _dispatchSlots)
                if (slot->Available()) { slot->resources.clear(); slot->intermediates.clear(); }
            std::shared_ptr<RecordedShader::Intermediate> owner;
            for (auto& candidate : _recordedIntermediates)
                if (candidate.use_count() == 1 && candidate->Available())
                {
                    const auto desc = candidate->resource->GetDesc();
                    if (desc.Width == inDesc.Width && desc.Height == inDesc.Height &&
                        desc.Format == inDesc.Format) { owner = candidate; break; }
                }
            if (!owner)
            {
                owner = std::make_shared<RecordedShader::Intermediate>();
                owner->device = InDevice;
                inDesc.Flags = ResourceFlags;
                inDesc.MipLevels = 1;
                inDesc.Alignment = 0;
                inDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
                const auto props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);
                if (FAILED(InDevice->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &inDesc,
                    D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&owner->resource)))) return false;
                ComPtr<RecordedShader::IntermediateLink> link;
                link.Attach(new RecordedShader::IntermediateLink(owner));
                if (FAILED(owner->resource->SetPrivateDataInterface(RecordedShader::IntermediateKey, link.Get())))
                    return false;
                _recordedIntermediates.push_back(owner);
            }
            if (!owner->Attach(InCommandList)) return false;
            owner->state = D3D12_RESOURCE_STATE_COMMON;
            _recordedBuffer = owner;
            owner->resource->AddRef(); // Existing raw Buffer()/destructor ownership.
            if (*OutResource) (*OutResource)->Release();
            *OutResource = owner->resource.Get();
            // Resizes do not keep dead allocations in the reusable pool.
            _recordedIntermediates.erase(std::remove_if(_recordedIntermediates.begin(),
                _recordedIntermediates.end(), [&](const auto& item) {
                    return item != owner && item.use_count() == 1 && item->Available();
                }), _recordedIntermediates.end());
            return true;
        }
        catch (...) { return false; }
    }
    if (*OutResource != nullptr)
    {
        auto bufDesc = (*OutResource)->GetDesc();

        if (bufDesc.Width != inDesc.Width || bufDesc.Height != inDesc.Height || bufDesc.Format != inDesc.Format)
        {
            DeferredRelease::Release(*OutResource);
            LOG_WARN("Release {}x{}, new one: {}x{}", bufDesc.Width, bufDesc.Height, inDesc.Width, inDesc.Height);
        }
        else
        {
            return true;
        }
    }

    D3D12_HEAP_PROPERTIES heapProperties;
    D3D12_HEAP_FLAGS heapFlags;
    HRESULT hr = InResource->GetHeapProperties(&heapProperties, &heapFlags);

    // Reserved resources have no heap
    if (hr != S_OK)
        heapProperties = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_DEFAULT);

    // Only what the shader needs, source flags like ALLOW_DEPTH_STENCIL or DENY_SHADER_RESOURCE
    // are invalid together with UAV and inherited mips would only waste memory
    inDesc.Flags = ResourceFlags;
    inDesc.MipLevels = 1;
    inDesc.Alignment = 0;
    inDesc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;

    hr = InDevice->CreateCommittedResource(&heapProperties, D3D12_HEAP_FLAG_NONE, &inDesc, InState, nullptr,
                                           IID_PPV_ARGS(OutResource));

    if (hr != S_OK)
    {
        LOG_ERROR("CreateCommittedResource result: {:X}", (UINT64) hr);
        return false;
    }

    LOG_DEBUG("Created new one: {}x{}", inDesc.Width, inDesc.Height);
    return true;
}

void Shader_Dx12::SetBufferState(ID3D12GraphicsCommandList* InCommandList, D3D12_RESOURCE_STATES InState,
                                 ID3D12Resource* Buffer, D3D12_RESOURCE_STATES* BufferState)
{
    std::unique_lock<std::recursive_mutex> lock(DlssNr::Submission::RecordingMutex(), std::defer_lock);
    if (_recordedLifetime)
    {
        lock.lock();
        auto owner = _recordedBuffer.lock();
        Microsoft::WRL::ComPtr<DlssNr::Submission::ILogicalCommandList> logical;
        if (!owner || owner->resource.Get() != Buffer || !RecordedLifetimeSupported(InCommandList) ||
            FAILED(InCommandList->QueryInterface(IID_PPV_ARGS(&logical))) || owner->invalidated ||
            !(owner->identity == logical->Identity()))
        {
            _admissionFailedList = InCommandList;
            return; // No barrier on an unowned recording; Dispatch fails closed too.
        }
        if (_admissionFailedList == InCommandList) _admissionFailedList = nullptr;
        BufferState = &owner->state;
    }
    if (BufferState == nullptr || *BufferState == InState)
        return;

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = Buffer;
    barrier.Transition.StateBefore = *BufferState;
    barrier.Transition.StateAfter = InState;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    InCommandList->ResourceBarrier(1, &barrier);

    *BufferState = InState;
}

std::function<void()> Shader_Dx12::RecordedBufferCleanup(ID3D12GraphicsCommandList* list)
{
    if (!_recordedLifetime) return {};
    std::lock_guard lock(DlssNr::Submission::RecordingMutex());
    auto owner = _recordedBuffer.lock();
    if (!owner) return {};
    return [owner = std::move(owner), list]()
    {
        std::lock_guard lock(DlssNr::Submission::RecordingMutex());
        ComPtr<DlssNr::Submission::ILogicalCommandList> logical;
        if (!list || owner->invalidated || FAILED(list->QueryInterface(IID_PPV_ARGS(&logical))) ||
            !(owner->identity == logical->Identity()) || owner->state == D3D12_RESOURCE_STATE_COMMON) return;
        const auto barrier = CD3DX12_RESOURCE_BARRIER::Transition(owner->resource.Get(), owner->state,
                                                                 D3D12_RESOURCE_STATE_COMMON);
        list->ResourceBarrier(1, &barrier);
        owner->state = D3D12_RESOURCE_STATE_COMMON;
    };
}

std::shared_ptr<RecordedShader::Dispatch> Shader_Dx12::AcquireDispatchLease(
    ID3D12GraphicsCommandList* list, ID3D12PipelineState* selectedPSO,
    std::initializer_list<ID3D12Resource*> resources)
{
    if (!_recordedLifetime || !RecordedLifetimeSupported(list) ||
        !_device || !_rootSignature || !selectedPSO || !_constantBuffer) return {};
    if (_admissionFailedList == list) { _admissionFailedList = nullptr; return {}; }
    _admissionFailedList = nullptr;
    try
    {
        std::shared_ptr<RecordedShader::Dispatch> slot;
        for (auto& candidate : _dispatchSlots)
            if (candidate.use_count() == 1 && candidate->Available()) { slot = candidate; break; }
        if (!slot)
        {
            slot = std::make_shared<RecordedShader::Dispatch>();
            slot->device = _device;
            ScopedSkipHeapCapture skipHeapCapture {};
            if (!slot->heap.Initialize(_device, _srcCount, _uavCount, _cbvCount, _rtvCount)) return {};
            auto desc = _constantBuffer->GetDesc();
            desc.Width = (desc.Width + 255) & ~UINT64(255);
            const auto props = CD3DX12_HEAP_PROPERTIES(D3D12_HEAP_TYPE_UPLOAD);
            if (FAILED(_device->CreateCommittedResource(&props, D3D12_HEAP_FLAG_NONE, &desc,
                D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&slot->constants)))) return {};
            slot->timing = std::make_unique<GpuTime_Dx12>(_device, false);
            _dispatchSlots.push_back(slot);
        }
        else
        {
            slot->timing = std::make_unique<GpuTime_Dx12>(_device, false);
        }
        slot->root = _rootSignature;
        slot->pipeline = selectedPSO;
        slot->resources.clear();
        slot->intermediates.clear();
        for (auto* resource : resources)
            if (resource)
            {
                slot->resources.emplace_back(resource);
                if (auto owner = RecordedShader::IntermediateOf(resource))
                    slot->intermediates.push_back(std::move(owner));
            }
        if (!slot->Attach(list)) return {};
        return slot;
    }
    catch (...) { return {}; }
}

// From DirectXHelpers.cpp licensed under MIT
void Shader_Dx12::CreateShaderResourceView(ID3D12Device* device, ID3D12Resource* tex,
                                           D3D12_CPU_DESCRIPTOR_HANDLE srvDescriptor, DXGI_FORMAT format)
{
    if (!device || !tex)
        throw std::invalid_argument("Direct3D device and resource must be valid");

    const auto desc = tex->GetDesc();

    if ((desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
    {
        LOG_ERROR("ERROR: CreateShaderResourceView called on a resource created without support for SRV");
        throw std::runtime_error("Can't have D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE");
    }

    D3D12_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    const auto viewFormat = (format != DXGI_FORMAT_UNKNOWN) ? format : desc.Format;
    // This is already a depth-plane SRV format; the shared translator would turn it into a DSV format.
    srvDesc.Format = viewFormat == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS
                         ? viewFormat
                         : TranslateTypelessFormats(viewFormat);
    srvDesc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;

    const UINT mipLevels = (desc.MipLevels) ? static_cast<UINT>(desc.MipLevels) : static_cast<UINT>(-1);

    switch (desc.Dimension)
    {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        if (desc.DepthOrArraySize > 1)
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1DARRAY;
            srvDesc.Texture1DArray.MipLevels = mipLevels;
            srvDesc.Texture1DArray.ArraySize = static_cast<UINT>(desc.DepthOrArraySize);
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE1D;
            srvDesc.Texture1D.MipLevels = mipLevels;
        }
        break;

    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        if (desc.DepthOrArraySize > 1)
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DARRAY;
            srvDesc.Texture2DArray.MipLevels = mipLevels;
            srvDesc.Texture2DArray.ArraySize = static_cast<UINT>(desc.DepthOrArraySize);
        }
        else
        {
            srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            srvDesc.Texture2D.MipLevels = mipLevels;
        }
        break;

    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        srvDesc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        srvDesc.Texture3D.MipLevels = mipLevels;
        break;

    case D3D12_RESOURCE_DIMENSION_BUFFER:
        LOG_ERROR("ERROR: CreateShaderResourceView cannot be used with DIMENSION_BUFFER. Use "
                  "CreateBufferShaderResourceView.");
        throw std::invalid_argument("buffer resources not supported");

    case D3D12_RESOURCE_DIMENSION_UNKNOWN:
    default:
        LOG_ERROR("ERROR: CreateShaderResourceView cannot be used with DIMENSION_UNKNOWN ({}).",
                  (uint32_t) desc.Dimension);
        throw std::invalid_argument("unknown resource dimension");
    }

    device->CreateShaderResourceView(tex, &srvDesc, srvDescriptor);
}

void Shader_Dx12::CreateUnorderedAccessView(ID3D12Device* device, ID3D12Resource* tex,
                                            D3D12_CPU_DESCRIPTOR_HANDLE uavDescriptor, uint32_t mipLevel)
{
    if (!device || !tex)
        throw std::invalid_argument("Direct3D device and resource must be valid");

    const auto desc = tex->GetDesc();

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) == 0)
    {
        LOG_ERROR("ERROR: CreateUnorderedResourceView called on a resource created without support for UAV.");
        throw std::runtime_error("Requires D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS");
    }

    D3D12_UNORDERED_ACCESS_VIEW_DESC uavDesc = {};
    uavDesc.Format = TranslateTypelessFormats(desc.Format);

    switch (desc.Dimension)
    {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        if (desc.DepthOrArraySize > 1)
        {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1DARRAY;
            uavDesc.Texture1DArray.MipSlice = mipLevel;
            uavDesc.Texture1DArray.FirstArraySlice = 0;
            uavDesc.Texture1DArray.ArraySize = desc.DepthOrArraySize;
        }
        else
        {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE1D;
            uavDesc.Texture1D.MipSlice = mipLevel;
        }
        break;

    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        if (desc.DepthOrArraySize > 1)
        {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2DARRAY;
            uavDesc.Texture2DArray.MipSlice = mipLevel;
            uavDesc.Texture2DArray.ArraySize = desc.DepthOrArraySize;
        }
        else
        {
            uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
            uavDesc.Texture2D.MipSlice = mipLevel;
        }
        break;

    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        uavDesc.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE3D;
        uavDesc.Texture3D.MipSlice = mipLevel;
        uavDesc.Texture3D.WSize = desc.DepthOrArraySize;
        break;

    case D3D12_RESOURCE_DIMENSION_BUFFER:
        LOG_ERROR("ERROR: CreateUnorderedResourceView cannot be used with DIMENSION_BUFFER. Use "
                  "CreateBufferUnorderedAccessView.");
        throw std::invalid_argument("buffer resources not supported");

    case D3D12_RESOURCE_DIMENSION_UNKNOWN:
    default:
        LOG_ERROR("ERROR: CreateUnorderedResourceView cannot be used with DIMENSION_UNKNOWN ({}).",
                  (uint32_t) desc.Dimension);
        throw std::invalid_argument("unknown resource dimension");
    }
    device->CreateUnorderedAccessView(tex, nullptr, &uavDesc, uavDescriptor);
}

void Shader_Dx12::CreateRenderTargetView(ID3D12Device* device, ID3D12Resource* tex,
                                         D3D12_CPU_DESCRIPTOR_HANDLE rtvDescriptor, uint32_t mipLevel)
{
    if (!device || !tex)
        throw std::invalid_argument("Direct3D device and resource must be valid");

    const auto desc = tex->GetDesc();

    if ((desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0)
    {
        LOG_ERROR("ERROR: CreateRenderTargetView called on a resource created without support for RTV.");
        throw std::runtime_error("Requires D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET");
    }

    D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
    rtvDesc.Format = TranslateTypelessFormats(desc.Format);

    switch (desc.Dimension)
    {
    case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
        if (desc.DepthOrArraySize > 1)
        {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1DARRAY;
            rtvDesc.Texture1DArray.MipSlice = mipLevel;
            rtvDesc.Texture1DArray.FirstArraySlice = 0;
            rtvDesc.Texture1DArray.ArraySize = desc.DepthOrArraySize;
        }
        else
        {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE1D;
            rtvDesc.Texture1D.MipSlice = mipLevel;
        }
        break;

    case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
        if (desc.SampleDesc.Count > 1)
        {
            if (desc.DepthOrArraySize > 1)
            {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMSARRAY;
                rtvDesc.Texture2DMSArray.ArraySize = desc.DepthOrArraySize;
            }
            else
            {
                rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DMS;
            }
        }
        else if (desc.DepthOrArraySize > 1)
        {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2DARRAY;
            rtvDesc.Texture2DArray.MipSlice = mipLevel;
            rtvDesc.Texture2DArray.ArraySize = desc.DepthOrArraySize;
        }
        else
        {
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            rtvDesc.Texture2D.MipSlice = mipLevel;
        }
        break;

    case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE3D;
        rtvDesc.Texture3D.MipSlice = mipLevel;
        rtvDesc.Texture3D.WSize = desc.DepthOrArraySize;
        break;

    case D3D12_RESOURCE_DIMENSION_BUFFER:
        LOG_ERROR("ERROR: CreateRenderTargetView cannot be used with DIMENSION_BUFFER.");
        throw std::invalid_argument("buffer resources not supported");

    case D3D12_RESOURCE_DIMENSION_UNKNOWN:
    default:
        LOG_ERROR("ERROR: CreateRenderTargetView cannot be used with DIMENSION_UNKNOWN ({}).",
                  (uint32_t) desc.Dimension);
        throw std::invalid_argument("unknown resource dimension");
    }
    device->CreateRenderTargetView(tex, &rtvDesc, rtvDescriptor);
}

bool Shader_Dx12::SetupRootSignature(ID3D12Device* InDevice, uint32_t srcCount, uint32_t uavCount, uint32_t cbvCount,
                                     uint32_t rtvCount, uint32_t samplerCount, uint32_t staticSamplerCount,
                                     const D3D12_STATIC_SAMPLER_DESC* pStaticSamplers, D3D12_ROOT_SIGNATURE_FLAGS flags)
{
    if (_init)
    {
        LOG_ERROR("Already inited");
        return true;
    }

    _srcCount = srcCount;
    _uavCount = uavCount;
    _cbvCount = cbvCount;
    _rtvCount = rtvCount;
    _samplerCount = samplerCount;

    if (_srcCount > 0)
        _descriptorRanges.emplace_back(D3D12_DESCRIPTOR_RANGE_TYPE_SRV, _srcCount, 0);

    if (_uavCount > 0)
        _descriptorRanges.emplace_back(D3D12_DESCRIPTOR_RANGE_TYPE_UAV, _uavCount, 0);

    if (_cbvCount > 0)
        _descriptorRanges.emplace_back(D3D12_DESCRIPTOR_RANGE_TYPE_CBV, _cbvCount, 0);

    if (_samplerCount > 0)
        _descriptorRanges.emplace_back(D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER, _samplerCount, 0);

    CD3DX12_ROOT_PARAMETER1 rootParameter {};
    rootParameter.InitAsDescriptorTable(static_cast<UINT>(_descriptorRanges.size()), _descriptorRanges.data());

    CD3DX12_VERSIONED_ROOT_SIGNATURE_DESC rootSigDesc {};
    rootSigDesc.Init_1_1(1, &rootParameter, staticSamplerCount, pStaticSamplers, flags);

    ComPtr<ID3DBlob> errorBlob;
    ComPtr<ID3DBlob> signatureBlob;

    do
    {
        auto hr = D3D12SerializeVersionedRootSignature(&rootSigDesc, &signatureBlob, &errorBlob);

        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] D3D12SerializeVersionedRootSignature error {1:x}", _name, hr);
            break;
        }

        hr = InDevice->CreateRootSignature(0, signatureBlob->GetBufferPointer(), signatureBlob->GetBufferSize(),
                                           IID_PPV_ARGS(&_rootSignature));

        if (FAILED(hr))
        {
            LOG_ERROR("[{0}] CreateRootSignature error {1:x}", _name, hr);
            break;
        }

    } while (false);

    if (_rootSignature == nullptr)
    {
        LOG_ERROR("[{0}] _rootSignature is null!", _name);
        return false;
    }

    return true;
}

bool Shader_Dx12::InitHeaps(ID3D12Device* InDevice, FrameDescriptorHeap* pHeaps, size_t numOFHeaps)
{
    ScopedSkipHeapCapture skipHeapCapture {};

    for (size_t i = 0; i < numOFHeaps; i++)
    {
        if (!pHeaps[i].Initialize(InDevice, _srcCount, _uavCount, _cbvCount, _rtvCount))
        {
            LOG_ERROR("[{0}] Failed to init heap", _name);
            return false;
        }
    }

    return true;
}
