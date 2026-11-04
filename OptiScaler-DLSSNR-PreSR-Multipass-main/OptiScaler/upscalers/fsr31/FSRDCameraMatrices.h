#pragma once

#include <DirectXMath.h>
#include <sl_consts.h>
#include <cmath>
#include <limits>

namespace FSRDCamera
{
enum class Source
{
    Missing,
    NGX,
    StreamlineMatrix,
    StreamlineScalars,
    StreamlineBasis,
};

// Physical camera metadata is distinct from RR's finite horizon for an infinite view.
struct Planes
{
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    bool farInfinite = false;
    bool valid = false;

    float DenoiserFar() const { return farInfinite ? 65504.0f : farPlane; }
};

struct PlaneOverrides
{
    float nearPlane = 0.0f;
    float farPlane = 0.0f;
    bool hasNear = false;
    bool hasFar = false;
};

struct Matrices
{
    DirectX::XMMATRIX view {};
    DirectX::XMMATRIX inverseView {};
    DirectX::XMMATRIX projection {};
    Source viewSource = Source::Missing;
    Source projectionSource = Source::Missing;
    bool isRightHanded = false;
    Planes planes;

    bool Complete() const { return viewSource != Source::Missing && projectionSource != Source::Missing; }
};

inline bool IsInitialized(float value)
{
    return std::isfinite(value) && value != sl::INVALID_FLOAT;
}

inline bool MatrixIsInitialized(const DirectX::XMMATRIX& matrix)
{
    DirectX::XMFLOAT4X4 values;
    DirectX::XMStoreFloat4x4(&values, matrix);
    for (int row = 0; row < 4; ++row)
        for (int column = 0; column < 4; ++column)
            if (!IsInitialized(values.m[row][column]))
                return false;
    return true;
}

// Both published and rebuilt matrices must survive every consumer's inversion.
// A finite sentinel fill alone does not establish that camera data was supplied.
inline bool TryInvert(const DirectX::XMMATRIX& matrix, DirectX::XMMATRIX& inverse)
{
    if (!MatrixIsInitialized(matrix))
        return false;
    const float determinant = DirectX::XMVectorGetX(DirectX::XMMatrixDeterminant(matrix));
    if (!std::isfinite(determinant) || determinant == 0.0f)
        return false;
    const DirectX::XMMATRIX candidate = DirectX::XMMatrixInverse(nullptr, matrix);
    if (!MatrixIsInitialized(candidate))
        return false;
    inverse = candidate;
    return true;
}

inline DirectX::XMMATRIX StreamlineProjection(const sl::float4x4& matrix)
{
    // sl_consts.h stores rows; sl_matrix_helpers.h composes row-vector transforms
    // as clipToView * viewToViewPrev * viewToClipPrev. Internally we use columns.
    const DirectX::XMMATRIX rows = DirectX::XMMatrixSet(
        matrix[0].x, matrix[0].y, matrix[0].z, matrix[0].w,
        matrix[1].x, matrix[1].y, matrix[1].z, matrix[1].w,
        matrix[2].x, matrix[2].y, matrix[2].z, matrix[2].w,
        matrix[3].x, matrix[3].y, matrix[3].z, matrix[3].w);
    return DirectX::XMMatrixTranspose(rows);
}

inline bool TryHandedness(float clipWFromViewZ, bool& isRightHanded)
{
    // clip.w = W * view.z for the perspective cameras the denoiser consumes.
    // W's sign is independent of standard/reversed depth. Missing/sentinel W
    // cannot select a convention for a scalar projection or a camera basis.
    if (!IsInitialized(clipWFromViewZ) || clipWFromViewZ == 0.0f)
        return false;
    isRightHanded = clipWFromViewZ < 0.0f;
    return true;
}

inline Planes ResolvePlanes(const DirectX::XMMATRIX& projection,
                            const sl::Constants* camera, const PlaneOverrides& overrides = {})
{
    Planes result;
    DirectX::XMFLOAT4X4 m;
    DirectX::XMStoreFloat4x4(&m, projection);
    const double A = m.m[2][2], B = m.m[2][3], W = m.m[3][2];
    // This pipeline reconstructs perspective positions, not orthographic ones.
    if (!MatrixIsInitialized(projection) || W == 0.0 || m.m[3][0] != 0.0f ||
        m.m[3][1] != 0.0f || m.m[3][3] != 0.0f || m.m[1][1] == 0.0f)
        return result;

    const double direction = W < 0.0 ? -1.0 : 1.0;
    // Full projection matrices encode their own depth direction. In particular,
    // linear NGX depth may omit DepthInverted while publishing reverse-Z.
    const bool depthInverted = B > 0.0;
    const double nearDenominator = depthInverted ? W - A : -A;
    const double farDenominator = depthInverted ? -A : W - A;
    const double nearPlane = direction * B / nearDenominator;
    const double farPlane = direction * B / farDenominator;
    result.nearPlane = static_cast<float>(nearPlane);
    result.farPlane = static_cast<float>(farPlane);
    // Never take abs(root): a far root behind the camera is not a physical plane.
    // Cyberpunk's effectively infinite reverse-Z projection has noisy A=-1.19e-6.
    // Accept that small WRONG-SIGN root as infinite, but a positive finite root
    // remains finite however small A/W is (e.g. Godot RH .05/800).
    result.farInfinite = farDenominator == 0.0 ||
        (farPlane < 0.0 && std::abs(farDenominator / W) < 1e-4);

    // Only the caller's current frame/viewport-correlated SL snapshot is accepted.
    // A valid scalar pair is authoritative even when a matrix's far term is noisy.
    if (camera && IsInitialized(camera->cameraNear) && IsInitialized(camera->cameraFar) &&
        camera->cameraNear > 0.0f &&
        (camera->cameraFar == 0.0f || camera->cameraFar > camera->cameraNear))
    {
        result.nearPlane = camera->cameraNear;
        result.farPlane = camera->cameraFar;
        result.farInfinite = camera->cameraFar == 0.0f;
    }
    // Match SR's established OptiKeys/INI precedence, without using SR defaults as metadata.
    if (overrides.hasNear)
        result.nearPlane = overrides.nearPlane;
    if (overrides.hasFar)
    {
        result.farPlane = overrides.farPlane;
        result.farInfinite = overrides.farPlane == std::numeric_limits<float>::max();
    }
    if (result.farInfinite)
        result.farPlane = std::numeric_limits<float>::max();
    result.valid = std::isfinite(result.nearPlane) && result.nearPlane > 0.0f &&
        std::isfinite(result.farPlane) && result.farPlane > result.nearPlane;
    return result;
}

inline bool TryScalarProjection(const sl::Constants& camera, bool depthInverted,
                                DirectX::XMMATRIX& projection)
{
    bool isRightHanded = false;
    if (!IsInitialized(camera.cameraFOV) || !IsInitialized(camera.cameraAspectRatio) ||
        !IsInitialized(camera.cameraNear) || !IsInitialized(camera.cameraFar) ||
        camera.orthographicProjection == sl::Boolean::eTrue ||
        !TryHandedness(camera.cameraViewToClip[2].w, isRightHanded))
        return false;

    // Retain support for titles that report degrees despite Streamline's radians contract.
    const float fov = camera.cameraFOV < 4.0f
        ? camera.cameraFOV : camera.cameraFOV * (DirectX::XM_PI / 180.0f);
    const float nearPlane = camera.cameraNear;
    const float farPlane = camera.cameraFar;
    if (!(fov > 0.0f && fov < DirectX::XM_PI) || camera.cameraAspectRatio <= 0.0f ||
        nearPlane <= 0.0f || (farPlane != 0.0f && farPlane <= nearPlane))
        return false;

    DirectX::XMMATRIX rows;
    if (farPlane == 0.0f)
    {
        const float yScale = 1.0f / std::tan(fov * 0.5f);
        const float xScale = yScale / camera.cameraAspectRatio;
        const float W = isRightHanded ? -1.0f : 1.0f;
        const float A = depthInverted ? 0.0f : W;
        const float B = depthInverted ? nearPlane : -nearPlane;
        rows = DirectX::XMMatrixSet(
            xScale, 0.0f,   0.0f, 0.0f,
            0.0f,   yScale, 0.0f, 0.0f,
            0.0f,   0.0f,   A,    W,
            0.0f,   0.0f,   B,    0.0f);
    }
    else
    {
        const float matrixNear = depthInverted ? farPlane : nearPlane;
        const float matrixFar = depthInverted ? nearPlane : farPlane;
        rows = isRightHanded
            ? DirectX::XMMatrixPerspectiveFovRH(fov, camera.cameraAspectRatio, matrixNear, matrixFar)
            : DirectX::XMMatrixPerspectiveFovLH(fov, camera.cameraAspectRatio, matrixNear, matrixFar);
    }

    const DirectX::XMMATRIX candidate = DirectX::XMMatrixTranspose(rows);
    DirectX::XMMATRIX inverse;
    if (!TryInvert(candidate, inverse))
        return false;
    projection = candidate;
    return true;
}

inline bool VectorIsInitialized(const sl::float3& vector)
{
    return IsInitialized(vector.x) && IsInitialized(vector.y) && IsInitialized(vector.z);
}

inline bool TryBasisView(const sl::Constants& camera, bool isRightHanded,
                        DirectX::XMMATRIX& view, DirectX::XMMATRIX& inverseView)
{
    if (!VectorIsInitialized(camera.cameraRight) || !VectorIsInitialized(camera.cameraUp) ||
        !VectorIsInitialized(camera.cameraFwd) || !VectorIsInitialized(camera.cameraPos))
        return false;

    // cameraFwd is a world-space direction. RH view space looks down -Z, so its
    // inverse view's +Z axis is -cameraFwd; LH view space uses +cameraFwd.
    const float forwardSign = isRightHanded ? -1.0f : 1.0f;
    const DirectX::XMMATRIX candidate = DirectX::XMMatrixSet(
        camera.cameraRight.x, camera.cameraUp.x, forwardSign * camera.cameraFwd.x, camera.cameraPos.x,
        camera.cameraRight.y, camera.cameraUp.y, forwardSign * camera.cameraFwd.y, camera.cameraPos.y,
        camera.cameraRight.z, camera.cameraUp.z, forwardSign * camera.cameraFwd.z, camera.cameraPos.z,
        0.0f,                0.0f,              0.0f,                            1.0f);
    DirectX::XMMATRIX candidateView;
    if (!TryInvert(candidate, candidateView))
        return false;
    inverseView = candidate;
    view = candidateView;
    return true;
}

inline Matrices Resolve(const DirectX::XMMATRIX* ngxView, const DirectX::XMMATRIX* ngxProjection,
                        const sl::Constants* camera, bool depthInverted,
                        const PlaneOverrides& overrides = {})
{
    Matrices result;
    DirectX::XMMATRIX inverseProjection;

    // Resolve projection first: its convention determines the fallback view's Z axis.
    if (ngxProjection && TryInvert(*ngxProjection, inverseProjection))
    {
        result.projection = *ngxProjection;
        result.projectionSource = Source::NGX;
    }
    else if (camera)
    {
        const DirectX::XMMATRIX fullProjection = StreamlineProjection(camera->cameraViewToClip);
        if (TryInvert(fullProjection, inverseProjection))
        {
            // Preserve all terms, including asymmetric/off-axis frustum offsets.
            result.projection = fullProjection;
            result.projectionSource = Source::StreamlineMatrix;
        }
        else if (TryScalarProjection(*camera, depthInverted, result.projection))
            result.projectionSource = Source::StreamlineScalars;
    }

    bool hasConvention = false;
    if (result.projectionSource != Source::Missing)
    {
        DirectX::XMFLOAT4X4 projection;
        DirectX::XMStoreFloat4x4(&projection, result.projection);
        hasConvention = TryHandedness(projection.m[3][2], result.isRightHanded);
        result.planes = ResolvePlanes(result.projection, camera, overrides);
    }

    if (ngxView && TryInvert(*ngxView, result.inverseView))
    {
        // Do not rebuild or adjust a valid title-supplied view.
        result.view = *ngxView;
        result.viewSource = Source::NGX;
    }
    else if (camera && hasConvention &&
             TryBasisView(*camera, result.isRightHanded, result.view, result.inverseView))
        result.viewSource = Source::StreamlineBasis;

    return result;
}
} // namespace FSRDCamera
