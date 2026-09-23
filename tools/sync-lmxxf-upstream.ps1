<#
.SYNOPSIS
  Synchronize vendored lmxxf source closure from an upstream clone without git cherry-pick.

.DESCRIPTION
  Copies the pinned subset of headers, shaders, and hip sources from the upstream
  repository (by default: ..\dlss5-on-amd-9070xt-porting) into third_party\lmxxf\,
  verifies/applies local compatibility patches, and updates UPSTREAM.md with the commit hash.
  By default, Development\HIP\hip_d3d12_bridge.h is preserved (pinned & patched) to protect
  local queue drain checks and zero-residual fallback implementations.
  Pass -UpdateBridge to explicitly overwrite and re-patch hip_d3d12_bridge.h.

.PARAMETER UpstreamPath
  Path to the cloned upstream repository. Default: '..\dlss5-on-amd-9070xt-porting'.

.PARAMETER SkipModules
  If set, do not update third_party\lmxxf\modules\ from upstream build output.

.PARAMETER UpdateBridge
  If set, overwrites Development\HIP\hip_d3d12_bridge.h from upstream and re-applies
  all local patches (zero fallback, drain check, clear resource management).

.PARAMETER SkipBuild
  If set, skips the post-sync compilation verification of LmxxfNrRuntime.dll.

.EXAMPLE
  .\tools\sync-lmxxf-upstream.ps1
  .\tools\sync-lmxxf-upstream.ps1 -UpdateBridge
  .\tools\sync-lmxxf-upstream.ps1 -UpstreamPath 'D:\repos\dlss5-on-amd-9070xt-porting'
#>
[CmdletBinding()]
param(
    [string]$UpstreamPath = '..\dlss5-on-amd-9070xt-porting',
    [switch]$SkipModules,
    [switch]$UpdateBridge,
    [switch]$SkipBuild
)

$ErrorActionPreference = 'Stop'
$root = Split-Path -Parent $PSScriptRoot
if (-not $root) { $root = (Get-Location).Path }
$vendorRoot = Join-Path $root 'third_party\lmxxf'

$upstream = Resolve-Path (Join-Path $root $UpstreamPath) -ErrorAction SilentlyContinue
if (-not $upstream -or -not (Test-Path $upstream)) {
    throw "Upstream repository not found at '$UpstreamPath'. Please clone or specify -UpstreamPath."
}

Write-Host "Syncing from upstream: $upstream" -ForegroundColor Cyan

# 1. Query git commit of upstream
$commitHash = ''
try {
    $commitHash = (git -C $upstream rev-parse HEAD 2>$null).Trim()
} catch {}
if (-not $commitHash) { $commitHash = 'unknown' }
Write-Host "Upstream HEAD commit: $commitHash" -ForegroundColor Green

# 2. Synchronize selected headers
$headerFiles = @(
    'Development\HIP\hip_api.h',
    'Development\HIP\hip_d3d12_bridge.h',
    'Development\HIP\hip_device_properties.h',
    'Development\HIP\hip_reference_network.h',
    'Development\HIP\packed_weights.h',
    'src\native_device_identity.h',
    'src\native_game_codec.h',
    'src\native_game_rgb_input.h',
    'src\native_hip_network.h',
    'src\native_input_geometry.h',
    'src\native_lab_paths.h',
    'src\native_network_geometry.h',
    'src\native_pinned_resource.h',
    'src\native_pso.h',
    'src\native_rgb_reflect.h',
    'src\native_rgb_texture.h',
    'src\native_shader_cache.h'
)

foreach ($rel in $headerFiles) {
    if ($rel -eq 'Development\HIP\hip_d3d12_bridge.h' -and -not $UpdateBridge) {
        Write-Host "  Preserved (pinned & patched): $rel (pass -UpdateBridge to overwrite and re-patch)" -ForegroundColor DarkYellow
        continue
    }
    $src = Join-Path $upstream $rel
    $dst = Join-Path $vendorRoot $rel
    if (Test-Path -LiteralPath $src) {
        $parent = Split-Path -Parent $dst
        if (-not (Test-Path $parent)) { New-Item -ItemType Directory -Force -Path $parent | Out-Null }
        Copy-Item -LiteralPath $src -Destination $dst -Force
        Write-Host "  Updated: $rel"
    } else {
        Write-Warning "  Missing in upstream: $rel"
    }
}

# 3. Synchronize shaders (only live D3D12 glue shaders; exclude retired dx12-network)
$shaderDir = Join-Path $upstream 'shaders'
if (Test-Path $shaderDir) {
    $dstShaders = Join-Path $vendorRoot 'shaders'
    robocopy $shaderDir $dstShaders *.hlsl /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    Write-Host "  Synchronized shaders"
}

# 4. Synchronize hip recipes
$hipDir = Join-Path $upstream 'hip'
if (Test-Path $hipDir) {
    $dstHip = Join-Path $vendorRoot 'hip'
    robocopy $hipDir $dstHip *.hip build-modules.ps1 rtc_compile.cpp SHA256SUMS README.md /NFL /NDL /NJH /NJS /nc /ns /np | Out-Null
    Write-Host "  Synchronized hip recipes"
}

# 5. Check and apply local patches
# Patch A: #include <algorithm> in hip_reference_network.h
$refNet = Join-Path $vendorRoot 'Development\HIP\hip_reference_network.h'
if (Test-Path $refNet) {
    $content = Get-Content -LiteralPath $refNet -Raw
    if ($content -notmatch '#include\s*<algorithm>') {
        $content = $content -replace '(#include\s*<vector>)', "`$1`r`n#include <algorithm>"
        [IO.File]::WriteAllText($refNet, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: #include <algorithm> in hip_reference_network.h" -ForegroundColor Yellow
    }
}

# Patch B: hip_d3d12_bridge.h (ClearOutput, WaitForSubmittedWork completion check, zero upload, and CancelUnsubmitted)
$bridgeH = Join-Path $vendorRoot 'Development\HIP\hip_d3d12_bridge.h'
if (Test-Path $bridgeH) {
    if (-not $UpdateBridge) {
        Write-Host "  Preserved Patch B: hip_d3d12_bridge.h is pinned (pass -UpdateBridge to re-patch)" -ForegroundColor DarkYellow
    } else {
        $content = Get-Content -LiteralPath $bridgeH -Raw

        # B.0: Ensure #include <algorithm>
        if ($content -notmatch '#include\s*<algorithm>') {
            if ($content -notmatch '(?m)^#include\s*<chrono>') {
                throw "Patch B failed: cannot find anchor '#include <chrono>' in hip_d3d12_bridge.h"
            }
            $content = $content -replace '(?m)^(#include\s*<chrono>)', "#include <algorithm>`r`n`$1"
        }

        # B.1: Member variables for clear resources
        if ($content -notmatch 'ID3D12Resource\*\s*zero_upload') {
            $varAnchor = 'HANDLE fence_handle{},event{};Handle semaphore{};Shared input,history,output;UINT64 value{};size_t pixels{};bool readable{},pending{},failed{};'
            if (-not $content.Contains($varAnchor)) {
                throw "Patch B failed: cannot find member variables anchor in hip_d3d12_bridge.h"
            }
            $content = $content.Replace($varAnchor, "$varAnchor`r`n ID3D12Resource* zero_upload{};ID3D12CommandAllocator* clear_alloc{};ID3D12GraphicsCommandList* clear_cmd{};`r`n size_t zero_upload_bytes{};bool clear_submission_unconfirmed{};")
        }

        # B.2: WaitForSubmittedWork fence completed value check
        if ($content -notmatch 'fence->GetCompletedValue\(\)<target') {
            $waitAnchor = 'if(pending&&queue&&fence){auto target=++value;if(FAILED(queue->Signal(fence,target))||FAILED(fence->SetEventOnCompletion(target,event))||WaitForSingleObject(event,30000)!=WAIT_OBJECT_0)return false;}'
            if (-not $content.Contains($waitAnchor)) {
                throw "Patch B failed: cannot find WaitForSubmittedWork anchor in hip_d3d12_bridge.h"
            }
            $waitReplacement = 'if(pending&&queue&&fence){auto target=++value;if(FAILED(queue->Signal(fence,target))||FAILED(fence->SetEventOnCompletion(target,event))||WaitForSingleObject(event,30000)!=WAIT_OBJECT_0||fence->GetCompletedValue()<target)return false;}'
            $content = $content.Replace($waitAnchor, $waitReplacement)
        }

        # B.3: Destructor cleanup of clear resources
        if ($content -notmatch 'clear_cmd->Release') {
            $dtorAnchor = "~D3D12Bridge(){`r`n  if(!WaitForSubmittedWork())return;"
            if (-not $content.Contains($dtorAnchor)) {
                $dtorAnchorLf = "~D3D12Bridge(){\n  if(!WaitForSubmittedWork())return;"
                if (-not $content.Contains($dtorAnchorLf)) {
                    throw "Patch B failed: cannot find destructor anchor in hip_d3d12_bridge.h"
                }
                $dtorAnchor = $dtorAnchorLf
            }
            $dtorReplacement = "$dtorAnchor`r`n  if(clear_cmd)clear_cmd->Release();if(clear_alloc)clear_alloc->Release();if(zero_upload)zero_upload->Release();"
            $content = $content.Replace($dtorAnchor, $dtorReplacement)
        }

        # B.4: Create() zero upload and clear cmd/alloc initialization
        if ($content -notmatch 'zero_upload_bytes=std::min') {
            $noiseAnchor = 'network->SetNoise(noise);'
            if (-not $content.Contains($noiseAnchor)) {
                throw "Patch B failed: cannot find 'network->SetNoise(noise);' anchor in hip_d3d12_bridge.h"
            }
            $initCode = @'
  // A small, verified zero source is enough for the rare D3D12 fallback. Avoid a
  // frame-sized upload allocation on the normal HIP path.
  zero_upload_bytes=std::min<size_t>(pixels*12,65536);
  D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
  D3D12_RESOURCE_DESC ud{};ud.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;ud.Width=zero_upload_bytes;ud.Height=1;ud.DepthOrArraySize=ud.MipLevels=1;ud.SampleDesc.Count=1;ud.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;ud.Flags=D3D12_RESOURCE_FLAG_NONE;
  Check(device->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&ud,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&zero_upload)),"zero upload buffer");
  void*mappedZero=nullptr;D3D12_RANGE r{0,0};Check(zero_upload->Map(0,&r,&mappedZero),"map zero upload buffer");
  if(!mappedZero)throw std::runtime_error("map zero upload buffer returned null");
  std::memset(mappedZero,0,zero_upload_bytes);zero_upload->Unmap(0,nullptr);
  Check(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&clear_alloc)),"clear allocator");
  Check(device->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,clear_alloc,nullptr,IID_PPV_ARGS(&clear_cmd)),"clear command list");Check(clear_cmd->Close(),"close clear command list");
'@
            $content = $content.Replace($noiseAnchor, "$noiseAnchor`r`n$initCode")
        }

        # B.5: ClearOutputAsync / ClearOutputD3D12 / ClearOutput methods
        if ($content -notmatch 'ClearOutputAsync') {
            $notifyCommentAnchor = '// Acknowledges submission, not GPU completion.'
            $notifyAnchor = 'void NotifyOutputSubmitted(ID3D12CommandQueue*consumer)'
            if ($content.Contains($notifyCommentAnchor)) {
                $targetAnchor = $notifyCommentAnchor
            } elseif ($content.Contains($notifyAnchor)) {
                $targetAnchor = $notifyAnchor
            } else {
                throw "Patch B failed: cannot find 'void NotifyOutputSubmitted' anchor in hip_d3d12_bridge.h"
            }
            $clearMethods = @'
 bool ClearOutputAsync() noexcept {
  if(!network||failed||!output.mapped)return false;
  auto&api=network->Runtime();
  try{
   api.Check(api.hipMemsetAsync(output.mapped,0,pixels*12,network->Stream()),"clear output");
   network->Synchronize();
   phase=Phase::OutputRecorded;
   return true;
  }catch(...){
   failed=true;
   return false;
  }
 }
 bool ClearOutputD3D12(ID3D12CommandQueue* targetQueue) noexcept {
  if(!network||!device||!targetQueue||!output.resource||!zero_upload||!zero_upload_bytes||clear_submission_unconfirmed)return false;
  // A failed HIP call can leave earlier work queued. Do not race that work with
  // a D3D12 write to the same shared buffer.
  if(network->Runtime().hipStreamSynchronize(network->Stream())!=0)return false;
  ID3D12Device* owner=nullptr;
  if(FAILED(targetQueue->GetDevice(IID_PPV_ARGS(&owner)))||!owner)return false;
  const bool sameDevice=NativeSameDevice(owner,device);owner->Release();
  if(!sameDevice)return false;
  ID3D12CommandAllocator* alloc=clear_alloc;
  ID3D12GraphicsCommandList* cmd=clear_cmd;
  ID3D12Fence* completion=nullptr;
  HANDLE completedEvent=nullptr;
  bool temp=false;
  bool submitted=false;
  const auto qType=targetQueue->GetDesc().Type;
  if(qType!=D3D12_COMMAND_LIST_TYPE_DIRECT||!alloc||!cmd){
   if(qType!=D3D12_COMMAND_LIST_TYPE_DIRECT&&qType!=D3D12_COMMAND_LIST_TYPE_COMPUTE)return false;
   if(FAILED(device->CreateCommandAllocator(qType,IID_PPV_ARGS(&alloc))))return false;
   if(FAILED(device->CreateCommandList(0,qType,alloc,nullptr,IID_PPV_ARGS(&cmd)))){alloc->Release();return false;}
   temp=true;
  }else if(FAILED(alloc->Reset())||FAILED(cmd->Reset(alloc,nullptr))){return false;}
  bool ok=false;
  try{
   if(FAILED(device->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&completion))))throw std::runtime_error("clear fence");
   completedEvent=CreateEventW(nullptr,FALSE,FALSE,nullptr);
   if(!completedEvent)throw std::runtime_error("clear event");
   Barrier(cmd,output.resource,D3D12_RESOURCE_STATE_COMMON,D3D12_RESOURCE_STATE_COPY_DEST);
   const UINT64 total=UINT64(pixels)*12;
   for(UINT64 offset=0;offset<total;offset+=zero_upload_bytes)
    cmd->CopyBufferRegion(output.resource,offset,zero_upload,0,std::min<UINT64>(zero_upload_bytes,total-offset));
   Barrier(cmd,output.resource,D3D12_RESOURCE_STATE_COPY_DEST,D3D12_RESOURCE_STATE_COMMON);
   Check(cmd->Close(),"close clear command list");
   ID3D12CommandList* lists[]={cmd};
   targetQueue->ExecuteCommandLists(1,lists);
   submitted=true;clear_submission_unconfirmed=true;
   Check(targetQueue->Signal(completion,1),"signal clear completion");
   Check(completion->SetEventOnCompletion(1,completedEvent),"wait for clear completion");
   ok=WaitForSingleObject(completedEvent,30000)==WAIT_OBJECT_0&&completion->GetCompletedValue()>=1&&SUCCEEDED(device->GetDeviceRemovedReason());
   if(ok){clear_submission_unconfirmed=false;phase=Phase::OutputRecorded;}
  }catch(...){ok=false;}
  // SetEventOnCompletion can still signal after a timeout. Keep its fence and
  // event alive whenever the submitted work has not been confirmed complete.
  if(!submitted||ok){if(completedEvent)CloseHandle(completedEvent);if(completion)completion->Release();}
  // If submission completion is unknown, retain command storage until the
  // session's fail-closed teardown instead of freeing a GPU-live allocator.
  if(temp&&(!submitted||ok)){cmd->Release();alloc->Release();}
  return ok;
 }
 bool ClearOutput(ID3D12CommandQueue* targetQueue) noexcept {
  if(ClearOutputAsync())return true;
  return ClearOutputD3D12(targetQueue);
 }
'@
            $content = $content.Replace($targetAnchor, "$clearMethods`r`n $targetAnchor")
        }

        # B.6: NotifyOutputSubmittedIfRecorded failure-safe reset
        $notifyIfAnchor = 'void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer)NotifyOutputSubmitted(consumer);}'
        if ($content.Contains($notifyIfAnchor)) {
            $notifyIfReplacement = 'void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer){if(failed){phase=Phase::Ready;return;}NotifyOutputSubmitted(consumer);}}'
            $content = $content.Replace($notifyIfAnchor, $notifyIfReplacement)
        }

        # B.7: CancelUnsubmitted and CurrentPhase (for older upstream commits if missing)
        if ($content -notmatch 'CancelUnsubmitted') {
            if ($content -match 'enum class Phase \{ Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded \};') {
                $content = $content -replace 'enum class Phase \{ Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded \};',
                    "public:`r`n enum class Phase { Ready, InputRecorded, OutputRecordedPendingHip, HipQueued, OutputRecorded };`r`n Phase CurrentPhase()const{return phase;}`r`nprivate:"
            }
            if ($content -notmatch 'void CancelUnsubmitted') {
                $marker = 'void NotifyOutputSubmitted(ID3D12CommandQueue*consumer){Require(Phase::OutputRecorded);QueueContract(consumer);phase=Phase::Ready;}'
                $replacement = "$marker`r`n void NotifyOutputSubmittedIfRecorded(ID3D12CommandQueue*consumer){if(phase==Phase::OutputRecorded&&consumer){if(failed){phase=Phase::Ready;return;}NotifyOutputSubmitted(consumer);}}`r`n void CancelUnsubmitted(){if(phase==Phase::InputRecorded||phase==Phase::OutputRecordedPendingHip){phase=Phase::Ready;readable=false;}}"
                $content = $content.Replace($marker, $replacement)
            }
        }

        [IO.File]::WriteAllText($bridgeH, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: local extensions to hip_d3d12_bridge.h" -ForegroundColor Yellow
    }
}

# Patch C: remove unused native_split.h from native_rgb_reflect.h
$reflectH = Join-Path $vendorRoot 'src\native_rgb_reflect.h'
if (Test-Path $reflectH) {
    $content = Get-Content -LiteralPath $reflectH -Raw
    if ($content -match '#include\s*"native_split\.h"') {
        $content = $content -replace '#include\s*"native_split\.h"\r?\n?', ''
        [IO.File]::WriteAllText($reflectH, $content, [Text.UTF8Encoding]::new($false))
        Write-Host "  Applied patch: removed native_split.h in native_rgb_reflect.h" -ForegroundColor Yellow
    }
}

# 6. Update UPSTREAM.md with new commit and timestamp
$upstreamMd = Join-Path $vendorRoot 'UPSTREAM.md'
if (Test-Path $upstreamMd) {
    $md = Get-Content -LiteralPath $upstreamMd -Raw
    $today = (Get-Date).ToString('yyyy-MM-dd')
    $md = $md -replace '(?m)^- Commit: .*', "- Commit: ``$commitHash`` (synced $today)"
    [IO.File]::WriteAllText($upstreamMd, $md, [Text.UTF8Encoding]::new($false))
    Write-Host "  Updated UPSTREAM.md" -ForegroundColor Green
}

# 7. Post-sync build verification
if (-not $SkipBuild) {
    Write-Host "Verifying runtime build: tools\build-lmxxf-runtime.cmd..." -ForegroundColor Cyan
    $buildCmd = Join-Path $root 'tools\build-lmxxf-runtime.cmd'
    if (Test-Path -LiteralPath $buildCmd) {
        & cmd.exe /c "`"$buildCmd`""
        if ($LASTEXITCODE -ne 0) {
            throw "Post-sync build verification FAILED! LmxxfNrRuntime.dll failed to compile (ExitCode $LASTEXITCODE)."
        }
        Write-Host "  Runtime build verified successfully (ExitCode 0)." -ForegroundColor Green
    } else {
        Write-Warning "  build-lmxxf-runtime.cmd not found, skipping build verification."
    }
} else {
    Write-Host "Skipping runtime build verification (-SkipBuild specified)." -ForegroundColor Yellow
}

Write-Host "Sync complete! Upstream commit: $commitHash" -ForegroundColor Green
