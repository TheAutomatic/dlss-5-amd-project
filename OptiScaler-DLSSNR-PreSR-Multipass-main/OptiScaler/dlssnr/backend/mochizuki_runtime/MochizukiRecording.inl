// Product recording ABI v2. Included inside the runtime's private namespace.
// The host serializes every lifecycle call through RecordingMutex, including collection.
// Only the background builder and watchdog run independently of that lock.
Recording* FindRecording(Session* s, void* token)
{
    if (!s) return nullptr;
    const auto it = s->recordings.find(token);
    return it == s->recordings.end() ? nullptr : it->second.get();
}

struct BoundRecording
{
    Session& s;
    Recording& r;
    Job savedJob;
    FrameBuffers savedBuffers;
    std::shared_ptr<ProductRuntime> savedNetwork;
    NetworkKey savedNet;
    BoundRecording(Session& session, Recording& record) : s(session), r(record)
    {
        Job next = r.job; // Allocate controls before changing the active view.
        savedJob = std::move(s.job);
        savedBuffers = s.TakeBuffers();
        savedNetwork = s.runtime;
        savedNet = s.net;
        auto view = r.buffers;
        s.UseBuffers(view);
        s.runtime = r.network;
        s.net = r.net;
        s.job = std::move(next);
    }
    ~BoundRecording()
    {
        r.job = std::move(s.job);
        auto view = s.TakeBuffers();
        s.UseBuffers(savedBuffers);
        s.runtime = std::move(savedNetwork);
        s.net = savedNet;
        s.job = std::move(savedJob);
    }
};
#include "MochizukiDirectRecording.inl"


void LogAdmission(Session* s, const char* mode, const char* event)
{
    nr::logf("[mochizuki] %s %s recipe=%u in_flight=%u physical=%u",
             mode, event, s->RecipeCount(), s->InFlightRecordings(), s->recordingBuffersOutstanding);
}

void ObserveDirectCompletion(Session* s, Recording& r)
{
    if (!r.job.directRecording || !r.tail || r.executingQueue || r.uncertain ||
        r.completedExecutions == r.submittedExecutions) return;
    const auto completed = r.tail->GetCompletedValue();
    if (completed == UINT64_MAX || completed < r.tailValue) return;
    if (!r.directRecordFailed) s->directCompleted += r.submittedExecutions - r.completedExecutions;
    r.completedExecutions = r.submittedExecutions;
    // One pool contains only the latest execution. Certification is not a claim
    // that every covered execution supplied a timing sample.
    const auto pending = r.completedExecutions - r.harvestedExecutionOrdinal;
    if (!r.directRecordFailed && pending) {
        r.coalescedExecutions += pending - 1;
        ++r.harvestAttempts;
        bool accepted = false;
        if (!r.network->reno) {
            const auto before = r.network->gpu_timing_serial();
            r.network->harvest_recording_timings(r.coreResources);
            const auto serial = r.network->gpu_timing_serial();
            accepted = serial != before;
            if (r.network == s->runtime) {
                s->infoRecordingTimingReads.store(r.network->recording_timing_reads(), std::memory_order_relaxed);
                s->infoRecordingTimingRejected.store(r.network->recording_timing_rejected(), std::memory_order_relaxed);
                if (accepted && serial != r.network->harvestedTimingSerial && r.network->last_gpu_ms() > 0) {
                    r.network->harvestedTimingSerial = serial;
                    s->PushGpuSample(r.network->last_gpu_ms(), r.network->last_network_ms(),
                                     r.network->last_encode_ms(), r.network->last_transfer_ms());
                }
            }
        } else if (r.directTiming) {
            r.directTiming->HarvestRecording(r.directTimingSlot, r.directSerial,
                [s, &r, &accepted](const MochizukiStageTiming::Sample& sample) {
                    accepted = true;
                    if (r.network == s->runtime) s->PushGpuSample(sample.total, sample.network, -1.f, -1.f);
                });
        }
        if (accepted) ++r.acceptedSamples;
        else ++r.rejectedSamples; // Optional/unavailable/invalid timing never fails execution.
        r.harvestedExecutionOrdinal = r.completedExecutions;
    }
    if (s->directTimingLog)
        nr::logf("[mochizuki] direct timing-supply session_id=%p recipe_id=%llu submitted_executions=%llu certified_executions=%llu harvested_execution_ordinal=%llu harvest_attempts=%llu accepted_samples=%llu rejected_samples=%llu coalesced_executions=%llu tail_value=%llu completed=%llu last_prepare_tick=%llu",
                 static_cast<void*>(s), (unsigned long long)r.directSerial, (unsigned long long)r.submittedExecutions,
                 (unsigned long long)r.completedExecutions, (unsigned long long)r.harvestedExecutionOrdinal,
                 (unsigned long long)r.harvestAttempts, (unsigned long long)r.acceptedSamples,
                 (unsigned long long)r.rejectedSamples, (unsigned long long)r.coalescedExecutions,
                 (unsigned long long)r.tailValue, (unsigned long long)completed, (unsigned long long)s->lastFrameTick);
    if (s->directCompleted <= 2 || s->directCompleted % 3600 == 0)
        nr::logf("[mochizuki] direct consumer-completion frame=%llu tail=%llu completed=%llu",
                 (unsigned long long)r.frameId, (unsigned long long)r.tailValue,
                 (unsigned long long)s->directCompleted);
}

void ObserveDirectCompletions(Session* s)
{
    if (!s->vk.adoptedVkd3d) return;
    for (auto& [token, record] : s->recordings) ObserveDirectCompletion(s, *record);
}


int32_t PrepareFrame(void* context, const MochizukiNrFrameInfo* info, LmxxfNrJob* out)
{
    auto* s = static_cast<Session*>(context);
    // Split shares one buffer set and may keep many closed lists. Inline ahead
    // is limited by reservations still executing, not by retained recipes.
    // INLINE_QUEUE on the session is only permission for a side queue.
    if (!s || s->uncertainExecution)
        return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: execution unresolved");
    ObserveDirectCompletions(s);
    const bool inlineExec = info && (info->flags & MOCHIZUKI_NR_FRAME_FLAG_INLINE_EXECUTION) != 0;
    if (!inlineExec && !(info && (info->flags & MOCHIZUKI_NR_FRAME_FLAG_DIRECT_RECORDING)) && s->executing)
        return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: execution unresolved");
    if (inlineExec && s->InFlightRecordings() >= Session::kRecordingSlots)
    {
        char text[192] {};
        std::snprintf(text, sizeof text,
                      "PrepareFrame: inline slots are full mode=inline recipe=%u in_flight=%u physical=%u",
                      s->RecipeCount(), s->InFlightRecordings(), s->recordingBuffersOutstanding);
        LogAdmission(s, "inline", "reject");
        return Fail(LMXXF_NR_UNAVAILABLE, text);
    }
    const auto savedFrame = s->lastFrameId;
    const auto savedTick = s->lastFrameTick;
    const int32_t rc = CorePrepareFrame(context, info, out);
    if (rc != LMXXF_NR_OK) return rc;
    return Guard(s, [&] {
        if (!inlineExec && !s->vk.adoptedVkd3d) {
            s->SelectColourInput(s->job);
            s->SelectMotionInput(s->job);
            s->LogColourInput();
            s->LogMotionInput();
        }
        auto r = std::make_unique<Recording>();
        r->job = s->job;
        r->frameId = info->frame_id;
        r->colour = s->job.colour;
        r->motion = s->job.motion;
        r->depth = s->job.depth;
        r->exposureTexture = s->job.exposureTexture;
        r->network = s->runtime;
        r->net = s->net;
        r->inlineExecution = inlineExec;
        bool checkedOut = false;
        try {
            if (inlineExec || s->job.directRecording) {
                if (!s->CheckoutRecordingBuffers(r->buffers)) {
                    s->lastFrameId = savedFrame;
                    s->lastFrameTick = savedTick;
                    LogAdmission(s, "inline", "reject");
                    return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: recording buffers unavailable");
                }
                checkedOut = true;
                r->privateBuffers = true;
                // The host builds its SRV from this pointer before any later session swap.
                out->private_output = r->buffers.result;
                out->first_pass_output = r->buffers.firstResult;
            } else {
                auto view = s->TakeBuffers();
                r->buffers = view;
                s->UseBuffers(view);
            }
            s->recordings.emplace(out->handle, std::move(r));
            return int32_t(LMXXF_NR_OK);
        } catch (...) {
            if (checkedOut) s->RecycleRecordingBuffers(r->buffers);
            s->lastFrameId = savedFrame;
            s->lastFrameTick = savedTick;
            throw;
        }
    });
}

int32_t RejectLmxxfFrame(void* context, const LmxxfNrFrameInfo*, LmxxfNrJob*)
{
    auto* s = static_cast<Session*>(context);
    if (s && s->vk.adoptedVkd3d)
        return s->RejectAdoptedLegacyPath("Use MochizukiNrPrepareFrame from the current package");
    return Fail(LMXXF_NR_INVALID_ARGUMENT, "Use MochizukiNrPrepareFrame from the current package");
}

template<class F> int32_t WithRecording(void* context, void* token, F fn)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    if (!r || r->invalidated)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "unknown or invalidated recording");
    return Guard(s, [&] {
        BoundRecording bound(*s, *r);
        return fn(s, JobHandle(s->jobGen));
    });
}

int32_t RecordInputs(void* context, void* token, void* list)
{
    auto* s = static_cast<Session*>(context);
    if (s && s->vk.adoptedVkd3d)
        return s->RejectAdoptedLegacyPath("RecordInputs: adopted vkd3d requires direct recording");
    return WithRecording(context, token, [list](Session* s, void* j) { return CoreRecordInputs(s, j, list); });
}

int32_t RecordOutputs(void* context, void* token, void* list)
{
    auto* s = static_cast<Session*>(context);
    if (s && s->vk.adoptedVkd3d)
        return s->RejectAdoptedLegacyPath("RecordOutputs: adopted vkd3d requires direct recording");
    return WithRecording(context, token, [list](Session* s, void* j) { return CoreRecordOutputs(s, j, list); });
}

int32_t CheckExecutionDevice(Session* s, ID3D12DeviceChild* object, const char* phase)
{
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    const HRESULT hr = object ? object->GetDevice(IID_PPV_ARGS(&dev)) : E_POINTER;
    if (SUCCEEDED(hr) && DlssNr::Backend::IsRecordingDevice(s->device, s->toVk.fence, dev.Get()))
        return LMXXF_NR_OK;
    if (SUCCEEDED(hr) && s->vk.adoptedVkd3d &&
        DlssNr::Backend::IsRecordingDevice(
            s->device, s->pipelineInvalidator ? static_cast<ID3D12DeviceChild*>(s->pipelineInvalidator.Get()) : s->vk.interopQueue,
            dev.Get()))
        return LMXXF_NR_OK;
    Microsoft::WRL::ComPtr<IUnknown> expected, observed;
    if (s->device) s->device->QueryInterface(IID_PPV_ARGS(&expected));
    if (dev) dev->QueryInterface(IID_PPV_ARGS(&observed));
    char text[256] {};
    std::snprintf(text, sizeof text,
                  "%s: device mismatch object=%p expected=%p actual=%p identities=%p/%p GetDevice=0x%08lX",
                  phase, static_cast<void*>(object), static_cast<void*>(s->device), static_cast<void*>(dev.Get()),
                  static_cast<void*>(expected.Get()), static_cast<void*>(observed.Get()), static_cast<unsigned long>(hr));
    return Fail(LMXXF_NR_INVALID_ARGUMENT, text);
}

int32_t BeginRecordingExecution(void* context, void* token, void* queue)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    auto* q = static_cast<ID3D12CommandQueue*>(queue);
    if (!r || r->invalidated)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Begin: unknown or invalidated recording");
    if (!q || q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Begin: a direct execution queue is required");
    if (const auto rc = CheckExecutionDevice(s, q, "Begin queue"); rc != LMXXF_NR_OK) return rc;
    if (r->job.directRecording) {
        // A failed recording may already contain native commands. Session failure
        // stops new NR work, not submission of the game's already-written batch.
        if (s->uncertainExecution || (s->failed && !r->directRecordFailed))
            return Fail(LMXXF_NR_UNAVAILABLE, "direct Begin: previous execution unresolved");
        if ((!r->job.enqueued && !r->directRecordFailed) || r->executingQueue)
            return Fail(LMXXF_NR_UNAVAILABLE, "direct Begin: recording unavailable or overlapping replay");
        if (r->tail && r->tail->GetCompletedValue() == UINT64_MAX)
            return Fail(LMXXF_NR_DEVICE_LOST, "direct Begin: device removed");
        if (r->tail && r->tail->GetCompletedValue() < r->tailValue)
            return Fail(LMXXF_NR_UNAVAILABLE, "direct Begin: previous replay has not completed");
        // The preceding tail is complete. Read its pool before this retained
        // command list can reset/overwrite the timestamps in native submission.
        ObserveDirectCompletion(s, *r);
        if (s->previousTail && FAILED(q->Wait(s->previousTail.Get(), s->previousTailValue)))
            return Fail(LMXXF_NR_FAILED, "direct Begin: previous game consumer wait failed");
        r->executingQueue = q;
        ++r->executions;
        s->exposure.Begin(r->job.meterTicket);
        s->gameTexel.Begin(r->job.texelTicket);
        return LMXXF_NR_OK;
    }
    if (s->uncertainExecution || s->failed)
        return Fail(LMXXF_NR_UNAVAILABLE, "Begin: previous execution unresolved");
    if (!r->inlineExecution && s->executing)
        return Fail(LMXXF_NR_UNAVAILABLE, "Begin: execution already active");
    // previousTail is published only after a submitted End, so this wait is on a
    // fence the queue has already been asked to signal. Do not wait on the fence
    // of a recording that has not submitted yet.
    if (s->previousTail && FAILED(q->Wait(s->previousTail.Get(), s->previousTailValue)))
        return Fail(LMXXF_NR_FAILED, "Begin: previous consumer wait failed");
    r->executingQueue = q;
    s->exposure.Begin(r->job.meterTicket);
    s->gameTexel.Begin(r->job.texelTicket);
    r->job.enqueued = false;
    if (r->executions++) r->job.reset = r->job.normalizationReset = true; // replay is a discontinuity
    if (!r->inlineExecution) {
        s->executing = token;
        return LMXXF_NR_OK;
    }
    if (s->executing && s->executing != token) {
        s->enqueueOrder.push_back(token);
        return LMXXF_NR_OK;
    }
    s->executing = token;
    return LMXXF_NR_OK;
}

int32_t EnqueueHip(void* context, void* token, void* queue)
{
    auto* s = static_cast<Session*>(context);
    if (s && s->vk.adoptedVkd3d)
        return s->RejectAdoptedLegacyPath("direct recording executes only in the game submission");
    auto* r = FindRecording(s, token);
    if (r && r->job.directRecording)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "direct recording executes only in the game submission");
    if (!r || s->executing != token || (!r->inlineExecution && r->executingQueue.Get() != queue))
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Enqueue: Begin on this queue is required");
    if (r->inlineExecution) {
        auto* q = static_cast<ID3D12CommandQueue*>(queue);
        if (!q || (q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT && q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_COMPUTE))
            return Fail(LMXXF_NR_INVALID_ARGUMENT, "Enqueue: direct/compute side queue required");
        if (const auto rc = CheckExecutionDevice(s, q, "Enqueue queue"); rc != LMXXF_NR_OK) return rc;
    }
    if (r->job.enqueued) return LMXXF_NR_OK;
    const auto now = GetTickCount64();
    const bool continuous = s->historyMotion && s->historyNetwork.lock() == r->network &&
        r->frameId == s->historyFrame + 1 && now - s->historyTick < 250 &&
        r->job.width == s->historyWidth && r->job.height == s->historyHeight &&
        r->job.controls.passes == s->historyPasses;
    const bool reset = s->resetPending.exchange(false);
    const uint32_t resetReasons = reset ? s->resetReasons.exchange(0) : 0;
    r->job.normalizationReset |= (resetReasons & (Session::ResetQueue | Session::ResetCancel |
        Session::ResetExternal | Session::ResetEnqueue)) != 0;
    if (r->job.reset || reset || !continuous) {
        const auto n = ++s->recordingHistoryResets;
        if (n <= 5 || n % 3600 == 0)
            nr::logf("[mochizuki] history reset mode=%s %llu: frame=%llu previous=%llu requested=%u pending=%u reasons=0x%X (startup=1 queue=2 network=4 extent=8 passes=16 noMotion=32 passthrough=64 cancel=128 external=256 enqueue=512) motion=%u networkChanged=%u frameGap=%u timeGap=%u geometryChanged=%u passesChanged=%u recipe=%u in_flight=%u physical=%u",
                     r->inlineExecution ? "inline" : "split",
                     static_cast<unsigned long long>(n), static_cast<unsigned long long>(r->frameId),
                     static_cast<unsigned long long>(s->historyFrame), unsigned(r->job.reset), unsigned(reset), resetReasons,
                     unsigned(s->historyMotion), unsigned(s->historyNetwork.lock() != r->network),
                     unsigned(r->frameId != s->historyFrame + 1), unsigned(now - s->historyTick >= 250),
                     unsigned(r->job.width != s->historyWidth || r->job.height != s->historyHeight),
                     unsigned(r->job.controls.passes != s->historyPasses),
                     s->RecipeCount(), s->InFlightRecordings(), s->recordingBuffersOutstanding);
    }
    r->job.reset = r->job.reset || reset || !continuous;
    const int32_t rc = WithRecording(context, token, [queue](Session* s, void* j) { return CoreEnqueueHip(s, j, queue); });
    if (rc == LMXXF_NR_OK)
    {
        s->historyNetwork = r->network;
        s->historyFrame = r->frameId;
        s->historyTick = now;
        s->historyWidth = r->job.width;
        s->historyHeight = r->job.height;
        s->historyPasses = r->job.controls.passes;
        s->historyMotion = r->job.motion != nullptr;
    }
    else
    {
        s->historyNetwork.reset();
        s->RequestHistoryReset(Session::ResetEnqueue);
    }
    if (s->enqueueCalls <= 5 || s->enqueueCalls % 3600 == 0)
        nr::logf("[mochizuki] %s enqueue frame=%llu rc=%d reset=%u passes=%d calls=%llu recipe=%u in_flight=%u physical=%u",
                 r->inlineExecution ? "inline" : "split",
                 static_cast<unsigned long long>(r->frameId), rc, unsigned(r->job.reset),
                 r->job.controls.passes, static_cast<unsigned long long>(s->enqueueCalls),
                 s->RecipeCount(), s->InFlightRecordings(), s->recordingBuffersOutstanding);
    return rc;
}

int32_t EndRecordingExecution(void* context, void* token, void* queue, uint32_t submitted,
                              void* fence, uint64_t value, int32_t signalStatus)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    if (!r || (!r->job.directRecording && s->executing != token) || r->executingQueue.Get() != queue)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "End: no matching Begin");
    r->ended = true;
    if (!r->job.directRecording) {
        s->executing = nullptr;
        if (!s->enqueueOrder.empty()) {
            s->executing = s->enqueueOrder.front();
            s->enqueueOrder.pop_front();
        }
    }
    r->executingQueue.Reset();
    if (submitted)
    {
        if ((submitted & ~3u) || !fence || !value || FAILED(signalStatus))
        {
            r->uncertain = s->uncertainExecution = true;
            return Fail(LMXXF_NR_UNAVAILABLE, "End: submitted work has no proven tail signal");
        }
        auto* f = static_cast<ID3D12Fence*>(fence);
        if (const auto rc = CheckExecutionDevice(s, f, "End fence"); rc != LMXXF_NR_OK)
        {
            r->uncertain = s->uncertainExecution = true;
            return rc;
        }
        r->tail = f;
        r->tailValue = value;
        ++r->submittedExecutions;
        // Do not publish measurements for executions that never enqueued. Legacy GPU-armed mode
        // can still enqueue after a missed capture: this check alone does not certify its predicated
        // measurement or input contents. See docs/architecture/nr-capture-proof-deferral.md.
        if (r->job.enqueued) {
            s->exposure.Submitted(r->job.meterTicket, f, value);
            s->gameTexel.Submitted(r->job.texelTicket, f, value);
        }
        s->previousTail = f;
        s->previousTailValue = value;
        if (r->job.directRecording && !r->directRecordFailed) {
            ++s->frames;
            ++s->directSubmitted;
            s->dispatches.store(r->recordedDispatches);
            s->historyBits.store((s->historyBits.load() << 1) | uint64_t(r->recordedHistoryConsumed));
            s->historyCount.store(std::min(s->historyCount.load() + 1, kHistorySamples));
            if (s->directTimingLog)
                nr::logf("[mochizuki] direct executed-generation session_id=%p recipe_id=%llu record_network_id=%llu record_model=%u current_network_id=%llu current_model=%u execution_ordinal=%llu admission_ordinal=%llu tail_value=%llu record_history_consumed=%u record_dispatches=%u",
                         static_cast<void*>(s), (unsigned long long)r->directSerial,
                         (unsigned long long)r->network->id, unsigned(r->net.networkModel),
                         (unsigned long long)(s->runtime ? s->runtime->id : 0), unsigned(s->net.networkModel),
                         (unsigned long long)r->submittedExecutions, (unsigned long long)r->executions,
                         (unsigned long long)value, unsigned(r->recordedHistoryConsumed), r->recordedDispatches);
            if (r->directTiming)
                r->directTiming->Submitted(r->directTimingSlot, r->directSerial);
            if (s->directSubmitted <= 2 || s->directSubmitted % 3600 == 0)
                nr::logf("[mochizuki] direct game-submission frame=%llu execution=%llu tail=%llu actual_nr_frames=%llu",
                         (unsigned long long)r->frameId, (unsigned long long)r->submittedExecutions,
                         (unsigned long long)value, (unsigned long long)s->frames.load());
        }
    }
    return LMXXF_NR_OK;
}

int32_t InvalidateRecording(void* context, void* token)
{
    auto* r = FindRecording(static_cast<Session*>(context), token);
    if (!r) return Fail(LMXXF_NR_INVALID_ARGUMENT, "Invalidate: unknown recording");
    r->invalidated = true;
    return LMXXF_NR_OK;
}

int32_t CollectRecording(void* context, void* token)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    if (!r) return Fail(LMXXF_NR_INVALID_ARGUMENT, "Collect: unknown recording");
    if (!r->invalidated || r->executingQueue || s->executing == token) return LMXXF_NR_UNAVAILABLE;
    if (std::find(s->enqueueOrder.begin(), s->enqueueOrder.end(), token) != s->enqueueOrder.end())
        return LMXXF_NR_UNAVAILABLE;
    // Vulkan has its own device. A D3D12 removal alone is insufficient evidence
    // that Vulkan stopped touching imported memory: retain the session on loss.
    if (r->uncertain || s->deviceLost || FAILED(s->device->GetDeviceRemovedReason()))
        return LMXXF_NR_UNAVAILABLE;
    if (r->tail)
    {
        const uint64_t done = r->tail->GetCompletedValue();
        if (done == UINT64_MAX || done < r->tailValue) return LMXXF_NR_UNAVAILABLE;
    }
    if (!r->job.directRecording && !r->inlineExecution && r->job.vkFinished)
    {
        uint64_t done = 0;
        if (vkGetSemaphoreCounterValue(s->vk.device, s->fromVk.semaphore, &done) != VK_SUCCESS ||
            done == UINT64_MAX || done < r->job.vkFinished)
            return LMXXF_NR_UNAVAILABLE;
    }
    ObserveDirectCompletions(s);
    if (r->job.directRecording && !r->tail) s->RequestHistoryReset(Session::ResetCancel);
    const bool recycle = r->privateBuffers;
    FrameBuffers buffers = std::move(r->buffers);
    s->recordings.erase(token);
    // A split recording holds a view of the session set. Recycling it would
    // pool that view and let a later checkout alias the live frame.
    if (recycle) s->RecycleRecordingBuffers(buffers);
    return LMXXF_NR_OK;
}

int32_t CancelUnsubmitted(void* context, void* token)
{ return InvalidateRecording(context, token); }
int32_t Retire(void*, void*)
{ return Fail(LMXXF_NR_INVALID_ARGUMENT, "Use InvalidateRecording and CollectRecording"); }
int32_t Poll(void* context, void* token, uint32_t* state)
{
    auto* r = FindRecording(static_cast<Session*>(context), token);
    if (!r || !state) return LMXXF_NR_INVALID_ARGUMENT;
    *state = r->job.state;
    return LMXXF_NR_OK;
}
int32_t GetTimings(void* context, LmxxfNrTimings* out)
{
    auto* s = static_cast<Session*>(context);
    if (!s || !out || out->struct_size != sizeof(*out)) return LMXXF_NR_INVALID_ARGUMENT;
    *out = { sizeof(*out) };
    float median = 0, p95 = 0;
    // The legacy network_ms ABI slot is Vulkan core/stage TOTAL. Pure-network
    // quantiles are explicit MochizukiNrInfo fields; do not change this contract.
    s->NetworkMs(median, p95);
    out->valid = median > 0;
    out->network_ms = median;
    out->frame_id = s->frames;
    return LMXXF_NR_OK;
}
