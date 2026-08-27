#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include <array>

// Firmament's wave-3 COMPOSITIONAL faceplate geometry (campaign 2026-08,
// .scaffold/gui-assets/rollout-2026-07 + DECISIONS.md D4): the plate is
// the accepted EMPTY family plate render (resources/gui/
// plate_firmament.png), and every control is composited live from the
// extracted control-sprite library at the coordinates in
// resources/gui/layout_manifest.json.
//
// This header carries only what is NOT per-control position data (that
// lives in the manifest, the single source of truth - see
// gui/LayoutManifest.h): editor chrome, plate-to-@1x scaling, per-SPRITE-
// FAMILY intrinsic geometry, the D4 toggle-group dressing (engraved rules
// + captions), and the arc meter's needle-pivot provenance.
//
// SUPERSEDES the M3 vector editor's runtime-computed panel layout
// (BusPanel/PointerKnob/CorrelationMeter generation). Those components
// stay in the tree per the suite's "superseded, not deleted" convention,
// but this editor no longer uses them.
namespace fmt::layout
{
    constexpr int plateCanvasWidthPx = 1264;
    constexpr int plateCanvasHeightPx = 848;
    constexpr int plateWidth1x = 900;
    constexpr int plateHeight1x = 604;

    constexpr float plateToUnit = (float) plateWidth1x / (float) plateCanvasWidthPx;

    constexpr int topStripHeight1x = 36;
    constexpr int topStripGap1x = 4;
    constexpr int scaleButtonWidth1x = 64;

    constexpr int baseEditorWidth = plateWidth1x;
    constexpr int baseEditorHeight = topStripHeight1x + topStripGap1x + plateHeight1x;

    constexpr std::array<float, 3> scaleSteps { 1.0f, 1.5f, 2.0f };

    // ==================== sprite-family intrinsic geometry ====================
    // (measured once against the sprite PNGs - sprite-library extraction
    // wave 2026-08-27; see each entry's provenance note.)

    // sprite_knob_brass.png (148x148, master-05 row-1 far-right knob).
    constexpr float knobAnchorX = 75.5f;
    constexpr float knobAnchorY = 70.0f;
    constexpr float knobCapRadius = 34.0f;

    // sprite_selector_stepped.png (188x210, overture wave-1 redo crop -
    // brass cap + engraved tick crown, crown static).
    constexpr float selectorAnchorX = 85.0f;
    constexpr float selectorAnchorY = 117.0f;
    constexpr float selectorCapRadius = 28.0f;
    constexpr float selectorSweepDeg = 90.0f;

    // sprite_toggle_up.png (117x129, family lever toggle, up = ON).
    constexpr float toggleAnchorX = 58.0f;
    constexpr float toggleAnchorY = 68.0f;

    // sprite_arc_meter.png (827x367, firmament wave-1 redo crop
    // (230,85,1025,420) + 16 px pad - D4 arc dial, -1/0/+1 face, hub +
    // anchor bar, NO needle): the manifest positions the CANVAS CENTRE
    // (413.5, 183.5). The baked hub (needle pivot) was measured at sprite
    // px (418, 267) - fractions below; the face's scale end hooks sit at
    // +-76 deg from the hub (gui/ArcNeedleMeter.h's arcEndAngleDeg).
    constexpr float arcAnchorX = 413.5f;
    constexpr float arcAnchorY = 183.5f;
    constexpr float arcSpriteWidthPx = 827.0f;
    constexpr float arcSpriteHeightPx = 367.0f;
    constexpr float arcPivotXFraction = 418.0f / arcSpriteWidthPx;
    constexpr float arcPivotYFraction = 267.0f / arcSpriteHeightPx;

    constexpr float knobSweepDeg = 270.0f;

    // ==================== engraved lettering ====================
    constexpr float labelBoxWidthPlatePx = 150.0f;
    constexpr float labelBoxHeightPlatePx = 26.0f;

    // D4 toggle-group dressing, drawn live by the editor (the empty plate
    // bakes neither): a thin engraved rule ABOVE each group of three and
    // an engraved caption BELOW it. Plate-px geometry, mirrored pairs.
    struct ToggleGroupDressing
    {
        float ruleX0, ruleX1, ruleY;
        float captionCx, captionCy;
        const char* caption;
    };

    constexpr std::array<ToggleGroupDressing, 2> toggleGroupDressings {
        ToggleGroupDressing { 150.0f, 430.0f, 392.0f, 290.0f, 478.0f, "SAFETY" },
        ToggleGroupDressing { 834.0f, 1114.0f, 392.0f, 974.0f, 478.0f, "WIDEN" },
    };
}
