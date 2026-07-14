#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"

namespace
{
    constexpr int knobSize = 100;
    constexpr int textBoxHeight = 20;
    constexpr int labelHeight = 20;
    constexpr int margin = 20;
    constexpr int toggleHeight = 24;

    constexpr int numRow1Knobs = 4; // Width, Bass Mono, Low Width, Output
    constexpr int numRow2Controls = 3; // Auto Mono Safety, Haas Mode, Haas Time

    constexpr int editorWidth = margin * 2 + numRow1Knobs * knobSize + (numRow1Knobs - 1) * margin;
    constexpr int row1Height = labelHeight + knobSize + textBoxHeight;
    constexpr int row2Height = knobSize; // tallest row-2 control (the Haas Time knob)
    constexpr int editorHeight = margin * 3 + row1Height + row2Height;
}

FirmamentAudioProcessorEditor::FirmamentAudioProcessorEditor (FirmamentAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit)
{
    configureKnob (widthKnob, ParamIDs::width, "Width");
    configureKnob (bassMonoKnob, ParamIDs::bassMonoFreq, "Bass Mono");
    configureKnob (lowWidthKnob, ParamIDs::lowWidth, "Low Width");
    configureKnob (outputKnob, ParamIDs::output, "Output");

    configureToggle (autoMonoSafetyToggle, ParamIDs::autoMonoSafety, "Auto Mono Safety");
    configureToggle (haasEnabledToggle, ParamIDs::haasEnabled, "Haas Mode");
    configureKnob (haasTimeKnob, ParamIDs::haasTimeMs, "Haas Time");

    setResizable (false, false);
    setSize (editorWidth, editorHeight);
}

FirmamentAudioProcessorEditor::~FirmamentAudioProcessorEditor() = default;

void FirmamentAudioProcessorEditor::configureKnob (Knob& knob, const juce::String& parameterId, const juce::String& labelText)
{
    knob.slider.setSliderStyle (juce::Slider::RotaryHorizontalVerticalDrag);
    knob.slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, knobSize, textBoxHeight);
    addAndMakeVisible (knob.slider);

    knob.label.setText (labelText, juce::dontSendNotification);
    knob.label.setJustificationType (juce::Justification::centred);
    // false => label sits above the slider it tracks; JUCE repositions it
    // automatically whenever the slider's bounds change, so resized() only
    // needs to place the sliders themselves.
    knob.label.attachToComponent (&knob.slider, false);
    addAndMakeVisible (knob.label);

    knob.attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, knob.slider);
}

void FirmamentAudioProcessorEditor::configureToggle (Toggle& toggle, const juce::String& parameterId, const juce::String& labelText)
{
    toggle.button.setButtonText (labelText);
    addAndMakeVisible (toggle.button);

    toggle.attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, parameterId, toggle.button);
}

void FirmamentAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (margin);

    auto row1 = bounds.removeFromTop (row1Height);
    row1.removeFromTop (labelHeight); // room for the attached labels above each knob

    const auto row1SlotWidth = row1.getWidth() / numRow1Knobs;

    for (auto* knob : { &widthKnob, &bassMonoKnob, &lowWidthKnob, &outputKnob })
        knob->slider.setBounds (row1.removeFromLeft (row1SlotWidth).reduced (margin / 2, 0));

    bounds.removeFromTop (margin);

    auto row2 = bounds.removeFromTop (row2Height);
    const auto row2SlotWidth = row2.getWidth() / numRow2Controls;

    auto autoMonoSlot = row2.removeFromLeft (row2SlotWidth).reduced (margin / 2, 0);
    autoMonoSafetyToggle.button.setBounds (autoMonoSlot.withSizeKeepingCentre (autoMonoSlot.getWidth(), toggleHeight));

    auto haasEnabledSlot = row2.removeFromLeft (row2SlotWidth).reduced (margin / 2, 0);
    haasEnabledToggle.button.setBounds (haasEnabledSlot.withSizeKeepingCentre (haasEnabledSlot.getWidth(), toggleHeight));

    auto haasTimeSlot = row2.removeFromLeft (row2SlotWidth).reduced (margin / 2, 0);
    haasTimeKnob.slider.setBounds (haasTimeSlot);
}
