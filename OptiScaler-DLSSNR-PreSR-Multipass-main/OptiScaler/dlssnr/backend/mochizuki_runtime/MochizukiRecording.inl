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
    std::shared_ptr<nr::Runtime> savedNetwork;
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

int32_t PrepareFrame(void* context, const MochizukiNrFrameInfo* info, LmxxfNrJob* out)
{
    auto* s = static_cast<Session*>(context);
    if (!s || s->executing || s->uncertainExecution)
        return Fail(LMXXF_NR_UNAVAILABLE, "PrepareFrame: execution unresolved");
    const int32_t rc = CorePrepareFrame(context, info, out);
    if (rc != LMXXF_NR_OK) return rc;
    return Guard(s, [&] {
        auto r = std::make_unique<Recording>();
        r->job = s->job;
        r->frameId = info->frame_id;
        r->colour = s->job.colour;
        r->motion = s->job.motion;
        r->network = s->runtime;
        r->net = s->net;
        auto view = s->TakeBuffers();
        r->buffers = view;
        s->UseBuffers(view);
        s->recordings.emplace(out->handle, std::move(r));
        return int32_t(LMXXF_NR_OK);
    });
}

int32_t RejectLmxxfFrame(void*, const LmxxfNrFrameInfo*, LmxxfNrJob*)
{ return Fail(LMXXF_NR_INVALID_ARGUMENT, "Use MochizukiNrPrepareFrame from the current package"); }

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
{ return WithRecording(context, token, [list](Session* s, void* j) { return CoreRecordInputs(s, j, list); }); }

int32_t RecordOutputs(void* context, void* token, void* list)
{ return WithRecording(context, token, [list](Session* s, void* j) { return CoreRecordOutputs(s, j, list); }); }

int32_t CheckExecutionDevice(Session* s, ID3D12DeviceChild* object, const char* phase)
{
    Microsoft::WRL::ComPtr<ID3D12Device> dev;
    const HRESULT hr = object ? object->GetDevice(IID_PPV_ARGS(&dev)) : E_POINTER;
    if (SUCCEEDED(hr) && DlssNr::Backend::IsRecordingDevice(s->device, s->toVk.fence, dev.Get()))
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
    if (!r || r->invalidated || s->executing)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Begin: unknown/invalidated recording or execution already active");
    if (!q || q->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Begin: a direct execution queue is required");
    if (const auto rc = CheckExecutionDevice(s, q, "Begin queue"); rc != LMXXF_NR_OK) return rc;
    if (s->uncertainExecution || s->failed)
        return Fail(LMXXF_NR_UNAVAILABLE, "Begin: previous execution unresolved");
    // This must precede producer submission: it protects shared input and output
    // against a preceding execution on another queue, including its SR consumer.
    if (s->previousTail && FAILED(q->Wait(s->previousTail.Get(), s->previousTailValue)))
        return Fail(LMXXF_NR_FAILED, "Begin: previous consumer wait failed");
    r->executingQueue = q;
    r->job.enqueued = false;
    if (r->executions++) r->job.reset = true; // A replay is a history discontinuity.
    s->executing = token;
    return LMXXF_NR_OK;
}

int32_t EnqueueHip(void* context, void* token, void* queue)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    if (!r || s->executing != token || r->executingQueue.Get() != queue)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "Enqueue: Begin on this queue is required");
    if (r->job.enqueued) return LMXXF_NR_OK;
    const auto now = GetTickCount64();
    const bool continuous = s->historyMotion && s->historyNetwork.lock() == r->network &&
        r->frameId == s->historyFrame + 1 && now - s->historyTick < 250 &&
        r->job.width == s->historyWidth && r->job.height == s->historyHeight &&
        r->job.controls.passes == s->historyPasses;
    const bool reset = s->resetPending.exchange(false);
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
        s->resetPending = true;
    }
    return rc;
}

int32_t EndRecordingExecution(void* context, void* token, void* queue, uint32_t submitted,
                              void* fence, uint64_t value, int32_t signalStatus)
{
    auto* s = static_cast<Session*>(context);
    auto* r = FindRecording(s, token);
    if (!r || s->executing != token || r->executingQueue.Get() != queue)
        return Fail(LMXXF_NR_INVALID_ARGUMENT, "End: no matching Begin");
    s->executing = nullptr;
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
        s->previousTail = f;
        s->previousTailValue = value;
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
    if (!r->invalidated || r->executingQueue) return LMXXF_NR_UNAVAILABLE;
    // Vulkan has its own device. A D3D12 removal alone is insufficient evidence
    // that Vulkan stopped touching imported memory: retain the session on loss.
    if (r->uncertain || s->deviceLost || FAILED(s->device->GetDeviceRemovedReason()))
        return LMXXF_NR_UNAVAILABLE;
    if (r->tail)
    {
        const uint64_t done = r->tail->GetCompletedValue();
        if (done == UINT64_MAX || done < r->tailValue) return LMXXF_NR_UNAVAILABLE;
    }
    s->recordings.erase(token);
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
    s->NetworkMs(median, p95);
    out->valid = median > 0;
    out->network_ms = median;
    out->frame_id = s->frames;
    return LMXXF_NR_OK;
}
