// SPDX-License-Identifier: MIT
#pragma once

#include <windows.h>
#include <DirectXMath.h>
#include <detours.h>
#include <nvsdk_ngx_params.h>
#include <nvsdk_ngx_defs_dlssd.h>
#include <sl.h>
#include <sl_consts.h>

#include <atomic>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>

namespace openrr_asi {

class SlCameraTracker {
public:
    static SlCameraTracker& Instance() {
        static auto* inst = new SlCameraTracker();
        return *inst;
    }

    void TryHookLoadedModule() {
        HMODULE mod = GetModuleHandleW(L"sl.interposer.dll");
        if (!mod) return;
        TryHookModule(mod);
    }

    void TryHookModule(HMODULE mod) {
        if (!mod) return;
        std::lock_guard lock(mu_);
        if (hookedMod_ == mod) return;
        auto* fn = reinterpret_cast<PFN_slSetConstants>(GetProcAddress(mod, "slSetConstants"));
        if (!fn) return;

        o_slSetConstants_ = fn;
        DetourTransactionBegin();
        DetourUpdateThread(GetCurrentThread());
        DetourAttach(reinterpret_cast<PVOID*>(&o_slSetConstants_), &HookedSlSetConstants);
        if (DetourTransactionCommit() == NO_ERROR) {
            hookedMod_ = mod;
            hooked_.store(true, std::memory_order_release);
        } else {
            o_slSetConstants_ = nullptr;
        }
    }

    bool GetSnapshot(sl::Constants& out) const {
        if (!hasConstants_.load(std::memory_order_acquire))
            return false;
        std::lock_guard lock(snapMu_);
        out = lastConstants_;
        return true;
    }

private:
    using PFN_slSetConstants = sl::Result(*)(const sl::Constants&, const sl::FrameToken&, const sl::ViewportHandle&);

    static sl::Result HookedSlSetConstants(const sl::Constants& values, const sl::FrameToken& frame, const sl::ViewportHandle& viewport) {
        auto& self = Instance();
        {
            std::lock_guard lock(self.snapMu_);
            self.lastConstants_ = values;
            self.hasConstants_.store(true, std::memory_order_release);
        }
        return self.o_slSetConstants_ ? self.o_slSetConstants_(values, frame, viewport) : sl::Result::eOk;
    }

    std::mutex mu_;
    HMODULE hookedMod_ = nullptr;
    PFN_slSetConstants o_slSetConstants_ = nullptr;
    std::atomic<bool> hooked_{false};

    mutable std::mutex snapMu_;
    sl::Constants lastConstants_{};
    std::atomic<bool> hasConstants_{false};
};

namespace camera {

inline bool IsInitialized(float v) {
    return std::isfinite(v) && v != sl::INVALID_FLOAT;
}

inline bool MatrixIsInitialized(const DirectX::XMMATRIX& matrix) {
    DirectX::XMFLOAT4X4 values;
    DirectX::XMStoreFloat4x4(&values, matrix);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            if (!IsInitialized(values.m[r][c]))
                return false;
    return true;
}

inline bool TryInvert(const DirectX::XMMATRIX& matrix, DirectX::XMMATRIX& inverse) {
    if (!MatrixIsInitialized(matrix))
        return false;
    const float det = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(matrix));
    if (!std::isfinite(det) || det == 0.0f)
        return false;
    const DirectX::XMMATRIX cand = DirectX::XMMatrixInverse(nullptr, matrix);
    if (!MatrixIsInitialized(cand))
        return false;
    inverse = cand;
    return true;
}

inline DirectX::XMMATRIX StreamlineProjection(const sl::float4x4& m) {
    const DirectX::XMMATRIX rows = DirectX::XMMatrixSet(
        m[0].x, m[0].y, m[0].z, m[0].w,
        m[1].x, m[1].y, m[1].z, m[1].w,
        m[2].x, m[2].y, m[2].z, m[2].w,
        m[3].x, m[3].y, m[3].z, m[3].w);
    return DirectX::XMMatrixTranspose(rows);
}

inline bool TryHandedness(float clipWFromViewZ, bool& isRightHanded) {
    if (!IsInitialized(clipWFromViewZ) || clipWFromViewZ == 0.0f)
        return false;
    isRightHanded = clipWFromViewZ < 0.0f;
    return true;
}

inline bool TryScalarProjection(const sl::Constants& cam, bool depthInverted, DirectX::XMMATRIX& projection) {
    bool isRightHanded = false;
    if (!IsInitialized(cam.cameraFOV) || !IsInitialized(cam.cameraAspectRatio) ||
        !IsInitialized(cam.cameraNear) || !IsInitialized(cam.cameraFar) ||
        cam.orthographicProjection == sl::Boolean::eTrue ||
        !TryHandedness(cam.cameraViewToClip[2].w, isRightHanded))
        return false;

    const float fov = cam.cameraFOV < 4.0f ? cam.cameraFOV : cam.cameraFOV * (DirectX::XM_PI / 180.0f);
    const float nearPlane = cam.cameraNear;
    const float farPlane = cam.cameraFar;
    if (!(fov > 0.0f && fov < DirectX::XM_PI) || cam.cameraAspectRatio <= 0.0f ||
        nearPlane <= 0.0f || (farPlane != 0.0f && farPlane <= nearPlane))
        return false;

    DirectX::XMMATRIX rows;
    if (farPlane == 0.0f) {
        const float yScale = 1.0f / std::tan(fov * 0.5f);
        const float xScale = yScale / cam.cameraAspectRatio;
        const float W = isRightHanded ? -1.0f : 1.0f;
        const float A = depthInverted ? 0.0f : W;
        const float B = depthInverted ? nearPlane : -nearPlane;
        rows = DirectX::XMMatrixSet(
            xScale, 0.0f,   0.0f, 0.0f,
            0.0f,   yScale, 0.0f, 0.0f,
            0.0f,   0.0f,   A,    W,
            0.0f,   0.0f,   B,    0.0f);
    } else {
        const float matrixNear = depthInverted ? farPlane : nearPlane;
        const float matrixFar = depthInverted ? nearPlane : farPlane;
        rows = isRightHanded
            ? DirectX::XMMatrixPerspectiveFovRH(fov, cam.cameraAspectRatio, matrixNear, matrixFar)
            : DirectX::XMMatrixPerspectiveFovLH(fov, cam.cameraAspectRatio, matrixNear, matrixFar);
    }

    const DirectX::XMMATRIX cand = DirectX::XMMatrixTranspose(rows);
    DirectX::XMMATRIX inv;
    if (!TryInvert(cand, inv))
        return false;
    projection = cand;
    return true;
}

inline bool VectorIsInitialized(const sl::float3& v) {
    return IsInitialized(v.x) && IsInitialized(v.y) && IsInitialized(v.z);
}

inline bool TryBasisView(const sl::Constants& cam, bool isRightHanded, DirectX::XMMATRIX& view) {
    if (!VectorIsInitialized(cam.cameraRight) || !VectorIsInitialized(cam.cameraUp) ||
        !VectorIsInitialized(cam.cameraFwd) || !VectorIsInitialized(cam.cameraPos))
        return false;

    const float forwardSign = isRightHanded ? -1.0f : 1.0f;
    const DirectX::XMMATRIX cand = DirectX::XMMatrixSet(
        cam.cameraRight.x, cam.cameraUp.x, forwardSign * cam.cameraFwd.x, cam.cameraPos.x,
        cam.cameraRight.y, cam.cameraUp.y, forwardSign * cam.cameraFwd.y, cam.cameraPos.y,
        cam.cameraRight.z, cam.cameraUp.z, forwardSign * cam.cameraFwd.z, cam.cameraPos.z,
        0.0f,              0.0f,           0.0f,                          1.0f);
    DirectX::XMMATRIX candView;
    if (!TryInvert(cand, candView))
        return false;
    view = candView;
    return true;
}

inline bool ReadNgxMatrix(const NVSDK_NGX_Parameter& params, const char* key, float (&out)[16]) {
    float* matrix = nullptr;
    if (params.Get(key, reinterpret_cast<void**>(&matrix)) != NVSDK_NGX_Result_Success || matrix == nullptr)
        return false;
    bool nonZero = false;
    for (int i = 0; i < 16; ++i) {
        if (!IsInitialized(matrix[i]))
            return false;
        nonZero = nonZero || matrix[i] != 0.0f;
    }
    if (!nonZero)
        return false;
    std::memcpy(out, matrix, sizeof(out));
    return true;
}

inline void ResolveCameraMatrices(const NVSDK_NGX_Parameter& params, bool depthInverted, float (&w2v)[16], float (&v2c)[16]) {
    const bool hasW2V = ReadNgxMatrix(params, NVSDK_NGX_Parameter_DLSS_WORLD_TO_VIEW_MATRIX, w2v);
    const bool hasV2C = ReadNgxMatrix(params, NVSDK_NGX_Parameter_DLSS_VIEW_TO_CLIP_MATRIX, v2c);

    DirectX::XMMATRIX ngxView{};
    DirectX::XMMATRIX ngxProj{};
    DirectX::XMMATRIX inv{};
    if (hasW2V)
        ngxView = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(w2v)));
    if (hasV2C)
        ngxProj = DirectX::XMMatrixTranspose(DirectX::XMLoadFloat4x4(reinterpret_cast<const DirectX::XMFLOAT4X4*>(v2c)));

    const bool validW2V = hasW2V && TryInvert(ngxView, inv);
    const bool validV2C = hasV2C && TryInvert(ngxProj, inv);
    if (validW2V && validV2C)
        return;

    SlCameraTracker::Instance().TryHookLoadedModule();
    sl::Constants slSnap{};
    if (!SlCameraTracker::Instance().GetSnapshot(slSnap))
        return;

    DirectX::XMMATRIX resolvedProj{};
    bool hasProj = false;
    if (validV2C) {
        resolvedProj = ngxProj;
        hasProj = true;
    } else {
        const DirectX::XMMATRIX fullProj = StreamlineProjection(slSnap.cameraViewToClip);
        if (TryInvert(fullProj, inv)) {
            resolvedProj = fullProj;
            hasProj = true;
        } else if (TryScalarProjection(slSnap, depthInverted, resolvedProj)) {
            hasProj = true;
        }
    }

    if (!validV2C && hasProj) {
        const DirectX::XMMATRIX rows = DirectX::XMMatrixTranspose(resolvedProj);
        DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(v2c), rows);
    }

    if (!validW2V && hasProj) {
        DirectX::XMFLOAT4X4 pm{};
        DirectX::XMStoreFloat4x4(&pm, resolvedProj);
        bool isRightHanded = false;
        DirectX::XMMATRIX resolvedView{};
        if (TryHandedness(pm.m[3][2], isRightHanded) && TryBasisView(slSnap, isRightHanded, resolvedView)) {
            const DirectX::XMMATRIX rows = DirectX::XMMatrixTranspose(resolvedView);
            DirectX::XMStoreFloat4x4(reinterpret_cast<DirectX::XMFLOAT4X4*>(w2v), rows);
        }
    }
}

} // namespace camera
} // namespace openrr_asi
