#include "../../OptiScaler-DLSSNR-PreSR-Multipass-main/OptiScaler/inputs/FG/Streamline_FG_ActivationGate.h"

#include <cstdio>
#include <cstdlib>
#include <initializer_list>

namespace
{
using Gate = StreamlineFGActivationGate;
using Action = Gate::Action;

const char* currentTest = "";
unsigned checks = 0;

void Check(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition)
    {
        std::fprintf(stderr, "FAIL %s:%d: %s\n", currentTest, line, expression);
        std::exit(EXIT_FAILURE);
    }
}

#define CHECK(expression) Check((expression), #expression, __LINE__)

void Resources(Gate& gate, uint32_t frame)
{
    gate.ReportDepth(frame, 1920, 1080);
    gate.ReportMotionVectors(frame, 1920, 1080);
}

void ReadyFrame(Gate& gate, uint32_t frame)
{
    Resources(gate, frame);
    gate.ReportEvaluate(frame);
}

void Prepare(Gate& gate)
{
    gate.SetRequested(true);
    gate.MarkPresentBoundary();
    gate.ReportConstants(0, false);
    gate.ReportInterpolationSize(1920, 1080);
}

void NoGameRequest()
{
    Gate gate;
    gate.MarkPresentBoundary();
    gate.ReportConstants(1, false);
    gate.ReportInterpolationSize(1920, 1080);
    ReadyFrame(gate, 1);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);
    CHECK(!gate.ConfirmActivation(true, false, false, 1));
    CHECK(gate.Evaluate(true, false, true, false, 1) == Action::Deactivate);
}

void InitialPresent()
{
    Gate gate;
    gate.SetRequested(true);
    gate.ReportConstants(1, false);
    gate.ReportInterpolationSize(1920, 1080);
    ReadyFrame(gate, 1);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);

    // The first Present establishes a boundary; it cannot also complete a ready frame.
    Resources(gate, 1);
    gate.ReportPresent(1);
    CHECK(gate.GetDiagnostics().hasPresentBoundary);
    CHECK(gate.ReadyFrameCount() == 0);
    ReadyFrame(gate, 2);
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::Activate);
}

void Thresholds()
{
    for (const uint32_t threshold : {1u, 30u, 300u})
    {
        Gate gate;
        Prepare(gate);
        CHECK(gate.Evaluate(true, false, false, false, threshold) == Action::None);
        for (uint32_t frame = 1; frame <= threshold; ++frame)
        {
            // Constants and interpolation size persist; depth and motion belong to each frame.
            ReadyFrame(gate, frame);
            CHECK(gate.ReadyFrameCount() == frame);
            const auto expected = frame == threshold ? Action::Activate : Action::None;
            CHECK(gate.Evaluate(true, false, false, false, threshold) == expected);
            if (frame < threshold)
                CHECK(!gate.ConfirmActivation(true, false, false, threshold));
        }
        CHECK(gate.ConfirmActivation(true, false, false, threshold));
        gate.ReportActivationResult(true);
        CHECK(gate.GetDiagnostics().active);
        CHECK(gate.Evaluate(true, false, true, false, threshold) == Action::None);
    }
}

void MissingInputs()
{
    for (unsigned missing = 0; missing < 4; ++missing)
    {
        Gate gate;
        gate.SetRequested(true);
        gate.MarkPresentBoundary();
        if (missing != 0)
            gate.ReportConstants(1, false);
        if (missing != 1)
            gate.ReportInterpolationSize(1920, 1080);
        if (missing != 2)
            gate.ReportDepth(1, 1920, 1080);
        if (missing != 3)
            gate.ReportMotionVectors(1, 1920, 1080);
        gate.ReportEvaluate(1);
        CHECK(gate.ReadyFrameCount() == 0);
        CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);
        CHECK(!gate.ConfirmActivation(true, false, false, 1));

        gate.ReportConstants(2, false);
        gate.ReportInterpolationSize(1920, 1080);
        ReadyFrame(gate, 2);
        CHECK(gate.ReadyFrameCount() == 1);
    }
}

void InvalidSizes()
{
    for (unsigned input = 0; input < 3; ++input)
    {
        for (const bool zeroWidth : {false, true})
        {
            Gate gate;
            Prepare(gate);
            const uint64_t width = zeroWidth ? 0u : 1920u;
            const uint32_t height = zeroWidth ? 1080u : 0u;
            gate.ReportDepth(1, input == 0 ? width : 1920, input == 0 ? height : 1080);
            gate.ReportMotionVectors(1, input == 1 ? width : 1920, input == 1 ? height : 1080);
            if (input == 2)
                gate.ReportInterpolationSize(width, height);
            gate.ReportEvaluate(1);
            CHECK(gate.ReadyFrameCount() == 0);
            CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);
        }
    }
}

void MissingFrameBreaksSequence()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    gate.ReportDepth(2, 1920, 1080);
    gate.ReportEvaluate(2);
    CHECK(gate.ReadyFrameCount() == 0);
    // Motion for a different frame must not combine with the preceding depth tag.
    gate.ReportMotionVectors(3, 1920, 1080);
    gate.ReportEvaluate(3);
    CHECK(gate.ReadyFrameCount() == 0);
    ReadyFrame(gate, 4);
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
    ReadyFrame(gate, 5);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
}

void InterleavedFramesDoNotMixInputs()
{
    constexpr uint32_t older = 100;
    constexpr uint32_t newer = older + 1;
    for (const bool olderDepth : {false, true})
    {
        for (const bool completeOnPresent : {false, true})
        {
            Gate gate;
            Prepare(gate);
            // The newer constants can advance Sl_Inputs' current frame before an older tag arrives.
            // Explicit resource tokens must still retain their own frame IDs.
            gate.ReportConstants(newer, false);
            gate.ReportDepth(olderDepth ? older : newer, 1920, 1080);
            gate.ReportMotionVectors(olderDepth ? newer : older, 1920, 1080);
            if (completeOnPresent)
                gate.ReportPresent(newer);
            else
                gate.ReportEvaluate(newer);

            const auto diagnostics = gate.GetDiagnostics();
            CHECK(diagnostics.lastEvaluatedFrameId == newer);
            CHECK(diagnostics.lastEvaluateHadDepth == !olderDepth);
            CHECK(diagnostics.lastEvaluateHadMotionVectors == olderDepth);
            CHECK(gate.ReadyFrameCount() == 0);
            CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);
            CHECK(!gate.ConfirmActivation(true, false, false, 1));
        }
    }
}

void InterleavedCompleteFramesCountSeparately()
{
    constexpr uint32_t older = 100;
    constexpr uint32_t newer = older + 1;
    Gate gate;
    Prepare(gate);
    gate.ReportConstants(newer, false);
    gate.ReportDepth(older, 1920, 1080);
    gate.ReportMotionVectors(newer, 1920, 1080);
    gate.ReportMotionVectors(older, 1920, 1080);
    gate.ReportDepth(newer, 1920, 1080);

    gate.ReportEvaluate(older);
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(gate.GetDiagnostics().lastEvaluateHadDepth);
    CHECK(gate.GetDiagnostics().lastEvaluateHadMotionVectors);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
    gate.ReportPresent(older);
    CHECK(gate.ReadyFrameCount() == 1);

    gate.ReportEvaluate(newer);
    CHECK(gate.ReadyFrameCount() == 2);
    CHECK(gate.GetDiagnostics().lastEvaluateHadDepth);
    CHECK(gate.GetDiagnostics().lastEvaluateHadMotionVectors);
    gate.ReportPresent(newer);
    CHECK(gate.ReadyFrameCount() == 2);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
    CHECK(gate.ConfirmActivation(true, false, false, 2));
}

void RepeatedCallbacks()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    for (unsigned repeat = 0; repeat < 5; ++repeat)
    {
        ReadyFrame(gate, 1);
        gate.ReportPresent(1);
        gate.SetRequested(true);
        CHECK(gate.ReadyFrameCount() == 1);
        CHECK(gate.Evaluate(true, false, false, false, 30) == Action::None);
    }
    ReadyFrame(gate, 2);
    CHECK(gate.ReadyFrameCount() == 2);
}

void FrameDiscontinuity()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    ReadyFrame(gate, 2);
    ReadyFrame(gate, 4);
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(gate.Evaluate(true, false, false, false, 3) == Action::None);
    // An out-of-order callback starts a new sequence too.
    ReadyFrame(gate, 3);
    CHECK(gate.ReadyFrameCount() == 1);
    // Frame 4 already completed above; its duplicate completion is ignored
    // instead of restarting the sequence (consecutive head stays at 3).
    ReadyFrame(gate, 4);
    CHECK(gate.GetDiagnostics().lastEvaluatedFrameId == 4);
    CHECK(gate.ReadyFrameCount() == 1);
    ReadyFrame(gate, 5);
    CHECK(gate.ReadyFrameCount() == 1);
    ReadyFrame(gate, 6);
    CHECK(gate.ReadyFrameCount() == 2);
    ReadyFrame(gate, 7);
    CHECK(gate.ReadyFrameCount() == 3);
    CHECK(gate.Evaluate(true, false, false, false, 3) == Action::Activate);
}

void DelayedPresentDuplicateDoesNotRewind()
{
    // Finding 1: Evaluate(N), Evaluate(N+1), Present(N), Evaluate(N+2), Present(N+1)
    // previously reset progress on every delayed Present.
    Gate gate;
    Prepare(gate);
    Resources(gate, 100);
    Resources(gate, 101);
    Resources(gate, 102);
    gate.ReportEvaluate(100);
    CHECK(gate.ReadyFrameCount() == 1);
    gate.ReportEvaluate(101);
    CHECK(gate.ReadyFrameCount() == 2);
    gate.ReportPresent(100);
    CHECK(gate.ReadyFrameCount() == 2);
    gate.ReportEvaluate(102);
    CHECK(gate.ReadyFrameCount() == 3);
    CHECK(gate.Evaluate(true, false, false, false, 3) == Action::Activate);
    gate.ReportPresent(101);
    // A duplicate completion after the decision must not disturb the gate.
    CHECK(gate.ConfirmActivation(true, false, false, 3));
}

void DuplicateCompletionDoesNotRestart()
{
    // Finding 1, discontinuity refinement: an already-completed frame completing
    // again is a duplicate, not a discontinuity.
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    ReadyFrame(gate, 2);
    gate.ReportPresent(1);
    gate.ReportPresent(2);
    gate.ReportEvaluate(2);
    CHECK(gate.ReadyFrameCount() == 2);
    // A genuine forward gap still restarts the sequence.
    ReadyFrame(gate, 5);
    CHECK(gate.ReadyFrameCount() == 1);
    ReadyFrame(gate, 6);
    CHECK(gate.ReadyFrameCount() == 2);
    ReadyFrame(gate, 7);
    CHECK(gate.Evaluate(true, false, false, false, 3) == Action::Activate);
}

void NoRenderSubmitActivatesViaPresent()
{
    // Finding 2: a game that never sends RenderSubmit markers still reaches the
    // gated decision from Present completion (markPresent dispatches it).
    constexpr uint32_t threshold = 3;
    Gate gate;
    gate.SetRequested(true);
    gate.ReportConstants(1, false);
    gate.ReportInterpolationSize(1920, 1080);
    for (uint32_t frame = 1; frame <= threshold; ++frame)
    {
        Resources(gate, frame);
        gate.ReportPresent(frame); // markPresent seam: completion + boundary
        // The first Present only establishes the boundary and cannot count.
        CHECK(gate.ReadyFrameCount() == frame - 1);
        // The Present-path dispatch runs the same decision as RenderSubmitStart.
        CHECK(gate.Evaluate(true, false, false, false, threshold) == Action::None);
    }
    Resources(gate, threshold + 1);
    gate.ReportPresent(threshold + 1);
    CHECK(gate.ReadyFrameCount() == threshold);
    CHECK(gate.Evaluate(true, false, false, false, threshold) == Action::Activate);
    CHECK(gate.ConfirmActivation(true, false, false, threshold));
}

void ResetConstants()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    gate.ReportConstants(2, true);
    CHECK(gate.ReadyFrameCount() == 0);
    ReadyFrame(gate, 2);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(!gate.ConfirmActivation(true, false, false, 1));
    gate.ReportConstants(3, false);
    ReadyFrame(gate, 3);
    CHECK(gate.ReadyFrameCount() == 1);
    ReadyFrame(gate, 4);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
}

void GameOffOn()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    CHECK(gate.ConfirmActivation(true, false, false, 1));
    gate.ReportActivationResult(true);
    gate.SetRequested(false);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(!gate.GetDiagnostics().requested);
    CHECK(gate.Evaluate(true, false, true, false, 1) == Action::Deactivate);
    CHECK(!gate.ConfirmActivation(true, false, false, 1));

    gate.SetRequested(true);
    CHECK(!gate.GetDiagnostics().hasPresentBoundary);
    ReadyFrame(gate, 2);
    CHECK(gate.ReadyFrameCount() == 0);
    gate.ReportPresent(2);
    ReadyFrame(gate, 3);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::Activate);
}

void PauseAndActivationBlock()
{
    for (const bool paused : {false, true})
    {
        Gate gate;
        Prepare(gate);
        ReadyFrame(gate, 1);
        const bool blocked = !paused; // The host combines feature and swapchain changes into this flag.
        CHECK(gate.Evaluate(true, blocked, false, paused, 1) == Action::None);
        CHECK(gate.ReadyFrameCount() == 0);
        CHECK(!gate.ConfirmActivation(true, blocked, paused, 1));
        CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
        gate.MarkPresentBoundary();
        ReadyFrame(gate, 2);
        CHECK(gate.ReadyFrameCount() == 1);
        CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
        ReadyFrame(gate, 3);
        CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
        CHECK(gate.ConfirmActivation(true, false, false, 2));
        gate.ReportActivationResult(true);
        CHECK(gate.Evaluate(true, blocked, true, paused, 2) == Action::None);
    }
}

void FeatureOffOnKeepsGameRequest()
{
    for (const bool wasActive : {false, true})
    {
        Gate gate;
        Prepare(gate);
        ReadyFrame(gate, 1);
        if (wasActive)
        {
            CHECK(gate.ConfirmActivation(true, false, false, 1));
            gate.ReportActivationResult(true);
        }
        CHECK(gate.Evaluate(false, false, wasActive, false, 2) ==
              (wasActive ? Action::Deactivate : Action::None));
        CHECK(gate.ReadyFrameCount() == 0);
        CHECK(gate.GetDiagnostics().requested);
        CHECK(!gate.ConfirmActivation(false, false, false, 1));

        // No new SetRequested call: the game left its FG switch on throughout.
        CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
        gate.MarkPresentBoundary();
        ReadyFrame(gate, 2);
        CHECK(gate.ReadyFrameCount() == 1);
        CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
        ReadyFrame(gate, 3);
        CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
    }
}

void ActivationFailure()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    ReadyFrame(gate, 2);
    CHECK(gate.ConfirmActivation(true, false, false, 2));
    gate.ReportActivationResult(false);
    CHECK(!gate.GetDiagnostics().active);
    CHECK(!gate.GetDiagnostics().hasPresentBoundary);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
    ReadyFrame(gate, 3);
    CHECK(gate.ReadyFrameCount() == 0);
    gate.ReportPresent(3);
    ReadyFrame(gate, 4);
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(!gate.ConfirmActivation(true, false, false, 2));
    ReadyFrame(gate, 5);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
}

void RecheckActivationConditions()
{
    for (unsigned changed = 0; changed < 4; ++changed)
    {
        Gate gate;
        Prepare(gate);
        ReadyFrame(gate, 1);
        CHECK(gate.Evaluate(true, false, false, false, 1) == Action::Activate);
        if (changed == 0)
            gate.SetRequested(false);
        CHECK(!gate.ConfirmActivation(changed != 1, changed == 2, changed == 3, 1));
        CHECK(!gate.GetDiagnostics().active);
    }
}

void ThresholdChanges()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    CHECK(gate.Evaluate(true, false, false, false, 30) == Action::None);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::Activate);
    // A raised threshold between decision and activation must be checked again.
    CHECK(!gate.ConfirmActivation(true, false, false, 30));
    CHECK(gate.Evaluate(true, false, false, false, 300) == Action::None);
    ReadyFrame(gate, 2);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
    CHECK(gate.ConfirmActivation(true, false, false, 2));
}

void DisabledGateBypass()
{
    Gate gate(false);
    gate.SetRequested(true);
    gate.ReportConstants(1, false);
    gate.ReportInterpolationSize(1920, 1080);
    gate.MarkPresentBoundary();
    ReadyFrame(gate, 1);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(gate.Evaluate(true, false, false, false, 0) == Action::None);
    CHECK(gate.Evaluate(false, true, true, true, 0) == Action::None);
    CHECK(!gate.ConfirmActivation(true, false, false, 0));
    CHECK(gate.GetDiagnostics().requested);
}

void GateOffOnResets()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    CHECK(gate.ReadyFrameCount() == 1);
    gate.SetEnabled(false);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(!gate.GetDiagnostics().hasPresentBoundary);
    CHECK(gate.GetDiagnostics().requested);
    ReadyFrame(gate, 2);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);
    CHECK(!gate.ConfirmActivation(true, false, false, 1));

    // A zero -> positive setting change must collect fresh ready frames and a Present.
    gate.SetEnabled(true);
    ReadyFrame(gate, 3);
    CHECK(gate.ReadyFrameCount() == 0);
    gate.ReportPresent(3);
    ReadyFrame(gate, 4);
    gate.SetEnabled(true); // Repeating the same setting must not discard progress.
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
    ReadyFrame(gate, 5);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
}

void OutputLossRestartsReadiness()
{
    Gate gate;
    Prepare(gate);
    ReadyFrame(gate, 1);
    CHECK(gate.ConfirmActivation(true, false, false, 1));
    gate.ReportActivationResult(true);
    CHECK(gate.Evaluate(true, false, true, false, 1) == Action::None);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::None);
    CHECK(gate.ReadyFrameCount() == 0);
    CHECK(!gate.GetDiagnostics().hasPresentBoundary);
    gate.ReportPresent(2);
    ReadyFrame(gate, 3);
    CHECK(gate.Evaluate(true, false, false, false, 1) == Action::Activate);
}

void PresentOnlyFrames()
{
    Gate gate;
    gate.SetRequested(true);
    gate.ReportConstants(1, false);
    gate.ReportInterpolationSize(1920, 1080);
    Resources(gate, 1);
    gate.ReportPresent(1);
    CHECK(gate.ReadyFrameCount() == 0);
    Resources(gate, 2);
    gate.ReportPresent(2);
    gate.ReportPresent(2);
    CHECK(gate.ReadyFrameCount() == 1);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::None);
    gate.ReportPresent(3);
    CHECK(gate.ReadyFrameCount() == 0);
    Resources(gate, 4);
    gate.ReportPresent(4);
    CHECK(gate.ReadyFrameCount() == 1);
    Resources(gate, 5);
    gate.ReportPresent(5);
    CHECK(gate.ReadyFrameCount() == 2);
    CHECK(gate.Evaluate(true, false, false, false, 2) == Action::Activate);
}
} // namespace

int main()
{
    const struct Test
    {
        const char* name;
        void (*run)();
    } tests[] = {
        {"no_game_request", NoGameRequest},
        {"initial_present", InitialPresent},
        {"thresholds_1_30_300", Thresholds},
        {"missing_inputs", MissingInputs},
        {"invalid_sizes", InvalidSizes},
        {"missing_frame_breaks_sequence", MissingFrameBreaksSequence},
        {"interleaved_frames_do_not_mix_inputs", InterleavedFramesDoNotMixInputs},
        {"interleaved_complete_frames_count_separately", InterleavedCompleteFramesCountSeparately},
        {"repeated_callbacks", RepeatedCallbacks},
        {"frame_discontinuity", FrameDiscontinuity},
        {"delayed_present_duplicate_does_not_rewind", DelayedPresentDuplicateDoesNotRewind},
        {"duplicate_completion_does_not_restart", DuplicateCompletionDoesNotRestart},
        {"no_render_submit_activates_via_present", NoRenderSubmitActivatesViaPresent},
        {"reset_constants", ResetConstants},
        {"game_off_on", GameOffOn},
        {"pause_and_activation_block", PauseAndActivationBlock},
        {"feature_off_on_keeps_game_request", FeatureOffOnKeepsGameRequest},
        {"activation_failure", ActivationFailure},
        {"recheck_activation_conditions", RecheckActivationConditions},
        {"threshold_changes", ThresholdChanges},
        {"disabled_gate_bypass", DisabledGateBypass},
        {"gate_off_on_resets", GateOffOnResets},
        {"output_loss_restarts_readiness", OutputLossRestartsReadiness},
        {"present_only_frames", PresentOnlyFrames},
    };
    for (const auto& test : tests)
    {
        currentTest = test.name;
        test.run();
        std::printf("PASS %s\n", test.name);
    }
    std::printf("STREAMLINE_FG_ACTIVATION_GATE_OK tests=%zu checks=%u\n", sizeof(tests) / sizeof(tests[0]), checks);
    return EXIT_SUCCESS;
}
