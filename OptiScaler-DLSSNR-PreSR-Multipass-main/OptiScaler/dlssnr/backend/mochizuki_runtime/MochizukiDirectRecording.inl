// Pinned Linux PE protocol (cc27501d, nr_pe_interop + Session::run_after).
// Included after BoundRecording; all calls are serialized by the host recording mutex.
int32_t GetDirectCommandBuffer(void* context, void* nativeList, uint64_t* commandBuffer)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !commandBuffer) return Fail(LMXXF_NR_INVALID_ARGUMENT, "direct handle: bad arguments");
    *commandBuffer = 0;
    if (!s->vk.adoptedVkd3d) return LMXXF_NR_OK; // Windows retains the bridge protocol.
    auto* list = static_cast<ID3D12GraphicsCommandList*>(nativeList);
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandListExt> ext;
    VkCommandBuffer command {};
    if (!list || FAILED(list->QueryInterface(IID_PPV_ARGS(&ext))) ||
        FAILED(ext->GetVulkanHandle(&command)) || !command)
        return Fail(LMXXF_NR_UNAVAILABLE, "direct handle: current native list exposes no VkCommandBuffer");
    *commandBuffer = uint64_t(reinterpret_cast<uintptr_t>(command));
    return LMXXF_NR_OK;
}

nr::ColourFrame DirectResource(Session* s, ID3D12Resource* resource, D3D12_RESOURCE_STATES state,
                               VkFormat fallback, bool depth = false)
{
    nr::ColourFrame out {};
    if (!resource) return out;
    UINT64 handle = 0, offset = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
    Microsoft::WRL::ComPtr<ID3D12DXVKInteropDevice1> newer;
    if (SUCCEEDED(s->vk.interop->QueryInterface(IID_PPV_ARGS(&newer))))
        newer->GetVulkanResourceInfo1(resource, &handle, &offset, &format);
    if (!handle && FAILED(s->vk.interop->GetVulkanResourceInfo(resource, &handle, &offset)))
        throw std::runtime_error("direct resource: vkd3d returned no image");
    const auto desc = resource->GetDesc();
    if (!handle || desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1)
        throw std::runtime_error("direct resource: a single-sample 2D VkImage is required");
    if (format == VK_FORMAT_UNDEFINED) format = fallback;
    if (depth) {
        switch (desc.Format) {
        case DXGI_FORMAT_R32G8X24_TYPELESS: case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS: format = VK_FORMAT_D32_SFLOAT_S8_UINT; break;
        case DXGI_FORMAT_R24G8_TYPELESS: case DXGI_FORMAT_D24_UNORM_S8_UINT:
        case DXGI_FORMAT_R24_UNORM_X8_TYPELESS: {
            if (format == VK_FORMAT_UNDEFINED) {
                VkFormatProperties fp {};
                vkGetPhysicalDeviceFormatProperties(s->vk.physical, VK_FORMAT_D24_UNORM_S8_UINT, &fp);
                format = fp.optimalTilingFeatures & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT ?
                    VK_FORMAT_D24_UNORM_S8_UINT : VK_FORMAT_D32_SFLOAT_S8_UINT;
            }
            break;
        }
        case DXGI_FORMAT_R32_TYPELESS: case DXGI_FORMAT_D32_FLOAT: case DXGI_FORMAT_R32_FLOAT:
            if (format == VK_FORMAT_UNDEFINED) format = desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ? VK_FORMAT_D32_SFLOAT : VK_FORMAT_R32_SFLOAT;
            break;
        case DXGI_FORMAT_R16_TYPELESS: case DXGI_FORMAT_D16_UNORM: case DXGI_FORMAT_R16_FLOAT:
            if (format == VK_FORMAT_UNDEFINED) format = desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ? VK_FORMAT_D16_UNORM : VK_FORMAT_R16_SFLOAT;
            break;
        default: break;
        }
    }
    if (format == VK_FORMAT_UNDEFINED) throw std::runtime_error("direct resource: unsupported VkFormat");
    out.image = (VkImage)handle;
    out.format = format;
    out.width = uint32_t(desc.Width); out.height = desc.Height;
    s->vk.interop->GetVulkanImageLayout(resource, state, &out.before);
    out.after = out.before;
    out.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (!(desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE)) out.usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) out.usage |= VK_IMAGE_USAGE_STORAGE_BIT;
    return out;
}

bool EnsureDirectInvalidator(Session* s) noexcept
{
    if (s->pipelineInvalidator && s->invalidatorCpu && s->invalidatorGpu) return true;
    if (s->invalidatorFailed || !s->device) return false;
    D3D12_HEAP_PROPERTIES heap {}; heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = 2; desc.Height = desc.DepthOrArraySize = desc.MipLevels = desc.SampleDesc.Count = 1;
    desc.Format = DXGI_FORMAT_R32_UINT;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    Microsoft::WRL::ComPtr<ID3D12Resource> image;
    Microsoft::WRL::ComPtr<ID3D12DescriptorHeap> cpu, gpu;
    HRESULT hr = s->device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
        D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&image));
    D3D12_DESCRIPTOR_HEAP_DESC hd {}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 1;
    if (SUCCEEDED(hr)) hr = s->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&cpu));
    hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    if (SUCCEEDED(hr)) hr = s->device->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&gpu));
    if (FAILED(hr) || !image || !cpu || !gpu) {
        s->invalidatorFailed = true;
        nr::logf("[mochizuki] could not build the direct pipeline invalidator (0x%08lX); keeping null compute PSO/root only",
                 static_cast<unsigned long>(hr));
        return false;
    }
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {}; uav.Format = desc.Format; uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    s->device->CreateUnorderedAccessView(image.Get(), nullptr, &uav, cpu->GetCPUDescriptorHandleForHeapStart());
    s->device->CreateUnorderedAccessView(image.Get(), nullptr, &uav, gpu->GetCPUDescriptorHandleForHeapStart());
    s->pipelineInvalidator = std::move(image); s->invalidatorCpu = std::move(cpu); s->invalidatorGpu = std::move(gpu);
    nr::logf("[mochizuki] direct interfaces: ID3D12GraphicsCommandListExt/GetVulkanHandle, "
             "ID3D12DXVKInteropDevice/GetVulkanResourceInfo+GetVulkanImageLayout; "
             "state restore=ALL_COMMANDS/layouts/null compute PSO+root/partial 2x1 ClearUAV meta");
    return true;
}

void ForgetDirectPipeline(Session* s, ID3D12GraphicsCommandList* list) noexcept
{
    list->SetPipelineState(nullptr);
    list->SetComputeRootSignature(nullptr);
    if (!EnsureDirectInvalidator(s)) return;
    // A partial rect forces vkd3d's meta shader, invalidating command_buffer_pipeline,
    // which the two D3D12 setters above cannot reach (official forget_bound_pipeline).
    const UINT zero[4] {};
    const D3D12_RECT rect {0, 0, 1, 1};
    list->ClearUnorderedAccessViewUint(s->invalidatorGpu->GetGPUDescriptorHandleForHeapStart(),
        s->invalidatorCpu->GetCPUDescriptorHandleForHeapStart(), s->pipelineInvalidator.Get(), zero, 1, &rect);
}

int32_t RecordDirect(void* context, void* token, void* nativeList, uint64_t commandBuffer)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    if (!r || r->invalidated || !r->job.directRecording || r->job.state != LMXXF_NR_JOB_PREPARED)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "direct record: recipe not prepared");
    // Even a pre-write refusal still has an attached observer. It must remain
    // submit-safe until Reset; after the first exposure/native command, this
    // recipe additionally pins every resource referenced by the partial stream.
    r->directRecordFailed = true;
    uint64_t current = 0;
    if (const auto rc = GetDirectCommandBuffer(context, nativeList, &current); rc != LMXXF_NR_OK) return rc;
    if (!current || current != commandBuffer)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "direct record: native command buffer generation changed");
    return Guard(s, [&] {
        BoundRecording bound(*s, *r);
        auto* list = static_cast<ID3D12GraphicsCommandList*>(nativeList);
        auto c = reinterpret_cast<VkCommandBuffer>(uintptr_t(commandBuffer));
        Job& j = s->job;
        EnsureDirectInvalidator(s);
        auto source = DirectResource(s, j.colour, j.colourState, s->colourVk);
        auto target = DirectResource(s, s->result, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s->colourVk);
        auto motion = j.motion ? DirectResource(s, j.motion, j.motionState, s->motionVk) : nr::ColourFrame {};
        auto depth = j.depth ? DirectResource(s, j.depth, j.depthState, VK_FORMAT_UNDEFINED, true) : nr::ColourFrame {};
        struct RestoreCompute {
            Session* session;
            ID3D12GraphicsCommandList* list;
            bool restored = false;
            ~RestoreCompute() { if (!restored) ForgetDirectPipeline(session, list); }
        } restoreCompute {s, list};
        const auto everything = [&](VkAccessFlags src, VkAccessFlags dst) {
            VkMemoryBarrier barrier {VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            barrier.srcAccessMask = src; barrier.dstAccessMask = dst;
            vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                 0, 1, &barrier, 0, nullptr, 0, nullptr);
        };
        const auto imageBarrier = [&](VkImage image, VkImageLayout before, VkImageLayout after) {
            ImageBarrier(c, image, before, after, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                         VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                         VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        };
        const auto copy = [&](const nr::ColourFrame& from, VkImage to, uint32_t width, uint32_t height) {
            imageBarrier(from.image, from.before, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            imageBarrier(to, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            VkImageCopy region {}; region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.extent = {width, height, 1};
            vkCmdCopyImage(c, from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, to, VK_IMAGE_LAYOUT_GENERAL, 1, &region);
            imageBarrier(from.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, from.after);
            imageBarrier(to, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        };
        const bool reset = s->resetPending.exchange(false);
        const uint32_t reasons = reset ? s->resetReasons.exchange(0) : 0;
        j.reset |= reset || s->historyNetwork.lock() != r->network || r->frameId != s->historyFrame + 1;
        j.normalizationReset |= (reasons & (Session::ResetQueue | Session::ResetCancel | Session::ResetExternal)) != 0;
        if (s->directRecorded < 2 || s->directRecorded % 3600 == 0)
            nr::logf("[mochizuki] direct-record.begin frame=%llu command=%p network=%llu", (unsigned long long)r->frameId,
                     (void*)c, (unsigned long long)s->runtime->id);
        // Preserve the existing exposure policy and true-tail readback tickets; no input carrier copy.
        float measured = 0;
        if (s->gameTexel.Poll(&measured)) s->textureExposure.store(std::isfinite(measured) && measured > 0 ? measured : 0);
        if (s->exposure.Poll(&measured)) s->meteredExposure.store(measured);
        if (j.exposureTexture) {
            s->gameTexel.RecordTexel(list, j.exposureTexture, j.exposureState, j.texelTicket, j.exposureFactor);
            s->meterIdle = 0; s->meteredExposure.store(0.f);
        } else if (s->gameExposure.load() > 0 || !j.meter || !j.linear) {
            s->meterIdle = 0; s->meteredExposure.store(0.f);
        } else if (s->exposure.Record(list, j.colour, j.colourState, j.width, j.height, j.meterTicket)) s->meterIdle = 0;
        else if (++s->meterIdle == Session::kMeterIdleFrames) s->meteredExposure.store(0.f);
        // Native exposure commands can rotate vkd3d's current command buffer.
        uint64_t afterExposure = 0;
        if (GetDirectCommandBuffer(context, nativeList, &afterExposure) != LMXXF_NR_OK || !afterExposure)
            throw std::runtime_error("direct record: exposure left no current command buffer");
        c = reinterpret_cast<VkCommandBuffer>(uintptr_t(afterExposure));
        everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        const uint32_t w = j.width, h = j.height, W = s->geometry.width, H = s->geometry.height;
        const bool padded = w < W || h < H;
        // Same-device image copy is the official run() seed protocol; the borrowed
        // input layout is restored immediately, and the output is restored below.
        copy(source, s->colourImage.image, w, h);
        if (padded) { Pad(c, s->colourImage.image, w, h, W, H); imageBarrier(s->colourImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL); }
        VkImage neural = s->colourImage.image;
        VkFormat neuralFormat = s->colourVk;
        if (j.canonicalise) {
            imageBarrier(s->canonicalColour.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
            s->convert.Dispatch(c, s->convertPipeline, W, H);
            imageBarrier(s->canonicalColour.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            neural = s->canonicalColour.image; neuralFormat = s->convert.convertFor;
        }
        nr::EngineFrame engine {};
        engine.colour.image = neural; engine.colour.format = neuralFormat;
        engine.colour.width = W; engine.colour.height = H; engine.colour.usage = kImageUsage;
        engine.reset = j.reset;
        engine.motion_scale_x = j.motionScaleX; engine.motion_scale_y = j.motionScaleY;
        engine.depth_inverted = j.depthInverted;
        if (motion.image) {
            if (j.motionPath == Job::Uploaded) {
                engine.motion = motion;
                engine.motion.width = j.motionWidth; engine.motion.height = j.motionHeight;
                engine.motion_texture_width = motion.width; engine.motion_texture_height = motion.height;
            } else {
                copy(motion, s->motionImage.image, j.motionWidth, j.motionHeight);
                // LINEAR clamps to the image, not the valid source rectangle.
                // Define its one-texel filter ring just like RunNetwork does.
                if (j.motionWidth < s->geometry.motionWidth || j.motionHeight < s->geometry.motionHeight) {
                    Pad(c, s->motionImage.image, j.motionWidth, j.motionHeight,
                        std::min(j.motionWidth + 1, s->geometry.motionWidth),
                        std::min(j.motionHeight + 1, s->geometry.motionHeight));
                    TransferBarrier(c, s->motionImage.image);
                }
                imageBarrier(s->motionScaled.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                Blit(c, s->motionImage.image, s->motionScaled.image, {0, 0}, {int32_t(j.motionWidth), int32_t(j.motionHeight)},
                     {0, 0}, {int32_t(w), int32_t(h)}, VK_FILTER_LINEAR);
                if (padded) { TransferBarrier(c, s->motionScaled.image); Pad(c, s->motionScaled.image, w, h, W, H); }
                imageBarrier(s->motionScaled.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                engine.motion.image = s->motionScaled.image; engine.motion.format = s->motionVk;
                engine.motion.width = W; engine.motion.height = H; engine.motion.usage = kImageUsage;
                engine.motion_texture_width = W; engine.motion_texture_height = H;
            }
        }
        if (depth.image) {
            engine.depth = depth;
            engine.depth.width = std::min(j.depthWidth ? j.depthWidth : depth.width, depth.width);
            engine.depth.height = std::min(j.depthHeight ? j.depthHeight : depth.height, depth.height);
        }
        s->runtime->set_history_strength(j.history); s->runtime->set_white_point(j.white);
        s->runtime->set_stage_timing(j.stageTiming);
        bool consumed = false;
        s->runtime->begin_recording_resources();
        try {
            if (s->runtime->reno) {
                s->runtime->reno->begin_recording_resources();
                auto& settings = j.reno;
                const uint32_t count = nr::renodx::stack_passes(settings);
                for (uint32_t k = count; k < s->runtime->renoPasses; ++k) s->runtime->release_feature(k + 1);
                s->runtime->renoPasses = count; s->runtime->reno->set(settings);
                nr::renodx::StageFrame sf {};
                sf.colour = sf.output = engine.colour; sf.motion = engine.motion;
                sf.reset = j.reset; sf.preserve_normalization = !j.normalizationReset; sf.after_upscale = j.afterUpscale;
                sf.motion_scale_x = engine.motion_scale_x * settings.mvec_scale_x;
                sf.motion_scale_y = engine.motion_scale_y * settings.mvec_scale_y; sf.preprocess = j.controls.preprocess;
                sf.create_flags_known = j.renoExposure > 0.f;
                if (sf.create_flags_known) {
                    const VkImage image = s->runtime->exposure.image.image;
                    imageBarrier(image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL);
                    VkClearColorValue value {}; value.float32[0] = j.renoExposure;
                    const VkImageSubresourceRange range {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
                    vkCmdClearColorImage(c, image, VK_IMAGE_LAYOUT_GENERAL, &value, 1, &range);
                    imageBarrier(image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
                    sf.exposure.image = image; sf.exposure.format = VK_FORMAT_R32_SFLOAT;
                    sf.exposure.width = sf.exposure.height = 1; sf.exposure.usage = kImageUsage;
                }
                try {
                    r->directTiming = std::make_unique<MochizukiStageTiming>();
                    VkPhysicalDeviceProperties timingProperties {};
                    vkGetPhysicalDeviceProperties(s->vk.physical, &timingProperties);
                    uint32_t familyCount = 0;
                    vkGetPhysicalDeviceQueueFamilyProperties(s->vk.physical, &familyCount, nullptr);
                    std::vector<VkQueueFamilyProperties> timingFamilies(familyCount);
                    vkGetPhysicalDeviceQueueFamilyProperties(s->vk.physical, &familyCount, timingFamilies.data());
                    r->directTiming->Create(s->vk.device, timingProperties.limits.timestampPeriod,
                        s->vk.family < familyCount ? timingFamilies[s->vk.family].timestampValidBits : 0, 0);
                    r->directTimingSlot = r->directTiming->Begin(c, false);
                } catch (...) {
                    r->directTiming.reset();
                    r->directTimingSlot = MochizukiStageTiming::kUnavailable;
                }
                uint32_t dispatches = 0;
                s->runtime->reno->record(c, sf, [&](uint32_t pass, const nr::ColourFrame& proxy, const nr::ColourFrame& output, bool resetPass) {
                    auto frame = engine; frame.colour = proxy; frame.target = output; frame.feature = pass + 1; frame.reset = resetPass;
                    frame.motion_scale_x = sf.motion_scale_x; frame.motion_scale_y = sf.motion_scale_y;
                    auto controls = j.controls; controls.preprocess = {}; controls.per_pass.clear(); controls.passes = 1;
                    controls.enabled = controls.apply_model = true;
                    const uint32_t k = std::min(pass, 3u); const bool own = k && !settings.pass_follow[k - 1];
                    controls.intensity = k ? settings.pass_intensity[k - 1] : settings.intensity;
                    controls.style = int(own ? settings.pass_style[k - 1] : settings.style);
                    controls.local_tone = own ? settings.pass_local_tone[k - 1] : settings.local_tone;
                    controls.local_structure = own ? settings.pass_local_structure[k - 1] : settings.local_structure;
                    controls.skin_structure = own ? settings.pass_skin_structure[k - 1] : settings.skin_structure;
                    controls.automatic_mask = own ? settings.pass_auto_mask[k - 1] : settings.auto_mask;
                    if (settings.skin_independent && controls.automatic_mask) {
                        controls.local_structure = std::min(controls.local_structure, 1.f);
                        if (controls.skin_structure < 0.f) controls.skin_structure = 1.f;
                    }
                    if (r->directTiming) r->directTiming->BeginNetwork(c, r->directTimingSlot);
                    const auto result = s->runtime->record_engine(c, frame, controls);
                    if (r->directTiming) r->directTiming->EndNetwork(c, r->directTimingSlot);
                    dispatches += result.frame.network_dispatches; consumed |= result.history_consumed;
                });
                r->renoResources = s->runtime->reno->end_recording_resources();
                if (r->directTiming) r->directTiming->End(c, r->directTimingSlot);
                s->dispatches.store(dispatches);
            } else if (motion.image) {
                const auto result = s->runtime->record_engine(c, engine, j.controls);
                s->dispatches.store(result.frame.network_dispatches); consumed = result.history_consumed;
            } else s->dispatches.store(s->runtime->record(c, engine.colour, j.controls, j.reset).network_dispatches);
            r->coreResources = s->runtime->end_recording_resources();
        } catch (...) {
            r->coreResources = s->runtime->end_recording_resources();
            if (s->runtime->reno) r->renoResources = s->runtime->reno->end_recording_resources();
            s->RequestHistoryReset(Session::ResetCancel);
            ForgetDirectPipeline(s, list); restoreCompute.restored = true;
            throw;
        }
        if (j.firstPass) {
            const VkImage first = s->runtime->first_pass_output();
            if (!first) throw std::runtime_error("direct first-pass output missing");
            auto firstTarget = DirectResource(s, s->firstResult, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, s->colourVk);
            imageBarrier(first, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
            imageBarrier(firstTarget.image, firstTarget.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
            VkImageBlit region {}; region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.srcOffsets[1] = region.dstOffsets[1] = {int32_t(w), int32_t(h), 1};
            vkCmdBlitImage(c, first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, firstTarget.image,
                          VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region, VK_FILTER_NEAREST);
            imageBarrier(first, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
            imageBarrier(firstTarget.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, firstTarget.after);
        }
        if (j.canonicalise) {
            imageBarrier(neural, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
            s->convert.DispatchUnconvert(c, s->convertPipeline, s->colourVk, w, h);
            imageBarrier(s->colourImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL);
        }
        imageBarrier(s->colourImage.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
        imageBarrier(target.image, target.before, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
        VkImageCopy output {}; output.srcSubresource = output.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}; output.extent = {w, h, 1};
        vkCmdCopyImage(c, s->colourImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, target.image,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &output);
        imageBarrier(target.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, target.after);
        imageBarrier(s->colourImage.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL);
        everything(VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
        ForgetDirectPipeline(s, list);
        restoreCompute.restored = true;
        r->directRecordFailed = false;
        j.enqueued = true; j.state = LMXXF_NR_JOB_CONSUMER_COMPLETE;
        r->directSerial = ++s->directRecorded;
        s->historyNetwork = r->network; s->historyFrame = r->frameId; s->historyTick = GetTickCount64();
        s->historyMotion = motion.image != VK_NULL_HANDLE;
        s->historyWidth = w; s->historyHeight = h; s->historyPasses = j.controls.passes;
        s->historyBits.store((s->historyBits.load() << 1) | uint64_t(consumed));
        s->historyCount.store(std::min(s->historyCount.load() + 1, kHistorySamples));
        s->infoInlinePath.store(3);
        s->directTimingUnavailable.store(s->runtime->reno ?
            r->directTimingSlot == MochizukiStageTiming::kUnavailable : !s->runtime->recording_timing_available());
        float prepEv = NAN;
        if (j.controls.preprocess.active() && j.controls.preprocess.exposure == 1)
            prepEv = s->runtime->preprocess_meter().first + j.controls.preprocess.bias_ev;
        s->infoPrepEv.store(prepEv);
        if (s->directRecorded <= 2 || s->directRecorded % 3600 == 0)
            nr::logf("[mochizuki] direct-record.end frame=%llu serial=%llu restored=layouts+ALL_COMMANDS+compute+meta",
                     (unsigned long long)r->frameId, (unsigned long long)r->directSerial);
        return int32_t(LMXXF_NR_OK);
    });
}
