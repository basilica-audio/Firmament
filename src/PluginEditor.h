#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "gui/ArcNeedleMeter.h"
#include "gui/LayoutManifest.h"
#include "gui/MasterCropKnob.h"
#include "gui/PlateTypography.h"
#include "gui/SpriteToggle.h"
#include "presets/PresetBar.h"

class FirmamentAudioProcessor;

// Wave-3 COMPOSITIONAL photoreal editor (campaign 2026-08, supersedes the
// M3 vector editor - BusPanel/PointerKnob/CorrelationMeter stay in the
// tree per the suite's "superseded, not deleted" convention but are no
// longer used): the accepted EMPTY family plate render
// (resources/gui/plate_firmament.png) is the sole baked background, and
// every control is composited live from the extracted control-sprite
// library at the coordinates in resources/gui/layout_manifest.json (the
// single source of truth - see gui/LayoutManifest.h). Draw order:
//
//   1. plate render (paint())
//   2. static control sprites - knob/selector bodies and the needle-free
//      D4 arc-dial face at their manifest positions
//   3. D4 toggle-group dressing - two thin engraved rules + captions
//      (fmt::layout::toggleGroupDressings; the empty plate bakes neither)
//   4. engraved lettering - PlateTypography, gilded gold on dark basalt
//   5. rotating cap crops - one MasterCropKnob child per knob/selector
//   6. lever toggles - SpriteToggle children (up = ON, mirrored = OFF)
//   7. needle overlay - the ArcNeedleMeter child (glow + master-extracted
//      needle rotated live from the processor's output-correlation
//      atomic; NADEL-REGEL compliant)
//
// Firmament-specific control set (rollout-2026-07/firmament/
// control-inventory.md + DECISIONS.md D4): 9 knobs + 3 stepped selectors
// in a 6-column 2-row grid, 6 family toggles in two groups of three
// flanking the meter zone, and the D4 correlation arc instrument reading
// the REAL output-correlation estimate (no dead decoration).
//
// Window scaling is STEPPED (100/150/200%, UA-style corner control,
// persisted as a plain property on the APVTS state tree), matching every
// merged M3 editor in the suite.
class FirmamentAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit FirmamentAudioProcessorEditor (FirmamentAudioProcessor& processorToEdit);
    ~FirmamentAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

    // The parsed layout manifest - exposed read-only so tests assert
    // layout invariants against the exact data this editor composites
    // from (tests/gui/EditorLayoutTests.cpp).
    const basilica::gui::LayoutManifest& layoutManifest() const noexcept { return manifest; }

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        const basilica::gui::ManifestControl* entry = nullptr;
        std::unique_ptr<basilica::gui::MasterCropKnob> slider;
        std::unique_ptr<SliderAttachment> attachment;
    };

    struct Toggle
    {
        const basilica::gui::ManifestControl* entry = nullptr;
        std::unique_ptr<basilica::gui::SpriteToggle> button;
        std::unique_ptr<ButtonAttachment> attachment;
    };

    struct Meter
    {
        const basilica::gui::ManifestControl* entry = nullptr;
        std::unique_ptr<basilica::gui::ArcNeedleMeter> component;
    };

    juce::Image spriteImageFor (const juce::String& spriteKey) const;
    void buildControlsFromManifest();
    void applyScaleStep (int newStepIndex);
    void cycleScale();
    void drawStaticSprites (juce::Graphics& g) const;
    void drawToggleGroupDressing (juce::Graphics& g) const;
    void drawPlateLettering (juce::Graphics& g) const;
    void timerCallback() override;

    float plateScale() const noexcept;
    juce::Point<float> plateOrigin() const noexcept;

    FirmamentAudioProcessor& audioProcessor;

    basilica::gui::LayoutManifest manifest;

    juce::Image plateImage;
    juce::Image knobSprite, selectorSprite, toggleSprite, arcMeterSprite, needleSprite;

    basilica::presets::PresetBar presetBar;
    juce::TextButton scaleButton;
    int scaleStepIndex = 0; // 0 = 100%, 1 = 150%, 2 = 200%

    std::vector<Knob> knobs;
    std::vector<Toggle> toggles;
    std::vector<Meter> meters;

    basilica::gui::PlateTypography typography;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentAudioProcessorEditor)
};
