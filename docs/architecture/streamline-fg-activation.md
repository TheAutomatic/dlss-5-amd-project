# Optional Streamline frame-generation activation delay

`[DLSSG] RequiredReadyFrames` accepts 0–300. The default (`auto`) is **0**
for every game. Zero keeps the existing activation path; a positive value
opts Streamline / DLSSG input into the readiness gate, regardless of the FG
output. Other input providers and externally owned FG are unchanged.

The setting is also available as **Frame Gen > Advanced FG Settings > Ready
Frames** when the active input is DLSSG. Save Settings persists the INI key.
Changing the threshold affects the next activation and does not interrupt an
already active FG output.

For example, to wait for 30 consecutive frames with ready inputs:

```ini
[DLSSG]
RequiredReadyFrames=30
```

The gate requires an FG request through Streamline 2 options; Streamline 1 uses
`notRenderingGameFrames=false`. After a Present boundary, a completed input
frame counts when it has nonzero depth and motion-vector dimensions, camera
constants, a nonzero interpolation size, and
no reset flag. Constants and interpolation dimensions can be reused across
frames, as Streamline permits. Evaluate and Present callbacks for the same
frame count only once. Missing resources, a frame-ID gap, a reset, or a
pause/output/swapchain transition restart the consecutive-frame count.

Disabling the game's FG request clears the sequence and deactivates the output.
Failed activation must collect a new sequence before retrying. This is a CPU
input-readiness check; it does not add GPU synchronization or guarantee that a
driver hang is fixed.

The focused CPU checks run with `tests\host\run.cmd streamline`. Game validation
should additionally cover FG off/on, loading/UI transitions, resize, both
Streamline versions, and the intended FG output. CPU tests do not certify
those GPU paths.
