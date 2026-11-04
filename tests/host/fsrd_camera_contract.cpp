// Consumer invariants adapted from burak113/OptiScaler 5a6adb10601fd16d62504febba850d8839e8f5fe
// fsrd_camera_matrices_test.cpp. Execute the production resolver, not extracted source.
#include "OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/upscalers/fsr31/FSRDCameraMatrices.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include <limits>

#define CHECK(condition) do { if (!(condition)) { \
    std::fprintf(stderr, "fsrd_camera_contract:%d: %s\n", __LINE__, #condition); \
    std::exit(1); } } while (false)

using namespace DirectX;
using namespace FSRDCamera;

static bool Near(float a, float b, float tolerance = 0.00002f)
{
    return std::isfinite(a) && std::isfinite(b) && std::abs(a - b) <= tolerance;
}

static bool Exact(const XMMATRIX& a, const XMMATRIX& b)
{
    XMFLOAT4X4 av, bv;
    XMStoreFloat4x4(&av, a);
    XMStoreFloat4x4(&bv, b);
    return std::memcmp(&av, &bv, sizeof(av)) == 0;
}

// Production matrices multiply column vectors. This independent scalar consumer
// catches row/column mistakes rather than undoing them with another helper call.
static XMFLOAT4 Transform(const XMMATRIX& matrix, float x, float y, float z)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, matrix);
    return {m._11*x + m._12*y + m._13*z + m._14,
            m._21*x + m._22*y + m._23*z + m._24,
            m._31*x + m._32*y + m._33*z + m._34,
            m._41*x + m._42*y + m._43*z + m._44};
}

static float Depth(const XMMATRIX& projection, float z)
{
    const auto clip = Transform(projection, 0, 0, z);
    CHECK(clip.w > 0);
    return clip.z / clip.w;
}

static void SetProjection(sl::Constants& camera, const XMMATRIX& rows)
{
    XMFLOAT4X4 m;
    XMStoreFloat4x4(&m, rows);
    for (unsigned row = 0; row < 4; ++row)
        camera.cameraViewToClip[row] = sl::float4(m.m[row][0], m.m[row][1], m.m[row][2], m.m[row][3]);
}

static sl::Constants Scalars(bool rh)
{
    sl::Constants camera;
    camera.cameraRight = sl::float3(1, 0, 0);
    camera.cameraUp = sl::float3(0, 1, 0);
    camera.cameraFwd = sl::float3(0, 0, rh ? -1.0f : 1.0f);
    camera.cameraPos = sl::float3(0, 0, 0);
    // Remaining matrix entries retain Streamline's finite INVALID_FLOAT sentinel.
    camera.cameraViewToClip[2].w = rh ? -1.0f : 1.0f;
    camera.cameraFOV = 1;
    camera.cameraAspectRatio = 1.6f;
    camera.cameraNear = .1f;
    camera.cameraFar = 100;
    return camera;
}

static void BasisAndPrecedence()
{
    for (bool rh : {false, true})
    {
        auto camera = Scalars(rh);
        // Rotated and translated world camera: forward is +X for either convention.
        camera.cameraRight = sl::float3(0, 0, rh ? 1.0f : -1.0f);
        camera.cameraFwd = sl::float3(1, 0, 0);
        camera.cameraPos = sl::float3(2, 3, 4);
        const auto resolved = Resolve(nullptr, nullptr, &camera, false);
        CHECK(resolved.Complete() && resolved.isRightHanded == rh);
        CHECK(resolved.viewSource == Source::StreamlineBasis);
        const auto world = Transform(resolved.inverseView, 0, 0, rh ? -5.0f : 5.0f);
        CHECK(Near(world.x, 7) && Near(world.y, 3) && Near(world.z, 4));
        const auto view = Transform(resolved.view, world.x, world.y, world.z);
        CHECK(Near(view.x, 0) && Near(view.y, 0) && Near(view.z, rh ? -5.0f : 5.0f));
        const auto origin = Transform(resolved.view, 2, 3, 4);
        CHECK(Near(origin.x, 0) && Near(origin.y, 0) && Near(origin.z, 0));
    }

    auto camera = Scalars(true);
    const XMMATRIX ngxView = XMMatrixTranspose(XMMatrixLookAtRH(
        XMVectorSet(7, 11, 13, 1), XMVectorSet(5, 10, 6, 1), XMVectorSet(0, 1, 0, 0)));
    const XMMATRIX ngxProjection = XMMatrixTranspose(XMMatrixPerspectiveFovLH(.8f, 1.9f, .2f, 500));
    const auto ngx = Resolve(&ngxView, &ngxProjection, &camera, true);
    CHECK(ngx.Complete() && ngx.viewSource == Source::NGX && ngx.projectionSource == Source::NGX);
    CHECK(Exact(ngx.view, ngxView) && Exact(ngx.projection, ngxProjection));
    CHECK(!ngx.isRightHanded); // Selected NGX projection, not SL basis or reversed-depth flag.
    const auto mixed = Resolve(nullptr, &ngxProjection, &camera, false);
    CHECK(mixed.Complete() && !mixed.isRightHanded);
    CHECK(mixed.viewSource == Source::StreamlineBasis && mixed.projectionSource == Source::NGX);
    CHECK(Near(Transform(mixed.inverseView, 0, 0, 5).z, -5));
    const sl::Constants absent;
    CHECK(Resolve(&ngxView, &ngxProjection, &absent, false).Complete());
    CHECK(Resolve(&ngxView, &ngxProjection, nullptr, false).Complete());
    const auto onlyView = Resolve(&ngxView, nullptr, nullptr, false);
    CHECK(!onlyView.Complete() && onlyView.viewSource == Source::NGX);
}

static void FullOffAxisProjection()
{
    for (bool rh : {false, true})
        for (bool reversed : {false, true})
        {
            auto camera = Scalars(rh);
            const float n = reversed ? 100.0f : .1f;
            const float f = reversed ? .1f : 100.0f;
            const XMMATRIX rows = rh
                ? XMMatrixPerspectiveOffCenterRH(-.13f, .27f, -.2f, .12f, n, f)
                : XMMatrixPerspectiveOffCenterLH(-.13f, .27f, -.2f, .12f, n, f);
            SetProjection(camera, rows);
            // Contradictory valid scalar measurements must not symmetrize the full matrix.
            camera.cameraFOV = .5f;
            camera.cameraAspectRatio = 2.4f;
            camera.cameraNear = .2f;
            camera.cameraFar = 300;
            const auto resolved = Resolve(nullptr, nullptr, &camera, reversed);
            CHECK(resolved.Complete() && resolved.isRightHanded == rh);
            CHECK(resolved.projectionSource == Source::StreamlineMatrix);
            CHECK(Exact(resolved.projection, XMMatrixTranspose(rows)));
            const float sign = rh ? -1.0f : 1.0f;
            XMFLOAT4 expected;
            XMStoreFloat4(&expected, XMVector4Transform(XMVectorSet(.3f, -.2f, sign * 5, 1), rows));
            const auto actual = Transform(resolved.projection, .3f, -.2f, sign * 5);
            CHECK(Near(actual.x, expected.x) && Near(actual.y, expected.y));
            CHECK(Near(actual.z, expected.z) && Near(actual.w, expected.w));
            CHECK(Near(Depth(resolved.projection, sign * .1f), reversed ? 1.0f : 0.0f));
            CHECK(Near(Depth(resolved.projection, sign * 100), reversed ? 0.0f : 1.0f));
            camera.cameraFOV = sl::INVALID_FLOAT;
            CHECK(Exact(Resolve(nullptr, nullptr, &camera, reversed).projection, resolved.projection));
        }
}

static void ScalarDepthConventions()
{
    for (bool rh : {false, true})
        for (bool reversed : {false, true})
            for (bool infinite : {false, true})
            {
                auto camera = Scalars(rh);
                if (infinite) camera.cameraFar = 0;
                const auto resolved = Resolve(nullptr, nullptr, &camera, reversed);
                CHECK(resolved.Complete() && resolved.isRightHanded == rh);
                CHECK(resolved.projectionSource == Source::StreamlineScalars);
                const float sign = rh ? -1.0f : 1.0f;
                CHECK(Near(Depth(resolved.projection, sign * .1f), reversed ? 1.0f : 0.0f));
                CHECK(Near(Depth(resolved.projection, sign * (infinite ? 1e8f : 100)), reversed ? 0.0f : 1.0f));
                const auto clip = Transform(resolved.projection, 1, 1, sign * 5);
                CHECK(Near(clip.y, 1.0f / std::tan(.5f)));
                CHECK(Near(clip.x * 1.6f, clip.y));
            }
    auto camera = Scalars(true);
    camera.cameraFOV = 60;
    const auto degrees = Resolve(nullptr, nullptr, &camera, false);
    CHECK(degrees.Complete());
    CHECK(Near(Transform(degrees.projection, 0, 1, -5).y, 1.0f / std::tan(XM_PI / 6)));
}

static void MissingAndCorruptData()
{
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    auto camera = Scalars(true);
    const XMMATRIX zero {};
    const XMMATRIX singular = XMMatrixScaling(1, 1, 0);
    const XMMATRIX sentinel = XMMatrixSet(
        sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT,
        sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT,
        sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT,
        sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT, sl::INVALID_FLOAT);
    for (const auto& bad : {zero, singular, sentinel})
    {
        const auto fallback = Resolve(&bad, &bad, &camera, false);
        CHECK(fallback.Complete() && fallback.viewSource == Source::StreamlineBasis);
        CHECK(fallback.projectionSource == Source::StreamlineScalars);
        CHECK(!Resolve(&bad, &bad, nullptr, false).Complete());
    }
    for (float bad : {sl::INVALID_FLOAT, nan, inf})
    {
        // A single sentinel in an otherwise invertible matrix is not valid camera data.
        const auto partial = XMMatrixSet(1, 0, 0, bad, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1);
        CHECK(!MatrixIsInitialized(partial));
        CHECK(!Resolve(&partial, &partial, nullptr, false).Complete());
        auto invalidBasis = camera;
        invalidBasis.cameraUp.y = bad;
        const auto missingView = Resolve(nullptr, nullptr, &invalidBasis, false);
        CHECK(!missingView.Complete() && missingView.viewSource == Source::Missing);
        auto invalidFull = camera;
        SetProjection(invalidFull, XMMatrixPerspectiveFovRH(1, 1.6f, .1f, 100));
        invalidFull.cameraViewToClip[0].y = bad;
        CHECK(Resolve(nullptr, nullptr, &invalidFull, false).projectionSource == Source::StreamlineScalars);
    }
    const sl::Constants absent;
    CHECK(!Resolve(nullptr, nullptr, &absent, false).Complete());
    CHECK(!Resolve(nullptr, nullptr, nullptr, false).Complete());
    for (float bad : {sl::INVALID_FLOAT, nan, inf, 0.0f})
    {
        auto invalid = camera;
        invalid.cameraViewToClip[2].w = bad;
        CHECK(!Resolve(nullptr, nullptr, &invalid, false).Complete());
    }
    for (float bad : {sl::INVALID_FLOAT, nan, inf, 0.0f, -1.0f, XM_PI, 180.0f})
    {
        auto invalid = camera;
        invalid.cameraFOV = bad;
        CHECK(!Resolve(nullptr, nullptr, &invalid, false).Complete());
    }
    for (float bad : {sl::INVALID_FLOAT, nan, inf, 0.0f, -1.0f})
    {
        auto invalid = camera;
        invalid.cameraNear = bad;
        CHECK(!Resolve(nullptr, nullptr, &invalid, false).Complete());
        invalid = camera;
        invalid.cameraAspectRatio = bad;
        CHECK(!Resolve(nullptr, nullptr, &invalid, false).Complete());
    }
    for (float bad : {sl::INVALID_FLOAT, nan, inf, -.1f, .1f, .05f})
    {
        auto invalid = camera;
        invalid.cameraFar = bad;
        CHECK(!Resolve(nullptr, nullptr, &invalid, false).Complete());
    }
    camera.orthographicProjection = sl::Boolean::eTrue;
    CHECK(!Resolve(nullptr, nullptr, &camera, false).Complete());
    camera = Scalars(true);
    camera.cameraUp = camera.cameraRight; // Finite, but non-invertible camera basis.
    CHECK(!Resolve(nullptr, nullptr, &camera, false).Complete());
}

static void PhysicalPlaneConsumers()
{
    const XMMATRIX view = XMMatrixIdentity();
    // PBZ supplies linear depth with DepthInverted=false, but its full
    // projection is reverse-Z. Texture flags must not swap physical planes.
    const auto pbz = XMMatrixSet(.74999815f, 0, 0, 0,
                                 0, 1.33333f, 0, 0,
                                 0, 0, 0, 10,
                                 0, 0, 1, 0);
    const auto pbzCamera = Resolve(&view, &pbz, nullptr, false);
    CHECK(pbzCamera.Complete() && pbzCamera.planes.valid);
    CHECK(Near(pbzCamera.planes.nearPlane, 10));
    CHECK(pbzCamera.planes.farInfinite);
    for (bool rh : {false, true})
        for (bool reversed : {false, true})
        {
            const float sign = rh ? -1.0f : 1.0f;
            for (float farPlane : {800.0f, 100000.0f})
            {
                const float matrixNear = reversed ? farPlane : .05f;
                const float matrixFar = reversed ? .05f : farPlane;
                const auto rows = rh
                    ? XMMatrixPerspectiveFovRH(1, 1.6f, matrixNear, matrixFar)
                    : XMMatrixPerspectiveFovLH(1, 1.6f, matrixNear, matrixFar);
                const auto projection = XMMatrixTranspose(rows);
                const auto resolved = Resolve(&view, &projection, nullptr, reversed);
                const auto& planes = resolved.planes;
                CHECK(resolved.Complete() && planes.valid && !planes.farInfinite);
                CHECK(Near(planes.nearPlane, .05f));
                // Standard-Z far precision is limited by float cancellation at A/W=1.
                CHECK(planes.farPlane > planes.nearPlane && std::isfinite(planes.farPlane));
                CHECK(Near(Depth(projection, sign * planes.nearPlane), reversed ? 1.0f : 0.0f));
                CHECK(Near(Depth(projection, sign * planes.farPlane), reversed ? 0.0f : 1.0f));
                CHECK(planes.DenoiserFar() == planes.farPlane);
                if (farPlane > 65504.0f) CHECK(planes.DenoiserFar() > 65504.0f);
                if (reversed) CHECK(Near(planes.farPlane / farPlane, 1));
            }
        }

    // Exact projection-only Godot failure: a small positive RH reverse-Z A
    // is finite, not an invitation to set the immutable SR infinite-depth flag.
    auto godot = XMMatrixTranspose(XMMatrixPerspectiveFovRH(1, 1.6f, 800, .05f));
    godot.r[2].m128_f32[2] = 6.246567e-5f;
    auto resolved = Resolve(&view, &godot, nullptr, true);
    CHECK(resolved.planes.valid && !resolved.planes.farInfinite);
    CHECK(resolved.planes.farPlane > 799 && resolved.planes.farPlane < 802);
    auto tinyFinite = godot;
    tinyFinite.r[2].m128_f32[2] = 1.19e-6f;
    const auto positiveRoot = Resolve(&view, &tinyFinite, nullptr, true);
    CHECK(positiveRoot.planes.valid && !positiveRoot.planes.farInfinite);
    CHECK(positiveRoot.planes.farPlane > 40000 && positiveRoot.planes.farPlane < 45000);

    for (bool rh : {false, true})
        for (bool reversed : {false, true})
        {
            auto scalar = Scalars(rh);
            scalar.cameraNear = .05f;
            scalar.cameraFar = 0;
            auto infinite = Resolve(nullptr, nullptr, &scalar, reversed);
            CHECK(infinite.planes.valid && infinite.planes.farInfinite);
            CHECK(infinite.planes.farPlane == std::numeric_limits<float>::max());
            CHECK(infinite.planes.DenoiserFar() == 65504.0f);
            auto noisy = infinite.projection;
            const float W = rh ? -1.0f : 1.0f;
            noisy.r[2].m128_f32[2] = reversed ? W * 1.19e-6f : W * (1.0f - 1.19e-6f);
            const auto inferred = Resolve(&view, &noisy, nullptr, reversed);
            CHECK(inferred.planes.valid && inferred.planes.farInfinite); // Wrong-sign far root.
            scalar.cameraFar = 800;
            const auto finiteMetadata = Resolve(&view, &noisy, &scalar, reversed);
            CHECK(finiteMetadata.planes.valid && !finiteMetadata.planes.farInfinite);
            CHECK(finiteMetadata.planes.nearPlane == .05f && finiteMetadata.planes.farPlane == 800);
            scalar.cameraFar = 0;
            const auto knownInfinite = Resolve(&view, &godot, &scalar, true);
            CHECK(knownInfinite.planes.valid && knownInfinite.planes.farInfinite);

            // Explicit SR input/INI values win, including a single-plane override.
            const PlaneOverrides explicitFinite {.nearPlane = .2f, .farPlane = 200000,
                                                  .hasNear = true, .hasFar = true};
            const auto overridden = Resolve(&view, &noisy, &scalar, reversed, explicitFinite);
            CHECK(overridden.planes.valid && !overridden.planes.farInfinite);
            CHECK(overridden.planes.nearPlane == .2f && overridden.planes.farPlane == 200000);
            CHECK(overridden.planes.DenoiserFar() == 200000);
            const PlaneOverrides explicitInfinite {.farPlane = std::numeric_limits<float>::max(), .hasFar = true};
            CHECK(Resolve(&view, &godot, nullptr, true, explicitInfinite).planes.farInfinite);
        }

    auto invalid = godot;
    invalid.r[2].m128_f32[2] = -0.25f; // A substantial wrong-sign root is not noisy infinity.
    CHECK(!Resolve(&view, &invalid, nullptr, true).planes.valid);
    CHECK(!Resolve(&view, &view, nullptr, false).planes.valid); // Orthographic/identity.
    const PlaneOverrides badNear {.nearPlane = -1, .hasNear = true};
    CHECK(!Resolve(&view, &godot, nullptr, true, badNear).planes.valid);
    const PlaneOverrides badFar {.farPlane = .01f, .hasFar = true};
    CHECK(!Resolve(&view, &godot, nullptr, true, badFar).planes.valid);
}

int main()
{
    BasisAndPrecedence();
    FullOffAxisProjection();
    ScalarDepthConventions();
    MissingAndCorruptData();
    PhysicalPlaneConsumers();
    std::puts("fsrd_camera_contract: PASS (production camera resolver CPU behavior)");
}
