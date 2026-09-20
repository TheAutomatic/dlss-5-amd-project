#pragma once
#include <d3d12.h>
#include "RootBindState.h"
#include <cstdint>

namespace DlssNr::Submission
{
// Continuation seed after Split. Not a full graphics snapshot.
// Fail-closed: bindings we cannot restore must MarkSplitIneligible on the proxy.
struct ContinuationState
{
    static constexpr UINT kMaxVb = D3D12_IA_VERTEX_INPUT_RESOURCE_SLOT_COUNT; // 32
    static constexpr UINT kMaxSo = D3D12_SO_BUFFER_SLOT_COUNT;                 // 4

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

    bool hasOm = false;
    UINT numRts = 0;
    D3D12_CPU_DESCRIPTOR_HANDLE rts[8] {};
    BOOL omSingle = FALSE;
    bool hasDsv = false;
    D3D12_CPU_DESCRIPTOR_HANDLE dsv {};

    // IA / SO / VRS / view-instance (plan D)
    bool hasIb = false;
    bool ibNull = false; // IASetIndexBuffer(nullptr)
    D3D12_INDEX_BUFFER_VIEW ib {};

    bool vbSet[kMaxVb] {};
    D3D12_VERTEX_BUFFER_VIEW vb[kMaxVb] {};
    bool vbNull[kMaxVb] {}; // per-slot null clear

    bool soSet[kMaxSo] {};
    D3D12_STREAM_OUTPUT_BUFFER_VIEW so[kMaxSo] {};
    bool soNull[kMaxSo] {};

    bool hasStripCut = false;
    D3D12_INDEX_BUFFER_STRIP_CUT_VALUE stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;

    bool hasVrs = false;
    D3D12_SHADING_RATE vrsBase = D3D12_SHADING_RATE_1X1;
    D3D12_SHADING_RATE_COMBINER vrsCombiners[2] {};
    bool hasVrsImage = false;
    ID3D12Resource *vrsImage = nullptr; // may be null = clear image

    bool hasViewInstanceMask = false;
    UINT viewInstanceMask = 0;

    bool hasDepthBounds = false;
    FLOAT depthBoundsMin = 0.f;
    FLOAT depthBoundsMax = 1.f;

    // D3D12 max programmed sample positions: 16 pixels * 16 spp.
    static constexpr UINT kMaxSamplePositions = 256;
    bool hasSamplePositions = false;
    UINT samplesPerPixel = 0;
    UINT numSamplePixels = 0;
    D3D12_SAMPLE_POSITION samplePositions[kMaxSamplePositions] {};

    RootBindState gfxRoots;
    RootBindState computeRoots;

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
        if (vrsImage)
        {
            vrsImage->Release();
            vrsImage = nullptr;
        }
        hasVrsImage = false;
    }

    void Reset()
    {
        ReleaseRefs();
        hasViewports = hasScissors = hasTopology = hasBlend = hasStencil = hasOm = false;
        hasDsv = false;
        numRts = 0;
        numViewports = numScissors = 0;
        topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        stencilRef = 0;
        hasIb = ibNull = false;
        hasStripCut = false;
        stripCut = D3D12_INDEX_BUFFER_STRIP_CUT_VALUE_DISABLED;
        hasVrs = false;
        vrsBase = D3D12_SHADING_RATE_1X1;
        vrsCombiners[0] = vrsCombiners[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
        hasViewInstanceMask = false;
        viewInstanceMask = 0;
        hasDepthBounds = false;
        depthBoundsMin = 0.f;
        depthBoundsMax = 1.f;
        hasSamplePositions = false;
        samplesPerPixel = numSamplePixels = 0;
        gfxRoots.Reset();
        computeRoots.Reset();
        for (UINT i = 0; i < kMaxVb; ++i)
        {
            vbSet[i] = false;
            vbNull[i] = false;
            vb[i] = {};
        }
        for (UINT i = 0; i < kMaxSo; ++i)
        {
            soSet[i] = false;
            soNull[i] = false;
            so[i] = {};
        }
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
        // D3D12: setting the SAME root signature keeps existing root bindings.
        // Only clear the parameter snapshot when the signature identity changes.
        const bool changed = (gfxRoot != s);
        if (gfxRoot)
            gfxRoot->Release();
        gfxRoot = s;
        if (gfxRoot)
            gfxRoot->AddRef();
        hasGfxRoot = gfxRoot != nullptr;
        if (changed)
            gfxRoots.OnSignatureChanged();
    }

    void OnComputeRoot(ID3D12RootSignature *s)
    {
        const bool changed = (computeRoot != s);
        if (computeRoot)
            computeRoot->Release();
        computeRoot = s;
        if (computeRoot)
            computeRoot->AddRef();
        hasComputeRoot = computeRoot != nullptr;
        if (changed)
            computeRoots.OnSignatureChanged();
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

    void OnOm(UINT n, const D3D12_CPU_DESCRIPTOR_HANDLE *rt, BOOL single,
              const D3D12_CPU_DESCRIPTOR_HANDLE *ds)
    {
        numRts = 0;
        hasOm = false;
        hasDsv = false;
        omSingle = single;
        if (rt && n > 0)
        {
            // single=TRUE: only the first handle of a contiguous range is provided.
            if (single)
            {
                rts[0] = rt[0];
                numRts = n > 8 ? 8 : n;
            }
            else
            {
                if (n > 8)
                    n = 8;
                for (UINT i = 0; i < n; ++i)
                    rts[i] = rt[i];
                numRts = n;
            }
            hasOm = true;
        }
        if (ds)
        {
            dsv = *ds;
            hasDsv = true;
        }
    }

    void OnIndexBuffer(const D3D12_INDEX_BUFFER_VIEW *v)
    {
        if (!v)
        {
            hasIb = true;
            ibNull = true;
            ib = {};
            return;
        }
        hasIb = true;
        ibNull = false;
        ib = *v;
    }

    void OnVertexBuffers(UINT start, UINT n, const D3D12_VERTEX_BUFFER_VIEW *v)
    {
        if (n == 0 || start >= kMaxVb)
            return;
        if (start + n > kMaxVb)
            n = kMaxVb - start;
        for (UINT i = 0; i < n; ++i)
        {
            const UINT slot = start + i;
            vbSet[slot] = true;
            if (!v)
            {
                vbNull[slot] = true;
                vb[slot] = {};
            }
            else
            {
                vbNull[slot] = false;
                vb[slot] = v[i];
            }
        }
    }

    void OnSoTargets(UINT start, UINT n, const D3D12_STREAM_OUTPUT_BUFFER_VIEW *v)
    {
        if (n == 0 || start >= kMaxSo)
            return;
        if (start + n > kMaxSo)
            n = kMaxSo - start;
        for (UINT i = 0; i < n; ++i)
        {
            const UINT slot = start + i;
            soSet[slot] = true;
            if (!v)
            {
                soNull[slot] = true;
                so[slot] = {};
            }
            else
            {
                soNull[slot] = false;
                so[slot] = v[i];
            }
        }
    }

    void OnStripCut(D3D12_INDEX_BUFFER_STRIP_CUT_VALUE value)
    {
        stripCut = value;
        hasStripCut = true;
    }

    void OnShadingRate(D3D12_SHADING_RATE base, const D3D12_SHADING_RATE_COMBINER *combiners)
    {
        vrsBase = base;
        if (combiners)
        {
            vrsCombiners[0] = combiners[0];
            vrsCombiners[1] = combiners[1];
        }
        else
        {
            vrsCombiners[0] = vrsCombiners[1] = D3D12_SHADING_RATE_COMBINER_PASSTHROUGH;
        }
        hasVrs = true;
    }

    void OnShadingRateImage(ID3D12Resource *image)
    {
        if (vrsImage)
        {
            vrsImage->Release();
            vrsImage = nullptr;
        }
        vrsImage = image;
        if (vrsImage)
            vrsImage->AddRef();
        hasVrsImage = true; // includes explicit null clear
    }

    void OnViewInstanceMask(UINT mask)
    {
        viewInstanceMask = mask;
        hasViewInstanceMask = true;
    }

    UINT CapturedVbSlotCount() const
    {
        UINT n = 0;
        for (UINT i = 0; i < kMaxVb; ++i)
            if (vbSet[i])
                ++n;
        return n;
    }

    void OnDepthBounds(FLOAT mn, FLOAT mx)
    {
        depthBoundsMin = mn;
        depthBoundsMax = mx;
        hasDepthBounds = true;
    }

    void OnSamplePositions(UINT spp, UINT pixels, const D3D12_SAMPLE_POSITION *pos)
    {
        if (spp == 0 || pixels == 0 || !pos)
        {
            hasSamplePositions = true;
            samplesPerPixel = 0;
            numSamplePixels = 0;
            return;
        }
        const UINT total = spp * pixels;
        if (total > kMaxSamplePositions)
            return;
        samplesPerPixel = spp;
        numSamplePixels = pixels;
        for (UINT i = 0; i < total; ++i)
            samplePositions[i] = pos[i];
        hasSamplePositions = true;
    }

    static bool SamplePositionsOverflow(UINT spp, UINT pixels)
    {
        return spp != 0 && pixels != 0 && (spp * pixels) > kMaxSamplePositions;
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
        computeRoots.ApplyCompute(list);
        gfxRoots.ApplyGraphics(list);
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
        if (hasOm || hasDsv)
            list->OMSetRenderTargets(numRts, hasOm ? rts : nullptr, omSingle,
                                     hasDsv ? &dsv : nullptr);
        if (hasIb)
            list->IASetIndexBuffer(ibNull ? nullptr : &ib);
        // Rebind VB slots; coalesce contiguous runs where possible.
        {
            UINT i = 0;
            while (i < kMaxVb)
            {
                if (!vbSet[i])
                {
                    ++i;
                    continue;
                }
                const UINT start = i;
                const bool nullRun = vbNull[i];
                UINT n = 0;
                while (i < kMaxVb && vbSet[i] && vbNull[i] == nullRun)
                {
                    ++n;
                    ++i;
                }
                if (nullRun)
                    list->IASetVertexBuffers(start, n, nullptr);
                else
                    list->IASetVertexBuffers(start, n, &vb[start]);
            }
        }
        {
            UINT i = 0;
            while (i < kMaxSo)
            {
                if (!soSet[i])
                {
                    ++i;
                    continue;
                }
                const UINT start = i;
                const bool nullRun = soNull[i];
                UINT n = 0;
                while (i < kMaxSo && soSet[i] && soNull[i] == nullRun)
                {
                    ++n;
                    ++i;
                }
                if (nullRun)
                    list->SOSetTargets(start, n, nullptr);
                else
                    list->SOSetTargets(start, n, &so[start]);
            }
        }

        ID3D12GraphicsCommandList1 *l1 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l1))) && l1)
        {
            if (hasViewInstanceMask)
                l1->SetViewInstanceMask(viewInstanceMask);
            if (hasDepthBounds)
                l1->OMSetDepthBounds(depthBoundsMin, depthBoundsMax);
            if (hasSamplePositions)
            {
                if (samplesPerPixel == 0 || numSamplePixels == 0)
                    l1->SetSamplePositions(0, 0, nullptr);
                else
                    l1->SetSamplePositions(samplesPerPixel, numSamplePixels,
                                           const_cast<D3D12_SAMPLE_POSITION *>(samplePositions));
            }
            l1->Release();
        }
        ID3D12GraphicsCommandList9 *l9 = nullptr;
        if (hasStripCut && SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l9))) && l9)
        {
            l9->IASetIndexBufferStripCutValue(stripCut);
            l9->Release();
        }

        ID3D12GraphicsCommandList5 *l5 = nullptr;
        if (SUCCEEDED(list->QueryInterface(IID_PPV_ARGS(&l5))) && l5)
        {
            if (hasVrs)
                l5->RSSetShadingRate(vrsBase, vrsCombiners);
            if (hasVrsImage)
                l5->RSSetShadingRateImage(vrsImage);
            l5->Release();
        }
    }
};
} // namespace DlssNr::Submission