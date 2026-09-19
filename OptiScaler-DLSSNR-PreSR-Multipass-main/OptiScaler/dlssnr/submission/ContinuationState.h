#pragma once
#include <d3d12.h>
#include <cstdint>

namespace DlssNr::Submission
{
// Minimal continuation seed after Split. Not a full graphics snapshot.
// Fail-closed: unknown / unsupported bindings set ineligible via callback.
struct ContinuationState
{
    bool hasViewports = false;
    UINT numViewports = 0;
    D3D12_VIEWPORT viewports[16] {};

    bool hasScissors = false;
    UINT numScissors = 0;
    D3D12_RECT scissors[16] {};

    bool hasTopology = false;
    D3D12_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;

    bool hasPso = false;
    ID3D12PipelineState *pso = nullptr;

    bool hasGfxRoot = false;
    ID3D12RootSignature *gfxRoot = nullptr;

    bool hasComputeRoot = false;
    ID3D12RootSignature *computeRoot = nullptr;

    bool hasHeaps = false;
    UINT numHeaps = 0;
    ID3D12DescriptorHeap *heaps[2] {};

    bool hasBlend = false;
    FLOAT blendFactor[4] { 0, 0, 0, 0 };

    bool hasStencil = false;
    UINT stencilRef = 0;

    ContinuationState() = default;
    ~ContinuationState() { ReleaseRefs(); }
    ContinuationState(const ContinuationState &) = delete;
    ContinuationState &operator=(const ContinuationState &) = delete;

    void ReleaseRefs()
    {
        if (pso)
        {
            pso->Release();
            pso = nullptr;
        }
        if (gfxRoot)
        {
            gfxRoot->Release();
            gfxRoot = nullptr;
        }
        if (computeRoot)
        {
            computeRoot->Release();
            computeRoot = nullptr;
        }
        for (UINT i = 0; i < numHeaps; ++i)
        {
            if (heaps[i])
            {
                heaps[i]->Release();
                heaps[i] = nullptr;
            }
        }
        numHeaps = 0;
        hasPso = hasGfxRoot = hasComputeRoot = hasHeaps = false;
    }

    void Reset()
    {
        ReleaseRefs();
        hasViewports = hasScissors = hasTopology = hasBlend = hasStencil = false;
        numViewports = numScissors = 0;
        topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        stencilRef = 0;
    }

    void OnViewports(UINT n, const D3D12_VIEWPORT *v)
    {
        if (!v || n == 0 || n > 16)
            return;
        numViewports = n;
        for (UINT i = 0; i < n; ++i)
            viewports[i] = v[i];
        hasViewports = true;
    }

    void OnScissors(UINT n, const D3D12_RECT *r)
    {
        if (!r || n == 0 || n > 16)
            return;
        numScissors = n;
        for (UINT i = 0; i < n; ++i)
            scissors[i] = r[i];
        hasScissors = true;
    }

    void OnTopology(D3D12_PRIMITIVE_TOPOLOGY t)
    {
        topology = t;
        hasTopology = true;
    }

    void OnPso(ID3D12PipelineState *p)
    {
        if (pso)
            pso->Release();
        pso = p;
        if (pso)
            pso->AddRef();
        hasPso = pso != nullptr;
    }

    void OnGfxRoot(ID3D12RootSignature *s)
    {
        if (gfxRoot)
            gfxRoot->Release();
        gfxRoot = s;
        if (gfxRoot)
            gfxRoot->AddRef();
        hasGfxRoot = gfxRoot != nullptr;
    }

    void OnComputeRoot(ID3D12RootSignature *s)
    {
        if (computeRoot)
            computeRoot->Release();
        computeRoot = s;
        if (computeRoot)
            computeRoot->AddRef();
        hasComputeRoot = computeRoot != nullptr;
    }

    void OnHeaps(UINT n, ID3D12DescriptorHeap *const *h)
    {
        for (UINT i = 0; i < numHeaps; ++i)
        {
            if (heaps[i])
            {
                heaps[i]->Release();
                heaps[i] = nullptr;
            }
        }
        numHeaps = 0;
        hasHeaps = false;
        if (!h || n == 0 || n > 2)
            return;
        for (UINT i = 0; i < n; ++i)
        {
            heaps[i] = h[i];
            if (heaps[i])
                heaps[i]->AddRef();
        }
        numHeaps = n;
        hasHeaps = true;
    }

    void OnBlend(const FLOAT f[4])
    {
        if (!f)
            return;
        for (int i = 0; i < 4; ++i)
            blendFactor[i] = f[i];
        hasBlend = true;
    }

    void OnStencil(UINT s)
    {
        stencilRef = s;
        hasStencil = true;
    }

    // Apply captured bindings onto a fresh continuation list.
    void ApplyTo(ID3D12GraphicsCommandList *list) const
    {
        if (!list)
            return;
        if (hasPso)
            list->SetPipelineState(pso);
        if (hasGfxRoot)
            list->SetGraphicsRootSignature(gfxRoot);
        if (hasComputeRoot)
            list->SetComputeRootSignature(computeRoot);
        if (hasHeaps)
            list->SetDescriptorHeaps(numHeaps, heaps);
        if (hasViewports)
            list->RSSetViewports(numViewports, viewports);
        if (hasScissors)
            list->RSSetScissorRects(numScissors, scissors);
        if (hasTopology)
            list->IASetPrimitiveTopology(topology);
        if (hasBlend)
            list->OMSetBlendFactor(blendFactor);
        if (hasStencil)
            list->OMSetStencilRef(stencilRef);
    }
};
} // namespace DlssNr::Submission
