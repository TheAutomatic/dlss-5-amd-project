// SPDX-License-Identifier: MIT
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <dxgi1_6.h>

#include "opennr_rr/d3d12.hpp"

#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace onr {

namespace {

// The AMD D3D12 driver extension, as exported by amdxc64.dll (AmdExtD3DCreateInterface). Only the vtable order and
// the structure used here are declared; the layout follows the interface headers of the Radeon Developer Tool Suite
// samples (AmdDxExt).
struct __declspec(uuid("014937EC-9288-446F-A9AC-D75A8E3A984F")) AmdFactory : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateInterface(IUnknown* outer, REFIID riid, void** out) = 0;
};
struct AmdElfInfo {
    uint32_t type = 2;   // pipeline ELF
    void* next = nullptr;
    const void* elf = nullptr;
    size_t size = 0;
    uint32_t threads[3] = {1, 1, 1};
};
struct __declspec(uuid("BDC14598-B7D2-4A8D-9CA5-67848E2AF745")) AmdDevice5 : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateGraphicsPipelineState(const void*, const void*, REFIID, void**) = 0;
    virtual void STDMETHODCALLTYPE PushMarker(ID3D12GraphicsCommandList*, const char*) = 0;
    virtual void STDMETHODCALLTYPE PopMarker(ID3D12GraphicsCommandList*) = 0;
    virtual void STDMETHODCALLTYPE SetMarker(ID3D12GraphicsCommandList*, const char*) = 0;
    virtual HRESULT STDMETHODCALLTYPE CheckExtFeatureSupport(uint32_t, void*, size_t) const = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateComputePipelineState(const void*, const void*, REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreatePipelineState(const void*, const void*, REFIID, void**) = 0;
    virtual void STDMETHODCALLTYPE SetPrimitiveTopology(ID3D12GraphicsCommandList*, uint32_t) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateComputePipelineFromElf(AmdElfInfo*, REFIID, void**) = 0;
};

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char b[128];
        snprintf(b, sizeof b, "%s failed (0x%08lx)", what, (unsigned long)hr);
        throw std::runtime_error(b);
    }
}

template <class T> void release(T*& p) {
    if (p) p->Release(), p = nullptr;
}

ID3D12Resource* buffer(ID3D12Device* dev, uint64_t size, D3D12_HEAP_TYPE heap, D3D12_RESOURCE_STATES state, bool uav) {
    D3D12_HEAP_PROPERTIES hp{heap};
    D3D12_RESOURCE_DESC rd{D3D12_RESOURCE_DIMENSION_BUFFER, 0, std::max<uint64_t>(size, 256), 1, 1, 1, DXGI_FORMAT_UNKNOWN, {1, 0},
                           D3D12_TEXTURE_LAYOUT_ROW_MAJOR, uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE};
    ID3D12Resource* r = nullptr;
    check(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, state, nullptr, IID_PPV_ARGS(&r)), "CreateCommittedResource");
    return r;
}

D3D12_RESOURCE_BARRIER transition(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
    D3D12_RESOURCE_BARRIER x{};
    x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    x.Transition = {r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b};
    return x;
}

uint64_t fnv(const void* p, size_t n, uint64_t h = 1469598103934665603ull) {
    auto b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}

std::string hex(uint64_t v) {
    char b[17];
    snprintf(b, sizeof b, "%016llx", (unsigned long long)v);
    return b;
}

constexpr char kCacheMagic[8] = {'O', 'N', 'R', 'E', 'L', 'F', '1', 0};

}  // namespace

// Vulkan (XGL) user data: entry 0 global table, entries 1..64 the push constant dwords 0..63, entry 65 the pointer to
// descriptor set 0; entries at or past the spill threshold are read from the spill table at byte 4 * entry. D3D12:
// entries 0..3 belong to the driver and root parameter 0 starts at entry 4. So the root constants are the push
// constant dwords 3..60 (entries 4..61, as in Vulkan), then dwords 0..2 (entries 62..64) and entry 65: every entry
// >= 4 keeps its Vulkan number (the spill offsets in the ISA stay valid) and only the user SGPRs that held entries
// 1..3 are pointed at 62..64. Only the PAL metadata (msgpack) changes, in place.
std::vector<uint8_t> patch_elf(const std::vector<uint8_t>& vk) {
    std::vector<uint8_t> e = vk;
    auto find = [&](const char* key, size_t n) {
        auto k = reinterpret_cast<const uint8_t*>(key);
        auto it = std::search(e.begin(), e.end(), k, k + n);
        if (it == e.end() || std::search(it + 1, e.end(), k, k + n) != e.end()) throw std::runtime_error(std::string("ELF: one ") + (key + 1) + " expected");
        return size_t(it - e.begin()) + n;
    };
    // Entries 1..3 at or past the spill threshold could be read from the spill table, where D3D12 keeps its own
    // entries 0..3. With such a threshold the code may hold no scalar load at offsets 4..15 from any base (a
    // conservative test: a buffer read there refuses the kernel too). Kernels whose push constants spill from the
    // start keep their first three dwords unused (ingest).
    size_t t = find("\xb0.spill_threshold", 17);
    const uint32_t thr = e[t] < 0x80 ? e[t] : e[t] == 0xcc ? e[t + 1] : e[t] == 0xcd ? uint32_t(e[t + 1] << 8 | e[t + 2]) : 0;
    if (thr <= 3) {
        auto rd = [&](size_t at, int n) {
            uint64_t v = 0;
            if (at + n > e.size()) throw std::runtime_error("ELF: truncated");
            memcpy(&v, e.data() + at, n);
            return v;
        };
        const uint64_t shoff = rd(0x28, 8);
        const uint32_t shsize = uint32_t(rd(0x3a, 2)), shnum = uint32_t(rd(0x3c, 2)), shstr = uint32_t(rd(0x3e, 2));
        const uint64_t names = rd(shoff + uint64_t(shstr) * shsize + 0x18, 8);
        bool text = false;
        for (uint32_t s = 0; s < shnum; ++s) {
            const uint64_t h = shoff + uint64_t(s) * shsize;
            const char* nm = reinterpret_cast<const char*>(e.data() + names + rd(h, 4));
            if (strcmp(nm, ".text")) continue;
            text = true;
            const uint64_t off = rd(h + 0x18, 8), size = rd(h + 0x20, 8);
            for (uint64_t a = off; a + 8 <= off + size; a += 4) {
                const uint32_t w0 = uint32_t(rd(a, 4)), w1 = uint32_t(rd(a + 4, 4));
                const uint32_t imm = w1 & 0xffffff;
                if ((w0 & 0xfc000000u) == 0xf4000000u && imm >= 4 * std::max(thr, 1u) && imm < 16)
                    throw std::runtime_error("ELF: a scalar load may read push constant dwords 0..2 through the spill table");
            }
        }
        if (!text) throw std::runtime_error("ELF: no .text");
    }
    size_t i = find("\xb2.user_data_reg_map", 19);
    if (e[i] != 0xdc) throw std::runtime_error("ELF: reg map is not an array16");
    uint32_t n = (e[i + 1] << 8) | e[i + 2];
    i += 3;
    for (uint32_t s = 0; s < n; ++s) {
        if (e[i] == 0xce) { i += 5; continue; }   // special entry (global table, spill table, workgroup count) or unused
        uint8_t v = e[i];
        if (v >= 0x80) throw std::runtime_error("ELF: reg map entry is not a fixint");
        if (v >= 1 && v <= 3) e[i] = uint8_t(62 + (v - 1));
        else if (v >= 62 && v != 65) throw std::runtime_error("ELF: an SGPR reads push constant dwords past 60");
        ++i;
    }
    i = find("\xb0.user_data_limit", 17);
    if (e[i] >= 0x80) throw std::runtime_error("ELF: user_data_limit is not a fixint");
    e[i] = uint8_t(4 + kRootDwords);
    return e;
}

void elf_threads(const std::vector<uint8_t>& e, uint32_t t[3]) {
    const char key[] = "\xb7.threadgroup_dimensions";
    auto k = reinterpret_cast<const uint8_t*>(key);
    auto it = std::search(e.begin(), e.end(), k, k + 24);
    if (it == e.end() || it + 32 > e.end() || it[24] != 0x93) throw std::runtime_error("ELF: no threadgroup_dimensions");
    for (int d = 0, i = 25; d < 3; ++d) {
        uint8_t c = it[i];
        uint32_t v = c < 0x80 ? c : c == 0xcc ? it[i + 1] : c == 0xcd ? uint32_t(it[i + 1] << 8 | it[i + 2]) : 0;
        if (!v) throw std::runtime_error("ELF: unexpected threadgroup_dimensions encoding");
        i += c < 0x80 ? 1 : c == 0xcc ? 2 : 3;
        t[d] = v;
    }
}

void root_constants(const void* push, uint32_t size, uint32_t* rc) {
    if (size > kMaxPushBytes) throw std::runtime_error("push constants past 244 bytes do not fit the root signature");
    uint32_t w[61] = {};
    memcpy(w, push, size);
    for (uint32_t i = 3; i < 61; ++i) rc[i - 3] = w[i];
    for (uint32_t i = 0; i < 3; ++i) rc[58 + i] = w[i];
    rc[61] = 0;
}

D3dDevice::~D3dDevice() {
    if (q_ && fence_) wait_own();
    reap(true);
    if (qrb_ && done_map_) qrb_->Unmap(0, nullptr);
    if (ring_ && ring_map_) ring_->Unmap(0, nullptr);
    release(heap_), release(qrb_), release(qh_), release(ring_), release(fence_), release(own_), release(ca_), release(q_);
    release(dispatch_sig_), release(rs_);
    if (ext_) static_cast<IUnknown*>(ext_)->Release();
    release(dev_);
}

bool D3dDevice::init(ID3D12Device* dev, Device* compiler, const std::wstring& cache_dir, std::string& err) {
    try {
        dev_ = dev;
        dev_->AddRef();
        compiler_ = compiler;
        HMODULE umd = GetModuleHandleA("amdxc64.dll");
        auto create = umd ? reinterpret_cast<HRESULT(__cdecl*)(IUnknown*, REFIID, void**)>(GetProcAddress(umd, "AmdExtD3DCreateInterface")) : nullptr;
        if (!create) throw std::runtime_error("amdxc64.dll (the AMD D3D12 driver) has no AmdExtD3DCreateInterface");
        AmdFactory* f = nullptr;
        check(create(dev_, __uuidof(AmdFactory), (void**)&f), "AmdExtD3DCreateInterface");
        AmdDevice5* x = nullptr;
        HRESULT hr = f->CreateInterface(dev_, __uuidof(AmdDevice5), (void**)&x);
        f->Release();
        check(hr, "IAmdExtD3DDevice5");
        ext_ = x;

        // the cache key: both drivers' versions (the ELF comes from the Vulkan one, runs on the D3D12 one)
        LARGE_INTEGER umd_ver{};
        IDXGIFactory4* fac = nullptr;
        if (SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&fac)))) {
            IDXGIAdapter* ad = nullptr;
            if (SUCCEEDED(fac->EnumAdapterByLuid(dev_->GetAdapterLuid(), IID_PPV_ARGS(&ad)))) {
                ad->CheckInterfaceSupport(__uuidof(IDXGIDevice), &umd_ver);
                ad->Release();
            }
            fac->Release();
        }
        driver_key_ = compiler_->info().name + "|" + std::to_string(compiler_->info().driver_version) + "|" + compiler_->info().driver_info + "|" +
                      std::to_string(umd_ver.QuadPart) + "|patch 2";
        if (!cache_dir.empty()) cache_ = cache_dir + L"\\elf-" + std::filesystem::path(hex(fnv(driver_key_.data(), driver_key_.size()))).wstring();

        D3D12_ROOT_PARAMETER rp{};
        rp.ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
        rp.Constants.Num32BitValues = kRootDwords;
        D3D12_ROOT_SIGNATURE_DESC rsd{1, &rp};
        ID3DBlob *blob = nullptr, *msg = nullptr;
        check(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &msg), "D3D12SerializeRootSignature");
        hr = dev_->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&rs_));
        blob->Release();
        if (msg) msg->Release();
        check(hr, "CreateRootSignature");
        D3D12_INDIRECT_ARGUMENT_DESC ia{D3D12_INDIRECT_ARGUMENT_TYPE_DISPATCH};
        D3D12_COMMAND_SIGNATURE_DESC cs{12, 1, &ia, 0};
        check(dev_->CreateCommandSignature(&cs, nullptr, IID_PPV_ARGS(&dispatch_sig_)), "CreateCommandSignature");

        D3D12_COMMAND_QUEUE_DESC qd{D3D12_COMMAND_LIST_TYPE_DIRECT};
        check(dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&q_)), "CreateCommandQueue");
        check(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca_)), "CreateCommandAllocator");
        check(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca_, nullptr, IID_PPV_ARGS(&own_)), "CreateCommandList");
        own_->Close();
        check(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)), "CreateFence");
        check(q_->GetTimestampFrequency(&ts_freq_), "GetTimestampFrequency");

        ring_ = buffer(dev_, uint64_t(kSlots) << 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        check(ring_->Map(0, nullptr, reinterpret_cast<void**>(&ring_map_)), "Map");
        D3D12_QUERY_HEAP_DESC qhd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, kSlots * kSlotQueries};
        check(dev_->CreateQueryHeap(&qhd, IID_PPV_ARGS(&qh_)), "CreateQueryHeap");
        qrb_ = buffer(dev_, uint64_t(kSlots) * kSlotQueries * 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
        // The ELFs read set 0 at the low 32 bits in its user data entry with the high bits of their own code address,
        // 32 bytes a view: the convention the driver's own descriptor tables follow, so a shader-visible heap fits.
        // Each recording slot owns kSlotViews (18) + kTableViews (12) descriptors so its sampled-image table is
        // snapshotted per recording and never overwritten in place by a later resize() or set_texture().
        if (dev_->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV) == 32) {
            D3D12_DESCRIPTOR_HEAP_DESC hd{D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, (kSlots + kOverflowDescSlots) * kSlotStride,
                                          D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE};
            check(dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&heap_)), "CreateDescriptorHeap");
            heap_cpu_ = heap_->GetCPUDescriptorHandleForHeapStart().ptr;
            heap_gpu_ = heap_->GetGPUDescriptorHandleForHeapStart().ptr;
        }
        void* m = nullptr;
        check(qrb_->Map(0, nullptr, &m), "Map");
        memset(m, 0, size_t(kSlots) * kSlotQueries * 8);
        done_map_ = static_cast<volatile uint64_t*>(m);
        return true;
    } catch (const std::exception& e) {
        err = e.what();
        return false;
    }
}

void D3dDevice::wait_own() {
    if (fence_->GetCompletedValue() >= fv_) return;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    fence_->SetEventOnCompletion(fv_, ev);
    WaitForSingleObject(ev, 10000);
    CloseHandle(ev);
}

void D3dDevice::begin_own() {
    own_m_.lock();
    check(ca_->Reset(), "CommandAllocator::Reset");
    check(own_->Reset(ca_, nullptr), "CommandList::Reset");
    bind(own_, kOwnSlot);
}

void D3dDevice::submit_own() {
    const uint32_t s = slot_;
    unbind();
    HRESULT hr = own_->Close();
    if (SUCCEEDED(hr)) {
        ID3D12CommandList* l = own_;
        q_->ExecuteCommandLists(1, &l);
        hr = q_->Signal(fence_, ++fv_);
        slots_[s].own_fv = fv_;
        if (SUCCEEDED(hr)) wait_own();
    }
    own_m_.unlock();
    check(hr, "own list");
    check(dev_->GetDeviceRemovedReason(), "the D3D12 device was removed");
}

bool D3dDevice::slot_retired(SlotState& s, uint32_t idx) {
    if (!s.serial) return true;
    if (dev_ && FAILED(dev_->GetDeviceRemovedReason())) return true;
    if (s.own) return s.own_fv != 0 && fence_ && fence_->GetCompletedValue() >= s.own_fv;
    if (binds_ - s.serial >= kGameSlots) return true;
    return s.query_count > 0 && done_map_ && done_map_[idx * kSlotQueries + s.query_count - 1] != 0;
}

bool D3dDevice::overflow_retired(OverflowUpload& o) {
    if (!o.serial) return true;
    if (dev_ && FAILED(dev_->GetDeviceRemovedReason())) return true;
    if (binds_ - o.serial >= kGameSlots) return true;
    return o.done && *o.done != 0;
}

bool D3dDevice::serial_retired(uint64_t serial) {
    if (!serial || serial == current_serial_) return !serial;
    for (uint32_t i = 0; i < kSlots; ++i)
        if (slots_[i].serial == serial) return slot_retired(slots_[i], i);
    for (auto& o : overflow_)
        if (o.serial == serial) return overflow_retired(o);
    return true;
}

void D3dDevice::reap(bool all) {
    if (all) {
        for (auto& s : slots_) s.serial = 0;
        for (auto& o : overflow_) {
            if (o.qrb && o.done) o.qrb->Unmap(0, nullptr);
            if (o.qrb) o.qrb->Release();
            if (o.qh) o.qh->Release();
            if (o.ring && o.map) o.ring->Unmap(0, nullptr);
            if (o.ring) o.ring->Release();
        }
        overflow_.clear();
        for (auto& g : grave_)
            if (g.obj) g.obj->Release();
        grave_.clear();
        return;
    }
    for (uint32_t i = 0; i < kSlots; ++i)
        if (slots_[i].serial && slots_[i].serial != current_serial_ && slot_retired(slots_[i], i))
            slots_[i].serial = 0;
    std::erase_if(overflow_, [&](OverflowUpload& o) {
        if (o.serial == current_serial_ || !overflow_retired(o)) return false;
        if (o.qrb && o.done) o.qrb->Unmap(0, nullptr);
        if (o.qrb) o.qrb->Release();
        if (o.qh) o.qh->Release();
        if (o.ring && o.map) o.ring->Unmap(0, nullptr);
        if (o.ring) o.ring->Release();
        return true;
    });
    size_t keep = 0;
    for (auto& g : grave_) {
        std::erase_if(g.wait_serials, [&](uint64_t s) { return serial_retired(s); });
        if (!g.wait_serials.empty()) {
            grave_[keep++] = std::move(g);
            continue;
        }
        if (g.obj) g.obj->Release();
    }
    grave_.resize(keep);
}

bool D3dDevice::all_retired() {
    reap(false);
    for (uint32_t i = 0; i < kSlots; ++i)
        if (slots_[i].serial && !slot_retired(slots_[i], i)) return false;
    return grave_.empty() && overflow_.empty();
}

void D3dDevice::bury(IUnknown* u) {
    if (!u) return;
    reap(false);
    Grave g;
    g.obj = u;
    for (uint32_t i = 0; i < kSlots; ++i)
        if (slots_[i].serial && !slot_retired(slots_[i], i)) g.wait_serials.push_back(slots_[i].serial);
    for (auto& o : overflow_)
        if (o.serial && !overflow_retired(o)) g.wait_serials.push_back(o.serial);
    if (current_serial_) g.wait_serials.push_back(current_serial_);
    if (g.wait_serials.empty()) {
        u->Release();
        return;
    }
    grave_.push_back(std::move(g));
}

uint64_t D3dDevice::bind(ID3D12GraphicsCommandList* cl, uint32_t slot) {
    current_serial_ = ++binds_;
    reap(false);
    cl_ = cl;
    uint32_t picked = (cl == own_) ? kOwnSlot : (slot % kGameSlots);
    bool found = cl == own_ || slot_retired(slots_[picked], picked);
    if (!found) {
        for (uint32_t i = 0; i < kGameSlots; ++i) {
            uint32_t cand = (picked + i) % kGameSlots;
            if (slot_retired(slots_[cand], cand)) {
                picked = cand;
                found = true;
                break;
            }
        }
    }
    slot_ = picked;
    nquery_ = 0;
    stamp_names.clear();
    if (found) {
        using_overflow_ = false;
        desc_slot_ = slot_;
        auto& s = slots_[slot_];
        s = SlotState{};
        s.serial = current_serial_;
        s.own = (cl == own_);
        if (done_map_) memset(const_cast<uint64_t*>(done_map_) + size_t(slot_) * kSlotQueries, 0, kSlotQueries * 8);
        cur_ring_ = ring_;
        cur_ring_map_ = ring_map_;
        ring_off_ = uint64_t(slot_) << 16, ring_end_ = ring_off_ + (1u << 16);
    } else {
        using_overflow_ = true;
        uint32_t picked_desc = UINT32_MAX;
        for (uint32_t i = 0; i < kOverflowDescSlots; ++i) {
            const uint32_t cand = kSlots + ((overflow_cursor_ + i) % kOverflowDescSlots);
            const bool busy = std::any_of(overflow_.begin(), overflow_.end(), [&](const OverflowUpload& x) { return x.desc_slot == cand; });
            if (!busy) {
                picked_desc = cand;
                overflow_cursor_ = (overflow_cursor_ + i + 1) % kOverflowDescSlots;
                break;
            }
        }
        if (picked_desc == UINT32_MAX)
            throw std::runtime_error("D3dDevice: all recording descriptor slots are in flight");
        desc_slot_ = picked_desc;
        OverflowUpload o;
        o.serial = current_serial_;
        o.desc_slot = desc_slot_;
        o.ring = buffer(dev_, 1u << 16, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
        check(o.ring->Map(0, nullptr, reinterpret_cast<void**>(&o.map)), "Map");
        D3D12_QUERY_HEAP_DESC oqhd{D3D12_QUERY_HEAP_TYPE_TIMESTAMP, 1};
        if (SUCCEEDED(dev_->CreateQueryHeap(&oqhd, IID_PPV_ARGS(&o.qh)))) {
            o.qrb = buffer(dev_, 8, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
            void* dm = nullptr;
            if (SUCCEEDED(o.qrb->Map(0, nullptr, &dm)) && dm) {
                *static_cast<uint64_t*>(dm) = 0;
                o.done = static_cast<volatile uint64_t*>(dm);
            }
        }
        cur_ring_ = o.ring;
        cur_ring_map_ = o.map;
        ring_off_ = 0, ring_end_ = (1u << 16) - 256;
        overflow_.push_back(std::move(o));
    }
    if (heap_) {
        const uint64_t base_desc = uint64_t(desc_slot_) * kSlotStride * 32;
        desc_lo_ = uint32_t(heap_gpu_ + base_desc);
        table_lo_ = desc_lo_ + kSlotViews * 32;
        memcpy(reinterpret_cast<uint8_t*>(heap_cpu_ + base_desc + kSlotViews * 32), tex_table_, sizeof(tex_table_));
    } else {
        desc_lo_ = table_lo_ = 0;
    }
    bound_ = nullptr;
    dirty_ = false;
    if (heap_) cl_->SetDescriptorHeaps(1, &heap_);
    cl_->SetComputeRootSignature(rs_);
    return current_serial_;
}

void D3dDevice::unbind() {
    if (cl_ && !using_overflow_) {
        if (cl_ != own_ && nquery_ == 0) stamp();
        slots_[slot_].query_count = nquery_;
        if (nquery_)
            cl_->ResolveQueryData(qh_, D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kSlotQueries, nquery_, qrb_, uint64_t(slot_) * kSlotQueries * 8);
    } else if (cl_ && using_overflow_ && !overflow_.empty()) {
        auto& o = overflow_.back();
        if (o.serial == current_serial_ && o.qh && o.qrb) {
            cl_->EndQuery(o.qh, D3D12_QUERY_TYPE_TIMESTAMP, 0);
            cl_->ResolveQueryData(o.qh, D3D12_QUERY_TYPE_TIMESTAMP, 0, 1, o.qrb, 0);
        }
    }
    current_serial_ = 0;
    using_overflow_ = false;
    cur_ring_ = nullptr;
    cur_ring_map_ = nullptr;
    cl_ = nullptr;
    bound_ = nullptr;
}

bool D3dDevice::read_stamps(uint32_t slot, std::vector<uint64_t>& ts) {
    const size_t n = ts.size();
    if (!n || n > kSlotQueries || !done_map_) return false;
    slot %= kSlots;
    if (!slot_retired(slots_[slot], slot)) return false;
    const auto* m = reinterpret_cast<const uint64_t*>(const_cast<const uint64_t*>(done_map_)) + size_t(slot) * kSlotQueries;
    memcpy(ts.data(), m, n * 8);
    return ts.front() && ts.back() && ts.back() >= ts.front();
}

Buffer D3dDevice::create(VkDeviceSize size) {
    Buffer b;
    ID3D12Resource* r = buffer(dev_, size, D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_STATE_COMMON, true);
    b.res = r;
    b.addr = r->GetGPUVirtualAddress();
    b.size = size;
    bytes_ += size;
    return b;
}

void D3dDevice::destroy(Buffer& b) {
    if (b.res && bytes_ >= b.size) bytes_ -= b.size;
    bury(static_cast<ID3D12Resource*>(b.res));
    b = Buffer{};
}

// Buffers stay in COMMON between the API's own uses of them (copies, indirect arguments): the kernels reach them by
// address, which the API state does not cover. Every copy moves the buffer out of COMMON and back, after a UAV
// barrier when a kernel may still touch it.
void D3dDevice::staged(const Buffer& b, uint64_t off, uint64_t size, const void* data, uint32_t value, bool is_fill) {
    if (!cl_ || !cur_ring_ || !cur_ring_map_) throw std::runtime_error("D3dDevice: recording outside bind()");
    const uint64_t at = (ring_off_ + 255) & ~uint64_t(255);
    if (at + size > ring_end_) throw std::runtime_error("D3dDevice: upload ring slot full");
    if (is_fill)
        for (uint64_t i = 0; i < size; i += 4) memcpy(cur_ring_map_ + at + i, &value, size_t(std::min<uint64_t>(4, size - i)));
    else
        memcpy(cur_ring_map_ + at, data, size_t(size));
    ring_off_ = at + size;
    auto* r = static_cast<ID3D12Resource*>(b.res);
    if (dirty_) barrier();
    D3D12_RESOURCE_BARRIER t = transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->ResourceBarrier(1, &t);
    cl_->CopyBufferRegion(r, off, cur_ring_, at, size);
    t = transition(r, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    cl_->ResourceBarrier(1, &t);
    t = transition(r, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
    cl_->ResourceBarrier(1, &t);
}

void D3dDevice::fill(const Buffer& b, VkDeviceSize off, VkDeviceSize size, uint32_t value) { staged(b, off, size, nullptr, value, true); }

void D3dDevice::update(const Buffer& b, VkDeviceSize off, VkDeviceSize size, const void* data) { staged(b, off, size, data, 0, false); }

void D3dDevice::upload(const Buffer& b, const void* src, VkDeviceSize size, VkDeviceSize off) {
    ID3D12Resource* up = buffer(dev_, size, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ, false);
    void* p = nullptr;
    up->Map(0, nullptr, &p);
    memcpy(p, src, size_t(size));
    up->Unmap(0, nullptr);
    begin_own();
    auto* r = static_cast<ID3D12Resource*>(b.res);
    D3D12_RESOURCE_BARRIER t = transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    own_->ResourceBarrier(1, &t);
    own_->CopyBufferRegion(r, off, up, 0, size);
    std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    own_->ResourceBarrier(1, &t);
    try {
        submit_own();
    } catch (...) {
        up->Release();
        throw;
    }
    up->Release();
}

void D3dDevice::download(const Buffer& b, void* dst, VkDeviceSize size, VkDeviceSize off) {
    ID3D12Resource* rb = buffer(dev_, size, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST, false);
    begin_own();
    auto* r = static_cast<ID3D12Resource*>(b.res);
    D3D12_RESOURCE_BARRIER t = transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    own_->ResourceBarrier(1, &t);
    own_->CopyBufferRegion(rb, 0, r, off, size);
    std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    own_->ResourceBarrier(1, &t);
    try {
        submit_own();
    } catch (...) {
        rb->Release();
        throw;
    }
    void* p = nullptr;
    rb->Map(0, nullptr, &p);
    memcpy(dst, p, size_t(size));
    rb->Unmap(0, nullptr);
    rb->Release();
}

void D3dDevice::fill(const Buffer& b, uint32_t value) {
    if (value == 0) return;
    std::vector<uint32_t> v(size_t((b.size + 3) / 4), value);
    upload(b, v.data(), b.size);
}

void D3dDevice::copy_textures_to_buffer(const TexCopy* c, uint32_t n, const Buffer& dst) {
    auto* d = static_cast<ID3D12Resource*>(dst.res);
    if (dirty_) barrier();
    std::vector<D3D12_RESOURCE_BARRIER> bar{transition(d, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST)};
    for (uint32_t i = 0; i < n; ++i) {
        if (c[i].state == D3D12_RESOURCE_STATE_COPY_SOURCE) continue;
        bool seen = false;   // a texture given twice is moved once
        for (auto& x : bar) seen |= x.Transition.pResource == c[i].tex;
        if (!seen) bar.push_back(transition(c[i].tex, D3D12_RESOURCE_STATES(c[i].state), D3D12_RESOURCE_STATE_COPY_SOURCE));
    }
    cl_->ResourceBarrier(UINT(bar.size()), bar.data());
    for (uint32_t i = 0; i < n; ++i) {
        D3D12_RESOURCE_DESC rd = c[i].tex->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
        dev_->GetCopyableFootprints(&rd, 0, 1, c[i].off, &fp, nullptr, nullptr, nullptr);
        fp.Footprint.RowPitch = c[i].pitch;
        D3D12_TEXTURE_COPY_LOCATION to{d, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
        to.PlacedFootprint = fp;
        D3D12_TEXTURE_COPY_LOCATION from{c[i].tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
        from.SubresourceIndex = 0;
        cl_->CopyTextureRegion(&to, 0, 0, 0, &from, nullptr);
    }
    for (auto& x : bar) std::swap(x.Transition.StateBefore, x.Transition.StateAfter);
    cl_->ResourceBarrier(UINT(bar.size()), bar.data());
}

void D3dDevice::copy_texture_to_buffer(ID3D12Resource* tex, uint32_t state, const Buffer& dst, uint64_t off, uint32_t pitch, uint32_t x,
                                       uint32_t y, uint32_t, uint32_t) {
    TexCopy c{tex, state, off, pitch, x, y};
    copy_textures_to_buffer(&c, 1, dst);
}

void D3dDevice::copy_buffer_to_texture(const Buffer& src, uint32_t pitch, ID3D12Resource* tex, uint32_t state, uint32_t x, uint32_t y,
                                       uint32_t w, uint32_t h) {
    auto* s = static_cast<ID3D12Resource*>(src.res);
    if (dirty_) barrier();
    D3D12_RESOURCE_BARRIER bar[2] = {transition(s, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE),
                                     transition(tex, D3D12_RESOURCE_STATES(state), D3D12_RESOURCE_STATE_COPY_DEST)};
    const UINT nb = state == D3D12_RESOURCE_STATE_COPY_DEST ? 1 : 2;
    cl_->ResourceBarrier(nb, bar);
    D3D12_RESOURCE_DESC rd = tex->GetDesc();
    D3D12_TEXTURE_COPY_LOCATION from{s, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT, {}};
    from.PlacedFootprint.Footprint = {rd.Format, w, h, 1, pitch};
    D3D12_TEXTURE_COPY_LOCATION to{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {}};
    to.SubresourceIndex = 0;
    D3D12_BOX box{0, 0, 0, w, h, 1};
    cl_->CopyTextureRegion(&to, x, y, 0, &from, &box);
    for (auto& t : bar) std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    cl_->ResourceBarrier(nb, bar);
}

std::vector<uint8_t> D3dDevice::elf_of(const std::string& spv_path, uint32_t subgroup) {
    std::ifstream f(spv_path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot open " + spv_path);
    std::vector<char> spv((std::istreambuf_iterator<char>(f)), {});
    uint64_t key = fnv(spv.data(), spv.size(), fnv(&subgroup, 4));
    std::filesystem::path file;
    if (!cache_.empty()) {
        file = std::filesystem::path(cache_) / (std::filesystem::path(spv_path).stem().string() + "-" + hex(key) + ".elf");
        std::ifstream c(file, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(c)), {});
        uint64_t h = 0;
        if (d.size() > 16 && !memcmp(d.data(), kCacheMagic, 8) && (memcpy(&h, d.data() + 8, 8), h == fnv(d.data() + 16, d.size() - 16))) {
            ++hits_;
            return std::vector<uint8_t>(d.begin() + 16, d.end());
        }
    }
    std::vector<uint8_t> elf = compiler_->pipeline_elf(spv_path, subgroup);
    if (elf.empty()) throw std::runtime_error("the Vulkan driver gave no pipeline ELF for " + spv_path);
    ++misses_;
    if (!file.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(file.parent_path(), ec);
        std::filesystem::path tmp = file;
        tmp += L".tmp";
        std::ofstream o(tmp, std::ios::binary);
        uint64_t h = fnv(elf.data(), elf.size());
        o.write(kCacheMagic, 8);
        o.write(reinterpret_cast<const char*>(&h), 8);
        o.write(reinterpret_cast<const char*>(elf.data()), std::streamsize(elf.size()));
        o.close();
        if (o) std::filesystem::rename(tmp, file, ec);
        else std::filesystem::remove(tmp, ec);
    }
    return elf;
}

Kernel D3dDevice::load(const std::string& spv_path, uint32_t subgroup) {
    std::vector<uint8_t> elf = elf_of(spv_path, subgroup);
    std::vector<uint8_t> patched;
    try {
        patched = patch_elf(elf);
    } catch (const std::exception& x) {
        throw std::runtime_error(std::filesystem::path(spv_path).stem().string() + ": " + x.what());
    }
    AmdElfInfo info;
    info.elf = patched.data();
    info.size = patched.size();
    elf_threads(elf, info.threads);
    Kernel k;
    k.name = spv_path;
    k.views = std::filesystem::path(spv_path).stem().string().rfind("d3d_", 0) == 0;
    ID3D12PipelineState* pso = nullptr;
    HRESULT hr = static_cast<AmdDevice5*>(ext_)->CreateComputePipelineFromElf(&info, IID_PPV_ARGS(&pso));
    if ((FAILED(hr) || !pso) && patched.size() > 48 && (patched[48] == 0x4e || patched[48] == 0x48)) {
        patched[48] ^= (0x4e ^ 0x48);
        hr = static_cast<AmdDevice5*>(ext_)->CreateComputePipelineFromElf(&info, IID_PPV_ARGS(&pso));
    }
    check(hr, "CreateComputePipelineFromElf");
    k.pso = pso;
    return k;
}

void D3dDevice::destroy(Kernel& k) {
    bury(static_cast<ID3D12PipelineState*>(k.pso));
    k = Kernel{};
}

void D3dDevice::bind_kernel(const Kernel& k, const void* push, uint32_t push_size) {
    if (!cl_) throw std::runtime_error("D3dDevice: recording outside bind()");
    if (bound_ != &k) cl_->SetPipelineState(static_cast<ID3D12PipelineState*>(k.pso)), bound_ = &k;
    uint32_t rc[kRootDwords];
    root_constants(push, push_size, rc);
    rc[61] = k.views ? desc_lo_ : table_lo_;
    cl_->SetComputeRoot32BitConstants(0, kRootDwords, rc, 0);
}

void D3dDevice::dispatch(const Kernel& k, const void* push, uint32_t push_size, uint32_t gx, uint32_t gy, uint32_t gz) {
    bind_kernel(k, push, push_size);
    if (per_dispatch_) stamp_names.push_back(k.name.substr(k.name.find_last_of("\\/") + 1) + "@" + std::to_string(nquery_)), stamp();
    cl_->Dispatch(gx, gy, gz);
    if (per_dispatch_) stamp();
    dirty_ = true;
}

void D3dDevice::dispatch_indirect(const Kernel& k, const void* push, uint32_t push_size, const Buffer& args, VkDeviceSize off) {
    barrier();   // the arguments were written by a kernel
    bind_kernel(k, push, push_size);
    auto* r = static_cast<ID3D12Resource*>(args.res);
    D3D12_RESOURCE_BARRIER t = transition(r, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_INDIRECT_ARGUMENT);
    cl_->ResourceBarrier(1, &t);
    if (per_dispatch_) stamp_names.push_back(k.name.substr(k.name.find_last_of("\\/") + 1) + "@" + std::to_string(nquery_)), stamp();
    cl_->ExecuteIndirect(dispatch_sig_, 1, r, off, nullptr, 0);
    if (per_dispatch_) stamp();
    std::swap(t.Transition.StateBefore, t.Transition.StateAfter);
    cl_->ResourceBarrier(1, &t);
    dirty_ = true;
}

void D3dDevice::srv(ID3D12Resource* r, uint32_t index, uint32_t fmt) {
    D3D12_SHADER_RESOURCE_VIEW_DESC d{};
    d.Format = DXGI_FORMAT(fmt);
    d.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    d.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    d.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(r, &d, {size_t(heap_cpu_ + (uint64_t(desc_slot_) * kSlotStride + index) * 32)});
}

void D3dDevice::uav(ID3D12Resource* r, uint32_t index, uint32_t fmt) {
    D3D12_UNORDERED_ACCESS_VIEW_DESC d{};
    d.Format = DXGI_FORMAT(fmt);
    d.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    dev_->CreateUnorderedAccessView(r, nullptr, &d, {size_t(heap_cpu_ + (uint64_t(desc_slot_) * kSlotStride + index) * 32)});
}

// The descriptor of a linear w x h image at address a: from the Vulkan device of its own, over a buffer of its own.
// The address is read back from where init_sampler found it.
static bool linear_descriptor(Device& d, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp, uint8_t out[48], uint64_t& at) {
    Buffer b = d.create(VkDeviceSize(w) * h * bpp + 65536);
    Image im = d.alias_image(b, w, h, fmt, bpp);
    VkSampler s = d.sampler(true, true);
    const bool ok = im.img && d.combined_descriptor(im, s, out);
    at = b.addr;
    d.destroy(s);
    d.destroy(im);
    d.destroy(b);
    return ok;
}

static void put_address(uint8_t* srd, uint64_t a) {
    const uint32_t lo = uint32_t(a >> 8);
    memcpy(srd, &lo, 4);
    srd[4] = uint8_t(a >> 40);
}

bool D3dDevice::init_sampler(std::string& why) {
    srd_ok_ = false;
    if (!heap_) return why = "no descriptor table", false;
    // the heap is written by the CPU at its CPU handle: it must be plain mapped memory
    MEMORY_BASIC_INFORMATION mi{};
    if (!VirtualQuery(reinterpret_cast<void*>(heap_cpu_), &mi, sizeof mi) || mi.State != MEM_COMMIT ||
        !(mi.Protect & (PAGE_READWRITE | PAGE_WRITECOMBINE | PAGE_NOCACHE)))
        return why = "the descriptor heap is not CPU-writable memory", false;
    Device* desc_dev = (compiler_ && compiler_->has_descriptors()) ? compiler_ : nullptr;
    if (!desc_dev) {
        srd_ = std::make_unique<Device>();
        LUID luid = dev_->GetAdapterLuid();
        DeviceOptions o;
        o.luid = reinterpret_cast<const uint8_t*>(&luid);
        o.descriptors = true;
        std::string err;
        if (!srd_->init(err, o) || !srd_->has_descriptors())
            return srd_.reset(), why = "Vulkan descriptor device: " + (err.empty() ? "lacks VK_EXT_descriptor_buffer" : err), false;
        desc_dev = srd_.get();
    }
    // two images at two addresses: only the base address may differ, at the bits it is expected at
    uint8_t d[2][48];
    uint64_t a[2];
    for (int i = 0; i < 2; ++i)
        if (!linear_descriptor(*desc_dev, 64, 8, VK_FORMAT_R16G16B16A16_SFLOAT, 8, d[i], a[i]))
            return srd_.reset(), why = "no linear image descriptor from Vulkan", false;
    for (int i = 0; i < 2; ++i) {
        uint8_t t[48];
        memcpy(t, d[i], 48);
        put_address(t, a[i]);
        if (memcmp(t, d[i], 48)) return srd_.reset(), why = "the image descriptor keeps its address elsewhere", false;
        put_address(t, a[i ^ 1]);
        if (memcmp(t, d[i ^ 1], 48)) return srd_.reset(), why = "two image descriptors differ past their address", false;
    }
    srd_ok_ = true;
    return true;
}

Image D3dDevice::alias_image(const Buffer& b, uint32_t w, uint32_t h, VkFormat fmt, uint32_t bpp) {
    if (!srd_ok_) return {};
    std::array<uint8_t, 48> t;
    uint64_t a = 0;
    Device* desc_dev = (compiler_ && compiler_->has_descriptors()) ? compiler_ : srd_.get();
    if (!desc_dev || !linear_descriptor(*desc_dev, w, h, fmt, bpp, t.data(), a)) return {};
    put_address(t.data(), b.addr);
    Image im;
    im.img = reinterpret_cast<VkImage>(uintptr_t(next_image_));
    im.w = w, im.h = h;
    images_[next_image_++] = t;
    return im;
}

void D3dDevice::destroy(Image& i) {
    images_.erase(uint64_t(reinterpret_cast<uintptr_t>(i.img)));
    i = Image{};
}

// the only sampler the table holds is the one linear_descriptor makes (linear, unnormalized, clamp)
VkSampler D3dDevice::sampler(bool linear, bool unnormalized) {
    return linear && unnormalized ? reinterpret_cast<VkSampler>(uintptr_t(1)) : VK_NULL_HANDLE;
}

void D3dDevice::set_texture(uint32_t slot, const Image& i, VkSampler s) {
    auto it = images_.find(uint64_t(reinterpret_cast<uintptr_t>(i.img)));
    if (it == images_.end() || slot >= 8 || s != reinterpret_cast<VkSampler>(uintptr_t(1)))
        throw std::runtime_error("D3dDevice::set_texture: no such image or sampler");
    memcpy(tex_table_[slot], it->second.data(), 48);
    if (cl_ && heap_)
        memcpy(reinterpret_cast<uint8_t*>(heap_cpu_ + (uint64_t(desc_slot_) * kSlotStride + kSlotViews) * 32 + slot * 48),
               it->second.data(), 48);
}

void D3dDevice::barrier() {
    D3D12_RESOURCE_BARRIER u{};
    u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
    cl_->ResourceBarrier(1, &u);
    dirty_ = false;
}

void D3dDevice::stamp() {
    if (cl_ && !using_overflow_ && nquery_ < kSlotQueries)
        cl_->EndQuery(qh_, D3D12_QUERY_TYPE_TIMESTAMP, slot_ * kSlotQueries + nquery_++);
}

bool D3dDevice::vram(uint64_t& used, uint64_t& budget) const {
    if (!dev_) return false;
    IDXGIFactory4* fac = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac)))) return false;
    IDXGIAdapter3* ad3 = nullptr;
    LUID luid = dev_->GetAdapterLuid();
    bool ok = false;
    if (SUCCEEDED(fac->EnumAdapterByLuid(luid, IID_PPV_ARGS(&ad3)))) {
        DXGI_QUERY_VIDEO_MEMORY_INFO info{};
        if (SUCCEEDED(ad3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
            used = info.CurrentUsage;
            budget = info.Budget;
            ok = true;
        }
        ad3->Release();
    }
    fac->Release();
    return ok;
}

}  // namespace onr
