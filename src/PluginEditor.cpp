#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "presets/Localisation.h"

#include <BinaryData.h>

#include <algorithm>

namespace
{
    // ----- M3 vector-editor layout metrics (issue #4) ---------------------
    // All values are design constants, not measurements of pre-rendered
    // art (there is none): the editor computes its own size from these plus
    // the control tables in the constructor, and tests/gui/EditorLayoutTests.cpp
    // asserts the resulting geometry (containment, no overlap) on the real
    // component tree, so a change here can never silently clip a control.
    constexpr int outerMargin = 10;
    constexpr int presetBarHeight = 30;
    constexpr int bandGap = 8;

    constexpr int panelPadding = 10;
    constexpr int panelBottomPadding = 8;

    // A knob slot: attached label above (JUCE 8.0.14 Label::
    // componentMovedOrResized sizes an above-attached label to
    // borderTopAndBottom + 6 + fontHeight ~ 22 px for the 14 px suite
    // serif, so 24 reserved keeps it clear of the panel header), then the
    // rotary area, then the value box baked into the slider's own bounds.
    constexpr int labelHeight = 24;
    constexpr int knobSize = 60;
    constexpr int textBoxHeight = 16;
    constexpr int knobSlotWidth = 80;
    constexpr int toggleSlotWidth = 70;
    constexpr int toggleHeight = 32;
    constexpr int slotGap = 6; // trimmed off the right of every slot
    constexpr int rowHeight = labelHeight + knobSize + textBoxHeight;

    // Right-hand meter bay on the two correlation-metered panels.
    constexpr int meterBayWidth = 150;
    constexpr int meterWidth = 134;
    constexpr int meterHeight = 96;

    // M2 i18n frame: selects German (resources/i18n/de.txt) or falls
    // through to English, once, at editor construction - see
    // Localisation.h's docs. `presetBar` is a member initialised via the
    // constructor's initialiser list, and its own constructor already calls
    // TRANS() on every button label - member initialisers run in
    // declaration order, so this helper (called from presetBar's own
    // initialiser expression below) is what guarantees installLocalisation()
    // runs before presetBar exists, not a call in the constructor *body*,
    // which would run too late.
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
    // Propagates to every child, including the preset bar's stock buttons
    // and any menus/dialogs they open.
    setLookAndFeel (&lookAndFeel);

    // FOCUS ORDER (WCAG 2.4.3): children are created and added in signal-
    // flow/reading order - preset bar, then Width, Bands, Mono Safety,
    // Widen, Output, left-to-right within each row. JUCE's default
    // traverser follows this creation order; do not reorder.
    addAndMakeVisible (presetBar);

    // --- Width: the core M/S image scale + its makeup switch --------------
    auto& width = addPanel ("Width");
    widthPanel = &width;
    addKnob (width, ParamIDs::width, "Width");
    addToggle (width, ParamIDs::widthComp, "Width Comp");

    addMeter (width, "Input correlation meter", "IN");

    // --- Bands: the two Side-path crossovers + per-band widths ------------
    auto& bands = addPanel ("Bands");
    bandsPanel = &bands;
    addKnob (bands, ParamIDs::bassMonoFreq, "Bass Mono");
    addKnob (bands, ParamIDs::bassMonoMode, "Bass Mode");
    addKnob (bands, ParamIDs::lowWidth, "Low Width");
    addKnob (bands, ParamIDs::highSplitFreq, "High Split");
    addKnob (bands, ParamIDs::highWidth, "High Width");

    // --- Mono Safety: correlation-driven Side attenuation -----------------
    auto& safety = addPanel ("Mono Safety");
    safetyPanel = &safety;
    addToggle (safety, ParamIDs::autoMonoSafety, "Safety");
    addKnob (safety, ParamIDs::safetyMode, "Response");
    addKnob (safety, ParamIDs::autoMonoSafetyFloorDb, "Floor");
    addToggle (safety, ParamIDs::autoMonoSafetyMultiband, "Multiband");

    // --- Widen: post-decode decorrelation / Haas alternatives -------------
    auto& widen = addPanel ("Widen");
    widenPanel = &widen;
    addToggle (widen, ParamIDs::decorrelateEnabled, "Decorrelate");
    addKnob (widen, ParamIDs::decorrelateMode, "Mode");
    addKnob (widen, ParamIDs::decorrelateAmount, "Amount");
    addToggle (widen, ParamIDs::haasEnabled, "Haas Mode");
    addKnob (widen, ParamIDs::haasTimeMs, "Haas Time");

    // --- Output: trim + mono audition monitor -----------------------------
    auto& output = addPanel ("Output");
    outputPanel = &output;
    addKnob (output, ParamIDs::output, "Output");
    addToggle (output, ParamIDs::monoAudition, "Audition");

    addMeter (output, "Output correlation meter", "OUT");

    // --- Size: computed from the control tables above ---------------------
    const auto contentWidth = std::max ({ panelRequiredWidth (width),
                                          panelRequiredWidth (bands),
                                          panelRequiredWidth (safety),
                                          panelRequiredWidth (widen),
                                          panelRequiredWidth (output) });

    auto contentHeight = presetBarHeight;

    for (const auto& panel : panels)
        contentHeight += bandGap + panelRequiredHeight (*panel);

    setResizable (false, false);
    setSize (outerMargin * 2 + contentWidth, outerMargin * 2 + contentHeight);

    // Correlation-meter polling: ~30 Hz GUI-thread timer feeding the
    // ballistic needles; the processor getters are relaxed-atomic loads, so
    // this never touches the audio thread.
    startTimerHz (30);
}

FirmamentAudioProcessorEditor::~FirmamentAudioProcessorEditor()
{
    stopTimer();
    setLookAndFeel (nullptr);
}

FirmamentAudioProcessorEditor::Panel& FirmamentAudioProcessorEditor::addPanel (const juce::String& sectionTitle)
{
    auto panel = std::make_unique<Panel>();
    panel->component = std::make_unique<basilica::gui::BusPanel> (sectionTitle);
    panel->rows.emplace_back();

    addAndMakeVisible (*panel->component);

    panels.push_back (std::move (panel));
    return *panels.back();
}

FirmamentAudioProcessorEditor::Knob& FirmamentAudioProcessorEditor::addKnob (Panel& panel, const char* parameterId,
                                                                             const juce::String& labelText)
{
    auto knob = std::make_unique<Knob>();

    knob->slider.setTextBoxStyle (juce::Slider::TextBoxBelow, false, knobSlotWidth - slotGap, textBoxHeight);
    knob->slider.setTitle (labelText);
    knob->slider.setName (labelText);
    panel.component->addAndMakeVisible (knob->slider);

    knob->label.setText (labelText, juce::dontSendNotification);
    knob->label.setJustificationType (juce::Justification::centred);
    knob->label.attachToComponent (&knob->slider, false); // above; auto-repositions with the slider
    panel.component->addAndMakeVisible (knob->label);

    // SliderAttachment MUST be constructed before the textFromValueFunction
    // override below, not after: JUCE 8.0.14's SliderParameterAttachment
    // constructor (juce_ParameterAttachments.cpp:128) itself assigns
    // `slider.textFromValueFunction` as part of wiring the attachment -
    // setting our own function BEFORE this point would be silently
    // clobbered the moment the attachment is created.
    knob->attachment = std::make_unique<SliderAttachment> (audioProcessor.apvts, parameterId, knob->slider);

    if (auto* param = audioProcessor.apvts.getParameter (parameterId))
    {
        // A-02 pattern: unit-carrying parameters declare their unit via
        // .withLabel() in ParameterLayout.cpp (%/Hz/dB/ms) - feed it into
        // both the value box and the accessibility value string. Choice
        // parameters have an empty label and getText() already returns the
        // choice NAME, so this is a no-op suffix for them.
        knob->slider.textFromValueFunction = [param] (double v)
        {
            const auto text = param->getText (param->convertTo0to1 ((float) v), 0);
            const auto unit = param->getLabel();
            return unit.isEmpty() ? text : text + " " + unit;
        };
        knob->slider.updateText();
    }

    panel.rows.back().push_back (&knob->slider);
    knobs.push_back (std::move (knob));
    return *knobs.back();
}

FirmamentAudioProcessorEditor::Toggle& FirmamentAudioProcessorEditor::addToggle (Panel& panel, const char* parameterId,
                                                                                 const juce::String& labelText)
{
    auto toggle = std::make_unique<Toggle>();

    // Real juce::ToggleButton on purpose: focusable and Space/Enter-
    // operable by default, and its createAccessibilityHandler() reports
    // AccessibilityRole::toggleButton (JUCE 8.0.14 juce_ToggleButton.cpp:71)
    // so it lands in the VoiceOver rotor as a toggle, not a plain button.
    toggle->button.setButtonText (labelText);
    toggle->button.setTitle (labelText);
    toggle->button.setName (labelText);
    panel.component->addAndMakeVisible (toggle->button);

    toggle->attachment = std::make_unique<ButtonAttachment> (audioProcessor.apvts, parameterId, toggle->button);

    panel.rows.back().push_back (&toggle->button);
    toggles.push_back (std::move (toggle));
    return *toggles.back();
}

basilica::gui::CorrelationMeter& FirmamentAudioProcessorEditor::addMeter (Panel& panel,
                                                                          const juce::String& accessibleTitle,
                                                                          const juce::String& faceLegend)
{
    auto meter = std::make_unique<basilica::gui::CorrelationMeter> (accessibleTitle, faceLegend);
    panel.component->addAndMakeVisible (*meter);
    panel.meter = meter.get();

    meters.push_back (std::move (meter));
    return *meters.back();
}

void FirmamentAudioProcessorEditor::timerCallback()
{
    // Correlation in [-1, +1], straight from the engine's per-block
    // metering (relaxed atomic reads - see PluginProcessor.h).
    if (widthPanel != nullptr && widthPanel->meter != nullptr)
        widthPanel->meter->setTargetCorrelation (audioProcessor.getCorrelationMeterValue());

    if (outputPanel != nullptr && outputPanel->meter != nullptr)
        outputPanel->meter->setTargetCorrelation (audioProcessor.getOutputCorrelationMeterValue());

    constexpr float dtSeconds = 1.0f / 30.0f;

    for (auto& meter : meters)
        meter->tick (dtSeconds);
}

int FirmamentAudioProcessorEditor::slotWidthFor (const juce::Component& control) noexcept
{
    return dynamic_cast<const juce::Slider*> (&control) != nullptr ? knobSlotWidth : toggleSlotWidth;
}

int FirmamentAudioProcessorEditor::rowWidth (const std::vector<juce::Component*>& row) noexcept
{
    int width = 0;

    for (const auto* control : row)
        width += slotWidthFor (*control);

    return width;
}

int FirmamentAudioProcessorEditor::panelRequiredWidth (const Panel& panel) const noexcept
{
    int widest = 0;

    for (const auto& row : panel.rows)
        widest = std::max (widest, rowWidth (row));

    return panelPadding * 2 + widest + (panel.meter != nullptr ? meterBayWidth : 0);
}

int FirmamentAudioProcessorEditor::panelRequiredHeight (const Panel& panel) const noexcept
{
    const auto numRows = (int) panel.rows.size();
    return basilica::gui::BusPanel::headerHeight + numRows * rowHeight + panelBottomPadding;
}

void FirmamentAudioProcessorEditor::paint (juce::Graphics& g)
{
    g.fillAll (basilica::gui::BasilicaLookAndFeel::getEditorBackgroundColour());
}

void FirmamentAudioProcessorEditor::resized()
{
    auto bounds = getLocalBounds().reduced (outerMargin);

    presetBar.setBounds (bounds.removeFromTop (presetBarHeight));

    const auto layoutPanel = [] (Panel& panel, juce::Rectangle<int> area)
    {
        panel.component->setBounds (area);

        auto content = panel.component->getLocalBounds().reduced (panelPadding, 0);
        content.removeFromTop (basilica::gui::BusPanel::headerHeight);

        if (panel.meter != nullptr)
        {
            auto bay = content.removeFromRight (meterBayWidth);
            panel.meter->setBounds (juce::Rectangle<int> (meterWidth,
                                                          juce::jmin (meterHeight, bay.getHeight()))
                                        .withCentre (bay.getCentre()));
        }

        for (auto& row : panel.rows)
        {
            auto rowArea = content.removeFromTop (rowHeight);
            rowArea.removeFromTop (labelHeight); // attached labels position themselves here

            for (auto* control : row)
            {
                auto slot = rowArea.removeFromLeft (slotWidthFor (*control)).withTrimmedRight (slotGap);

                if (dynamic_cast<juce::Slider*> (control) != nullptr)
                    control->setBounds (slot.withHeight (knobSize + textBoxHeight));
                else
                    control->setBounds (slot.withSizeKeepingCentre (slot.getWidth(), toggleHeight)
                                            .withY (rowArea.getY() + (knobSize - toggleHeight) / 2));
            }
        }
    };

    for (auto& panel : panels)
    {
        bounds.removeFromTop (bandGap);
        layoutPanel (*panel, bounds.removeFromTop (panelRequiredHeight (*panel)));
    }
}
