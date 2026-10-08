# Menu navigation and background rendering

The Insert menu uses eight stable page IDs: Neural Rendering, Upscaling, Frame
Generation, Latency & FPS, Textures, Compatibility, Menu, and Help / Status.
The NR page retains the existing input/model/output navigation, backend controls,
history/reuse exclusion and reset groups. Language and Save Settings remain in
the footer. Labels do not become INI identifiers; MenuLocale provides translations.

The sidebar measures translated labels. Narrow windows or high UI scale use a
compact page selector instead. Page content wraps to its viewport and scrolls
vertically; it must not preserve an old horizontal content width after resizing.
Cards reuse the first section heading instead of introducing another foldout.
Custom, bundled CJK and fallback font paths retain the same mixed-script sizing.

Layout and blur rendering are selectively adapted from OptiScaler upstream
`434b9555d960e6e471a461c2b0d98b8e1fdacfa2`; existing NR logic and overlay entry
points remain local. `[Menu] BackgroundBlur` and `BackgroundBlurStrength` belong
to ConfigKeys/Config, with defaults true/1.0 and strength clamped to 0..4. They
are persisted and reset with other appearance controls. Only the visible menu
requests blur. Unsupported backbuffer layouts/usage retain the ordinary background.

DX11 copies the current backbuffer and restores compute bindings. DX12 uses a
menu fence, per-backbuffer source descriptors and allocator completion checks;
an unfinished frame skips the overlay. Queue changes and cleanup drain submitted
work; failed completion retains resources and disables their reuse. Blur textures
are released before the ImGui descriptor heap. Vulkan uses the existing overlay
queue and command buffers, requires transfer-source usage and compute capability,
and waits for prior work before resize/destruction. The private Mochizuki Vulkan
scope is unchanged. No temporary rendering device or global spoofing is added.

`tests/host/menu-localization.cmd` renders real ImGui components with WARP across
fonts/languages/scales, including every navigation page and narrow/wide resize.
This covers component layout, not full-game rendering. Open/close, HDR,
Alt+Tab/resize and device shutdown on DX11/DX12/Vulkan remain game acceptance items.
