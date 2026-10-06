# Menu localization

`[Menu] Language=en` is the default. The Ins menu offers English and 简体中文;
changes apply on the next frame and **Save Settings** persists the selection.
The bottom action row places the language label and a content-sized selector
immediately before Save Settings, with wrapping only when the viewport cannot fit the row.
The canonical Chinese code is `zh-CN`; `zh` and case-insensitive `zh-cn` are accepted
on ini load. Unrecognized values use English. Language is a host UI key in ConfigKeys,
not a runtime environment option, model parameter or translated ini key.

`menu/MenuStrings.inl` contains UTF-8 English-to-Chinese entries. `MenuLocalization.h`
provides immutable lookups and an explicit thread-local frame snapshot, with English
fallback for unknown strings (including driver/runtime diagnostic details). `MenuUi.h`
adapts only display calls. Format specifiers keep their original types/order; menu
values, backend names and config identifiers are not translated on write.

Both languages use a stable `###Menu/original` suffix (existing `###` identities are
retained). The `###` bytes participate in ImGui hashing, so translated IDs must never
be compared with raw English IDs. Deferred sliders use `MenuUi::GetID`, modal popup
openers use `MenuUi::OpenPopup`, and window lookups use the same canonical label.
Control IDs and expansion state survive language switches and label wrapping.
Combo item indices remain unchanged. Custom pipeline text is translated before
both text measurement and drawing. Do not localize a cached/static label at initialization;
look it up at draw time so both switching directions work.

## Font

The host embeds `menu/font/NotoSansSC-Menu.ttf` as resource 201. It is a regular-weight
subset, merged with the current base font at context creation even with UseHQFont off.
Both languages are available without rebuilding the atlas during rendering and without
installing Windows language packs. The underlying DLL owns the resource bytes; ImGui
must not free them (`FontDataOwnedByAtlas=false`).
`MenuFont::MergeChinese` derives the merge size from the actual base font, adds
8% optical size compensation for Noto's glyph face, and shares the base font's
vertical offset. This keeps mixed Latin/CJK labels visually aligned with both
Hack and the non-HQ fallback, without changing the menu's scale or saved font choice.

Source: [google/fonts](https://github.com/google/fonts/tree/6e8069ff8ba3dab2a397fb30e7fbd243aba9b57a/ofl/notosanssc),
`NotoSansSC[wght].ttf`, SHA256
`A3041811A78C361B1DE50F953C805E0244951C21C5BD412F7232EF0D899AF0DA`.
SIL Open Font License is shipped in `Licenses/NotoSansSC_OFL.txt`. The modified family
is named OptScaler Menu Sans; copyright/license metadata is retained.

Rebuild only when catalog glyph coverage changes:
`python tools/build/build-menu-font.py --source <original-ttf>` (fonttools required).
Normal builds/CI use the committed subset and do not download fonts. FontTools source
used for the initial subset: `4cc164be6ec3046ebabf4cc635961e4d03f42da5`.

Changing text requires checking Chinese terminology, printf placeholders, glyph coverage,
control ID stability, and narrow/scaled layout. Preserve technical names such as NR, SR,
ViT, DX12 and API enum names when a translation would obscure their meaning.

## Layout validation

Long value labels move above their slider/input; buttons, checkboxes, radio controls
and tree headings wrap within the available column. SameLine rows break before they
overflow. The existing responsive one/two-column structure and NR Input/Model/Output
navigation remain. No additional settings category is introduced.

`tests/host/menu-localization.cmd` renders real ImGui components with D3D11 WARP at
0.5–4x scale and 320–1920 pixel widths in both languages and both bundled base fonts
(96 combinations). It checks glyph coverage, mixed-script height/alignment,
language/layout-independent IDs, the compact language/save row, disabled controls,
modal identity and horizontal bounds.
Header background and selectable/tree hit-area padding are accounted for explicitly.
It is a component/layout regression, not a substitute for in-game DPI and input testing.
