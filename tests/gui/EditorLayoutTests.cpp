#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"

#include <BinaryData.h>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cmath>
#include <map>
#include <vector>

// Wave-3 compositional-layout invariants, asserted against the SAME parsed
// manifest the editor composites from (PluginEditor::layoutManifest() /
// gui/LayoutManifest.h) - never a second hand-maintained coordinate list.
// The expected control census comes from the rollout control inventory
// (.scaffold/gui-assets/rollout-2026-07/firmament/control-inventory.md +
// DECISIONS.md D4): 9 knobs + 3 selectors in a 6-column 2-row grid,
// 6 toggles in two mirrored groups of three, 1 correlation arc meter.
namespace
{
    basilica::gui::LayoutManifest parseManifest()
    {
        return basilica::gui::LayoutManifest::parse (BinaryData::layout_manifest_json,
                                                     BinaryData::layout_manifest_jsonSize);
    }

    // The plate's usable control field (inside the gold pinstripe border) -
    // family plate geometry, slightly conservative on purpose.
    constexpr float fieldLeft = 85.0f, fieldRight = 1162.0f;
    constexpr float fieldTop = 85.0f, fieldBottom = 768.0f;

    // Baked central divider flourish (family plate: y ~448..459,
    // x ~510..740) - no control cap may cover it.
    const juce::Rectangle<float> dividerKeepOut (500.0f, 444.0f, 250.0f, 20.0f);

    // Baked amber vent grilles (family plate, measured).
    const juce::Rectangle<float> ventLeftKeepOut (150.0f, 488.0f, 142.0f, 195.0f);
    const juce::Rectangle<float> ventRightKeepOut (963.0f, 488.0f, 142.0f, 195.0f);

    float capRadiusPlatePx (const basilica::gui::ManifestControl& control)
    {
        using namespace fmt::layout;

        if (control.kind == "selector")
            return selectorCapRadius * control.scale;

        if (control.kind == "toggle")
            return 40.0f * control.scale; // between housing half-width (~33) and half-height (~48): the D4 triplets pack at an 85 px pitch, so the circle proxy must not overstate the housing's real width

        return knobCapRadius * control.scale;
    }

    // The arc meter is a wide rectangle, not a disc - its hardware extent
    // for keep-out purposes (the sprite canvas minus the blended margin).
    juce::Rectangle<float> arcHardwareBox (const basilica::gui::ManifestControl& meter)
    {
        using namespace fmt::layout;
        const auto w = (arcSpriteWidthPx - 40.0f) * meter.scale;
        const auto h = (arcSpriteHeightPx - 30.0f) * meter.scale;
        return { meter.cx - w * 0.5f, meter.cy - h * 0.5f, w, h };
    }
}

TEST_CASE ("Manifest parses and matches the rollout control inventory census", "[gui][layout]")
{
    const auto manifest = parseManifest();

    REQUIRE (manifest.isValid());
    CHECK (manifest.plateWidthPx == fmt::layout::plateCanvasWidthPx);
    CHECK (manifest.plateHeightPx == fmt::layout::plateCanvasHeightPx);

    CHECK (manifest.ofKind ("knob").size() == 9);
    CHECK (manifest.ofKind ("selector").size() == 3);
    CHECK (manifest.ofKind ("toggle").size() == 6);
    CHECK (manifest.ofKind ("meter").size() == 1); // the D4 correlation arc instrument
    CHECK (manifest.controls.size() == 19);
}

TEST_CASE ("Every non-meter manifest control id resolves to a real APVTS parameter of the right type", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    FirmamentAudioProcessor processor;

    for (const auto& control : manifest.controls)
    {
        if (control.kind == "meter")
            continue; // meter ids are editor-defined display elements, not parameters

        auto* parameter = processor.apvts.getParameter (control.id);
        INFO ("manifest id \"" << control.id.toStdString() << "\"");
        REQUIRE (parameter != nullptr);

        if (control.kind == "toggle")
            CHECK (dynamic_cast<juce::AudioParameterBool*> (parameter) != nullptr);
        else if (control.kind == "selector")
            CHECK (dynamic_cast<juce::AudioParameterChoice*> (parameter) != nullptr);
        else if (control.kind == "knob")
            CHECK (dynamic_cast<juce::AudioParameterFloat*> (parameter) != nullptr);
    }
}

TEST_CASE ("Knobs and selectors share a uniform 6-column, 2-row grid", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    std::map<float, std::vector<float>> rows; // cy -> sorted cx list

    for (const auto& control : manifest.controls)
        if (control.kind == "knob" || control.kind == "selector")
            rows[control.cy].push_back (control.cx);

    REQUIRE (rows.size() == 2);

    std::vector<float> columns;

    for (auto& [cy, xs] : rows)
    {
        juce::ignoreUnused (cy);
        std::sort (xs.begin(), xs.end());
        CHECK (xs.size() == 6);

        // Uniform column rhythm (the LAYOUT-INVARIANTE).
        for (size_t i = 2; i < xs.size(); ++i)
            CHECK (std::abs ((xs[i] - xs[i - 1]) - (xs[1] - xs[0])) < 1.0f);

        if (columns.empty())
            columns = xs;
        else
            for (size_t i = 0; i < xs.size(); ++i)
                CHECK (std::abs (columns[i] - xs[i]) < 1.0f); // rows share columns
    }
}

TEST_CASE ("Toggle groups are two mirrored triplets flanking the meter zone", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    const auto toggles = manifest.ofKind ("toggle");
    REQUIRE (toggles.size() == 6);

    const auto centre = (float) fmt::layout::plateCanvasWidthPx * 0.5f;
    std::vector<float> leftXs, rightXs;

    for (const auto* toggle : toggles)
    {
        CHECK (toggle->cy == toggles.front()->cy); // one shared row
        (toggle->cx < centre ? leftXs : rightXs).push_back (toggle->cx);
    }

    REQUIRE (leftXs.size() == 3);
    REQUIRE (rightXs.size() == 3);

    std::sort (leftXs.begin(), leftXs.end());
    std::sort (rightXs.begin(), rightXs.end());

    // Mirror symmetry about the plate centre line.
    for (size_t i = 0; i < 3; ++i)
        CHECK (std::abs ((centre - leftXs[i]) - (rightXs[2 - i] - centre)) < 1.0f);
}

TEST_CASE ("Every control stays inside the pinstripe field and off the baked plate art", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    for (const auto& control : manifest.controls)
    {
        INFO ("control \"" << control.id.toStdString() << "\"");

        if (control.kind == "meter")
        {
            const auto box = arcHardwareBox (control);
            CHECK (box.getX() >= fieldLeft);
            CHECK (box.getRight() <= fieldRight);
            CHECK (box.getY() >= fieldTop);
            CHECK (box.getBottom() <= fieldBottom);
            CHECK_FALSE (box.intersects (ventLeftKeepOut));
            CHECK_FALSE (box.intersects (ventRightKeepOut));
            CHECK_FALSE (box.intersects (dividerKeepOut));
            continue;
        }

        const auto r = capRadiusPlatePx (control);

        CHECK (control.cx - r >= fieldLeft);
        CHECK (control.cx + r <= fieldRight);
        CHECK (control.cy - r >= fieldTop);
        CHECK (control.cy + r <= fieldBottom);

        const juce::Rectangle<float> capBox (control.cx - r, control.cy - r, 2.0f * r, 2.0f * r);
        CHECK_FALSE (capBox.intersects (dividerKeepOut));
        CHECK_FALSE (capBox.intersects (ventLeftKeepOut));
        CHECK_FALSE (capBox.intersects (ventRightKeepOut));

        if (control.labelCy > 0.0f)
        {
            using namespace fmt::layout;
            const juce::Rectangle<float> labelBox (control.cx - labelBoxWidthPlatePx * 0.5f,
                                                   control.labelCy - labelBoxHeightPlatePx * 0.5f,
                                                   labelBoxWidthPlatePx, labelBoxHeightPlatePx);

            CHECK (labelBox.getY() >= fieldTop);
            CHECK (labelBox.getBottom() <= fieldBottom);

            // Lettering never intrudes into its own control's rotating cap.
            CHECK (labelBox.getY() >= control.cy + r - 1.0f);
        }
    }
}

TEST_CASE ("No two composited elements overlap", "[gui][layout]")
{
    const auto manifest = parseManifest();
    REQUIRE (manifest.isValid());

    for (size_t a = 0; a < manifest.controls.size(); ++a)
    {
        for (size_t b = a + 1; b < manifest.controls.size(); ++b)
        {
            const auto& ca = manifest.controls[a];
            const auto& cb = manifest.controls[b];

            INFO (ca.id.toStdString() << " vs " << cb.id.toStdString());

            if (ca.kind == "meter" || cb.kind == "meter")
            {
                const auto& meter = ca.kind == "meter" ? ca : cb;
                const auto& other = ca.kind == "meter" ? cb : ca;
                const auto r = capRadiusPlatePx (other);
                const juce::Rectangle<float> otherBox (other.cx - r, other.cy - r, 2.0f * r, 2.0f * r);
                CHECK_FALSE (arcHardwareBox (meter).intersects (otherBox));
                continue;
            }

            const auto minGap = capRadiusPlatePx (ca) + capRadiusPlatePx (cb);
            const auto dx = ca.cx - cb.cx;
            const auto dy = ca.cy - cb.cy;

            CHECK (dx * dx + dy * dy >= minGap * minGap);
        }
    }
}

TEST_CASE ("Editor base size derives from the plate geometry", "[gui][layout]")
{
    using namespace fmt::layout;

    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    CHECK (editor.getWidth() == baseEditorWidth);
    CHECK (editor.getHeight() == baseEditorHeight);
    CHECK (editor.layoutManifest().isValid());
}
