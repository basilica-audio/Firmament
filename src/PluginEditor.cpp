#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

namespace
{
    constexpr int knobSize = 100;
    constexpr int textBoxHeight = 20;
    constexpr int labelHeight = 20;
    constexpr int margin = 20;
    constexpr int toggleHeight = 24;
    constexpr int presetBarHeight = 28;

    constexpr int selectorHeight = 24;

    constexpr int numRow1Knobs = 4; // Width, Bass Mono, Low Width, Output
    constexpr int numRow2Controls = 3; // Auto Mono Safety, Haas Mode, Haas Time
    constexpr int numRow3Controls = 4; // AutoMono Safety Floor, Multiband, Decorrelate, Decorrelate Amount
    constexpr int numRow4Controls = 3; // v0.3.0: Decorrelate Mode, Bass Mono Mode, Safety Response
    constexpr int numRow5Controls = 4; // v0.3.0: High Split, High Width, Width Comp, Mono Audition

    constexpr int editorWidth = margin * 2 + numRow1Knobs * knobSize + (numRow1Knobs - 1) * margin;
    constexpr int row1Height = labelHeight + knobSize + textBoxHeight;
    constexpr int row2Height = knobSize; // tallest row-2 control (the Haas Time knob)
    constexpr int row3Height = knobSize; // tallest row-3 control (the two knobs)
    constexpr int row4Height = labelHeight + selectorHeight; // v0.3.0 mode selectors
    constexpr int row5Height = labelHeight + knobSize + textBoxHeight; // v0.3.0 knobs/toggles
    constexpr int editorHeight = margin * 7 + presetBarHeight + row1Height + row2Height + row3Height + row4Height + row5Height;

    // M2 i18n frame (.scaffold/specs/preset-system-m2.md): selects German
    // (resources/i18n/de.txt) or falls through to English, once, at editor
    // construction - see Localisation.h's docs. `presetBar` is a member
    // initialised via the constructor's initialiser list, and its own
    // constructor already calls TRANS() on every button label - member
    // initialisers run in declaration order regardless of the order they're
    // written in, so this helper (called from presetBar's own initialiser
    // expression below) is what actually guarantees installLocalisation()
    // runs before presetBar exists, not a installLocalisation() call in the
    // constructor *body*, which would run too late.
    basilica::presets::PresetManager& initLocalisationThenGetPresetManager (FirmamentAudioProcessor& processor)
    {
        basilica::presets::installLocalisation (BinaryData::de_txt, BinaryData::de_txtSize);
        return processor.presetManager;
    }
}

FirmamentAudioProcessorEditor::FirmamentAudioProcessorEditor (FirmamentAudioProcessor& processorToEdit)
    : juce::AudioProcessorEditor (&processorToEdit),
      audioProcessor (processorToEdit),
      presetBar (initLocalisationThenGetPresetManager (processorToEdit))
{
    addAndMakeVisible (presetBar);

    configureKnob (widthKnob, ParamIDs::width, "Width");
    configureKnob (bassMonoKnob, ParamIDs::bassMonoFreq, "Bass Mono");
    configureKnob (lowWidthKnob, ParamIDs::lowWidth, "Low Width");
    configureKnob (outputKnob, ParamIDs::output, "Output");

    configureToggle (autoMonoSafetyToggle, ParamIDs::autoMonoSafety, "Auto Mono Safety");
    configureToggle (haasEnabledToggle, ParamIDs::haasEnabled, "Haas Mode");
    configureKnob (haasTimeKnob, ParamIDs::haasTimeMs, "Haas Time");

    configureKnob (autoMonoSafetyFloorKnob, ParamIDs::autoMonoSafetyFloorDb, "Safety Floor");
    configureToggle (autoMonoSafetyMultibandToggle, ParamIDs::autoMonoSafetyMultiband, "Safety Multiband");
    configureToggle (decorrelateEnabledToggle, ParamIDs::decorrelateEnabled, "Decorrelate");
    configureKnob (decorrelateAmountKnob, ParamIDs::decorrelateAmount, "Decorrelate Amount");

    // v0.3.0 additions - minimal generic controls only (M3 owns the real
    // GUI).
    configureSelector (decorrelateModeSelector, ParamIDs::decorrelateMode, "Decorrelate Mode");
    configureSelector (bassMonoModeSelector, ParamIDs::bassMonoMode, "Bass Mono Mode");
    configureSelector (safetyModeSelector, ParamIDs::safetyMode, "Safety Response");
    configureKnob (highSplitKnob, ParamIDs::highSplitFreq, "High Split");
    configureKnob (highWidthKnob, ParamIDs::highWidth, "High Width");
    configureToggle (widthCompToggle, ParamIDs::widthComp, "Width Comp");
    configureToggle (monoAuditionToggle, ParamIDs::monoAudition, "Mono Audition");

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

void FirmamentAudioProcessorEditor::configureSelector (Selector& selector, const juce::String& parameterId, const juce::String& labelText)
{
    auto* parameter = audioProcessor.apvts.getParameter (parameterId);
    jassert (parameter != nullptr);

    // JUCE 8.0.14 gotcha (suite-wide finding): ComboBoxAttachment does NOT
    // populate the box's items - they must be added from the parameter's
    // own value strings BEFORE attaching, or the box stays empty and the
    // attachment's initial sync selects nothing.
    if (parameter != nullptr)
        selector.box.addItemList (parameter->getAllValueStrings(), 1);

    addAndMakeVisible (selector.box);

    selector.label.setText (labelText, juce::dontSendNotification);
    selector.label.setJustificationType (juce::Justification::centred);
    selector.label.attachToComponent (&selector.box, false);
    addAndMakeVisible (selector.label);

    selector.attachment = std::make_unique<ComboBoxAttachment> (audioProcessor.apvts, parameterId, selector.box);
}

void FirmamentAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (margin);

    presetBar.setBounds (bounds.removeFromTop (presetBarHeight));
    bounds.removeFromTop (margin);

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

    bounds.removeFromTop (margin);

    // Row 3 (v0.2.0): Auto Mono Safety Floor knob, Auto Mono Safety
    // Multiband toggle, Decorrelate toggle, Decorrelate Amount knob.
    auto row3 = bounds.removeFromTop (row3Height);
    const auto row3SlotWidth = row3.getWidth() / numRow3Controls;

    auto floorSlot = row3.removeFromLeft (row3SlotWidth).reduced (margin / 2, 0);
    autoMonoSafetyFloorKnob.slider.setBounds (floorSlot);

    auto multibandSlot = row3.removeFromLeft (row3SlotWidth).reduced (margin / 2, 0);
    autoMonoSafetyMultibandToggle.button.setBounds (multibandSlot.withSizeKeepingCentre (multibandSlot.getWidth(), toggleHeight));

    auto decorrelateSlot = row3.removeFromLeft (row3SlotWidth).reduced (margin / 2, 0);
    decorrelateEnabledToggle.button.setBounds (decorrelateSlot.withSizeKeepingCentre (decorrelateSlot.getWidth(), toggleHeight));

    auto decorrelateAmountSlot = row3.removeFromLeft (row3SlotWidth).reduced (margin / 2, 0);
    decorrelateAmountKnob.slider.setBounds (decorrelateAmountSlot);

    bounds.removeFromTop (margin);

    // Row 4 (v0.3.0): the three mode selectors.
    auto row4 = bounds.removeFromTop (row4Height);
    row4.removeFromTop (labelHeight); // room for the attached labels
    const auto row4SlotWidth = row4.getWidth() / numRow4Controls;

    for (auto* selector : { &decorrelateModeSelector, &bassMonoModeSelector, &safetyModeSelector })
        selector->box.setBounds (row4.removeFromLeft (row4SlotWidth).reduced (margin / 2, 0));

    bounds.removeFromTop (margin);

    // Row 5 (v0.3.0): High Split/High Width knobs, Width Comp/Mono Audition
    // toggles.
    auto row5 = bounds.removeFromTop (row5Height);
    row5.removeFromTop (labelHeight);
    const auto row5SlotWidth = row5.getWidth() / numRow5Controls;

    highSplitKnob.slider.setBounds (row5.removeFromLeft (row5SlotWidth).reduced (margin / 2, 0));
    highWidthKnob.slider.setBounds (row5.removeFromLeft (row5SlotWidth).reduced (margin / 2, 0));

    auto widthCompSlot = row5.removeFromLeft (row5SlotWidth).reduced (margin / 2, 0);
    widthCompToggle.button.setBounds (widthCompSlot.withSizeKeepingCentre (widthCompSlot.getWidth(), toggleHeight));

    auto monoAuditionSlot = row5.removeFromLeft (row5SlotWidth).reduced (margin / 2, 0);
    monoAuditionToggle.button.setBounds (monoAuditionSlot.withSizeKeepingCentre (monoAuditionSlot.getWidth(), toggleHeight));
}
