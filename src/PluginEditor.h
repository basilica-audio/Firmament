#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include "presets/PresetBar.h"

class FirmamentAudioProcessor;

// A simple, functional v0.1 editor: one rotary slider per float parameter and
// one toggle button per bool parameter, bound to the APVTS via
// SliderAttachment/ButtonAttachment. A custom vector-drawn GUI and metering
// (the correlation/phase value already computed by FirmamentEngine, see
// FirmamentAudioProcessor::getCorrelationMeterValue()) are M3 scope; this
// editor is deliberately plain but fully wired and usable for every
// parameter added so far.
class FirmamentAudioProcessorEditor final : public juce::AudioProcessorEditor
{
public:
    explicit FirmamentAudioProcessorEditor (FirmamentAudioProcessor& processorToEdit);
    ~FirmamentAudioProcessorEditor() override;

    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    // One knob + label per float parameter, in signal-flow order.
    struct Knob
    {
        juce::Slider slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    // One toggle button per bool parameter.
    struct Toggle
    {
        juce::ToggleButton button;
        std::unique_ptr<ButtonAttachment> attachment;
    };

    void configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText);
    void configureToggle (Toggle& toggle, const juce::String& parameterId, const juce::String& labelText);

    FirmamentAudioProcessor& audioProcessor;

    // M2 preset system (src/presets/PresetBar.h) - a horizontal strip
    // docked at the top of the editor. Constructed after the localisation
    // frame is installed (see the constructor) so its TRANS()'d strings
    // (and any of its own dialogs opened later) pick up the right language
    // from the very first paint.
    basilica::presets::PresetBar presetBar;

    // Row 1: the core M/S width signal path.
    Knob widthKnob;
    Knob bassMonoKnob;
    Knob lowWidthKnob;
    Knob outputKnob;

    // Row 2: M1 safety/alternative-widening additions.
    Toggle autoMonoSafetyToggle;
    Toggle haasEnabledToggle;
    Knob haasTimeKnob;

    // Row 3: v0.2.0 additions (docs/design-brief.md) - Auto Mono Safety's
    // floor/multiband refinements and the new Decorrelate widening
    // technique.
    Knob autoMonoSafetyFloorKnob;
    Toggle autoMonoSafetyMultibandToggle;
    Toggle decorrelateEnabledToggle;
    Knob decorrelateAmountKnob;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentAudioProcessorEditor)
};
