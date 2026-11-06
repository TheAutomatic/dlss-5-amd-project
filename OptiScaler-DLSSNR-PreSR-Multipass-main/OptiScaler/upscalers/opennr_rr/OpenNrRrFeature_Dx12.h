#pragma once
#include <upscalers/IFeature_Dx12.h>
#include "opennr_rr_api.h"

#include <string>
#include <vector>

// OpenNrRrRuntime.dll, loaded once from OptiScaler's folder.
namespace OpenNrRr
{
// Loads the runtime and asks it once whether this adapter and nvngx_dlssd.dll can run the network.
bool Usable(LUID adapter);
// Whether Usable has run, and why OpenRR is not offered (empty while it is).
bool Checked();
std::string Reason();
// A failure this session: OpenRR stays off until the game restarts, so the fallback never returns to it.
void Block(const std::string& why);
// The running context's state, for the menu. False without one.
bool Info(OnrRrInfo& info);
// Whether the game's lists are OptiScaler's split proxies or the runtime's D3D12 direct recording path is available.
bool SplitReady();
// The runtime's ABI 5 (the Vulkan host backend), loading the runtime; null when it does not offer it.
const OnrRrApi5* Api5();
// The nvngx_dlssd.dll Usable settled on, and the runtime's last error on this thread.
std::wstring DlssdPath();
std::string LastError();
// Whether an nvngx_dlssd.dll to build the weights from is there (the opennr-rr folder, OptiScaler's lookup, the game's).
bool DlssdFound();
// The context the menu's Info reads.
void SetInfoContext(void* context);
void ClearInfoContext(void* context);
// Applies Config::OpenNrRrMode ("new" vs "legacy") to the context via OnrRrSetFastMode, and queries OnrRrGetFastMode.
void SyncFastMode(void* context);
int32_t FastModeState();
// The DLSS-D texture and scalar keys the inputs inventory in the log reads.
std::vector<const char*> NgxTextureKeys();
std::vector<const char*> NgxScalarKeys();
void ResolveMatrices(const NVSDK_NGX_Parameter& params, bool depthInverted, float (&w2v)[16], float (&v2c)[16]);
} // namespace OpenNrRr

class OpenNrRrFeatureDx12 : public IFeature_Dx12
{
  public:
    OpenNrRrFeatureDx12(unsigned int InHandleId, NVSDK_NGX_Parameter* InParameters);
    ~OpenNrRrFeatureDx12();

    // OpenRR's release, as the overlay and the menu show it (not the runtime's ABI)
    feature_version Version() override { return { 0, 1, 1 }; }
    Upscaler GetUpscalerType() const override { return Upscaler::OpenNrRr; }
    bool IsWithDx12() final { return false; }

    bool RecordingsRetired();
    static void DestroyWhenRetired(std::unique_ptr<OpenNrRrFeatureDx12> feature);
    static void ReapDeferredFeatures();

  protected:
    bool InitInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;
    bool EvaluateInternal(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters) override;

  private:
    void* _context = nullptr;
    // The job armed on a list's between slot, until that list runs.
    void* _job = nullptr;
    ID3D12CommandList* _jobList = nullptr;
    uint32_t _maxRenderWidth = 0;
    uint32_t _maxRenderHeight = 0;
    bool _ngxHWDepth = false;
    bool _hasNGXDepthType = false;
    int _roughnessPacked = -1; // -1 until the create params or the first frame settle it
    int _loggedHWDepth = -1;
    int _loggedSwitches = -1;   // the diagnostic switches last logged
    float _loggedMaskScale = 0.0f;
    bool _fellBack = false;
    std::vector<uint64_t> _recordingTickets;

    void DropStaleJob();
    bool FillFrame(const NVSDK_NGX_Parameter& params, OnrRrFrame& frame);
    void FallBack(const std::string& why);
    static int32_t Between(void* feature, void* job, void* queue);
    // D3D12 backend: the frame recorded into the game's list (Record)
    bool EvaluateD3d12(ID3D12GraphicsCommandList* InCommandList, NVSDK_NGX_Parameter* InParameters);
};
