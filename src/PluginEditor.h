#pragma once

#include <juce_audio_processors/juce_audio_processors.h>

#include <memory>
#include <vector>

#include "gui/BasilicaLookAndFeel.h"
#include "gui/BusPanel.h"
#include "gui/CorrelationMeter.h"
#include "gui/PointerKnob.h"
#include "presets/PresetBar.h"

class FirmamentAudioProcessor;

// M3 custom vector editor + accessible parameter surface (issue #4), ported
// from Miserere's merged M3 implementation (basilica-audio/miserere PR #31).
//
// Everything is drawn at runtime by BasilicaLookAndFeel / the src/gui
// components - no photoreal PNG assets exist in this plugin (unlike the
// filmstrip/faceplate siblings): pointer knobs with engraved scale rings,
// lamp toggles, and two vector correlation needle meters (input and output
// - the broadband meter surface FirmamentAudioProcessor exposes), grouped
// into one BusPanel per processing section in signal-flow order (Width /
// Bands / Mono Safety / Widen / Output).
//
// FOCUS ORDER CONTRACT (WCAG 2.4.3, suite-wide convention): JUCE's default
// traverser walks children in z-order, which equals CREATION order - the
// constructor therefore creates every control in signal-flow/reading order
// (preset bar first, then panel by panel, left-to-right within each row),
// and nothing may reorder children afterwards. Each BusPanel is an
// accessibility focus container (NOT a keyboard focus container - see
// BusPanel.h), so screen readers hear "Mono Safety, Floor" while Tab still
// walks the whole editor.
//
// Controls are built data-driven from ID/label tables (see the .cpp) - all
// float AND choice parameters are PointerKnobs (choice knobs snap to their
// integer detents and announce the choice NAME - the v0.3.0 interim
// editor's ComboBox selectors are gone), bool parameters are real
// juce::ToggleButtons (focusable and Space/Enter-operable out of the box,
// reported as toggle buttons by AT).
class FirmamentAudioProcessorEditor final : public juce::AudioProcessorEditor,
                                            private juce::Timer
{
public:
    explicit FirmamentAudioProcessorEditor (FirmamentAudioProcessor& processorToEdit);
    ~FirmamentAudioProcessorEditor() override;

    void paint (juce::Graphics& g) override;
    void resized() override;

private:
    using SliderAttachment = juce::AudioProcessorValueTreeState::SliderAttachment;
    using ButtonAttachment = juce::AudioProcessorValueTreeState::ButtonAttachment;

    struct Knob
    {
        basilica::gui::PointerKnob slider;
        juce::Label label;
        std::unique_ptr<SliderAttachment> attachment;
    };

    struct Toggle
    {
        juce::ToggleButton button;
        std::unique_ptr<ButtonAttachment> attachment;
    };

    // One section faceplate: the BusPanel component plus its control rows
    // (each row a left-to-right list of the controls laid out in it) and
    // an optional correlation needle meter in the panel's right bay.
    struct Panel
    {
        std::unique_ptr<basilica::gui::BusPanel> component;
        std::vector<std::vector<juce::Component*>> rows;
        basilica::gui::CorrelationMeter* meter = nullptr; // owned via `meters`
    };

    Panel& addPanel (const juce::String& sectionTitle);
    Knob& addKnob (Panel& panel, const char* parameterId, const juce::String& labelText);
    Toggle& addToggle (Panel& panel, const char* parameterId, const juce::String& labelText);
    basilica::gui::CorrelationMeter& addMeter (Panel& panel, const juce::String& accessibleTitle,
                                               const juce::String& faceLegend);

    void timerCallback() override;

    static int slotWidthFor (const juce::Component& control) noexcept;
    static int rowWidth (const std::vector<juce::Component*>& row) noexcept;
    int panelRequiredWidth (const Panel& panel) const noexcept;
    int panelRequiredHeight (const Panel& panel) const noexcept;

    FirmamentAudioProcessor& audioProcessor;

    // Must be constructed before any child that paints with it and
    // installed on `this` so it propagates to every child (including the
    // preset bar's stock buttons/menus/dialogs).
    basilica::gui::BasilicaLookAndFeel lookAndFeel;

    // M2 preset system - constructed after the localisation frame is
    // installed (see the constructor) so its TRANS()'d strings pick up the
    // right language from the very first paint.
    basilica::presets::PresetBar presetBar;

    std::vector<std::unique_ptr<Knob>> knobs;
    std::vector<std::unique_ptr<Toggle>> toggles;
    std::vector<std::unique_ptr<basilica::gui::CorrelationMeter>> meters;
    std::vector<std::unique_ptr<Panel>> panels;

    // Signal-flow panels, kept as raw pointers into `panels` for layout:
    // all five stack as full-width bands, top to bottom.
    Panel* widthPanel = nullptr;
    Panel* bandsPanel = nullptr;
    Panel* safetyPanel = nullptr;
    Panel* widenPanel = nullptr;
    Panel* outputPanel = nullptr;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentAudioProcessorEditor)
};
