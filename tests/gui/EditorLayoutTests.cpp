#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/BusPanel.h"
#include "gui/CorrelationMeter.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <functional>
#include <vector>

// M3 vector-editor layout tests (issue #4). Unlike the photoreal siblings
// (whose EditorLayoutTests assert hand-measured pixel manifests against
// baked master renders), Firmament's geometry is COMPUTED from the layout
// constants + control tables in PluginEditor.cpp - so the manifest under
// test here is the real constructed component tree itself: containment, no
// overlap, and full parameter coverage. Any arithmetic slip in the layout
// constants shows up as a concrete clipped/colliding control here.
namespace
{
    template <typename ComponentType>
    void visitDescendants (juce::Component& parent, const std::function<void (ComponentType&)>& visit)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
        {
            auto* child = parent.getChildComponent (i);

            if (auto* typed = dynamic_cast<ComponentType*> (child))
                visit (*typed);

            visitDescendants<ComponentType> (*child, visit);
        }
    }

    juce::Rectangle<int> boundsInEditor (const juce::Component& component, const juce::Component& editor)
    {
        auto bounds = component.getBounds();

        for (const auto* ancestor = component.getParentComponent();
             ancestor != nullptr && ancestor != &editor;
             ancestor = ancestor->getParentComponent())
        {
            bounds += ancestor->getPosition();
        }

        return bounds;
    }
}

TEST_CASE ("Every automatable parameter has exactly one attached control", "[gui][layout]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    int sliders = 0, toggles = 0;
    visitDescendants<juce::Slider> (editor, [&] (juce::Slider&) { ++sliders; });
    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton&) { ++toggles; });

    // The APVTS carries 9 float + 3 choice + 6 bool parameters = 18
    // (ParameterIds.h). One knob per float/choice parameter, one toggle per
    // bool parameter - no parameter may be left off the M3 surface, and no
    // control may exist without a parameter.
    CHECK ((int) processor.getParameters().size() == 18);
    CHECK (sliders + toggles == (int) processor.getParameters().size());
    CHECK (sliders == 12);
    CHECK (toggles == 6);
}

TEST_CASE ("Moving a knob moves its parameter - one wiring spot check per section", "[gui][layout]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* title;      // unique across the whole editor
        const char* parameterId;
        double sliderValue;     // a legal, non-default value
        float expectedRaw;      // denormalised parameter value afterwards
    };

    const Expectation expectations[] = {
        { "Width", "width", 150.0, 150.0f },                 // Width
        { "High Width", "highWidth", 150.0, 150.0f },        // Bands
        { "Floor", "autoMonoSafetyFloorDb", -12.0, -12.0f }, // Mono Safety
        { "Haas Time", "haasTimeMs", 10.0, 10.0f },          // Widen
        { "Output", "output", 6.0, 6.0f },                   // Output
    };

    for (const auto& expectation : expectations)
    {
        juce::Slider* knob = nullptr;
        visitDescendants<juce::Slider> (editor, [&] (juce::Slider& s)
        {
            if (s.getTitle() == expectation.title)
                knob = &s;
        });

        REQUIRE (knob != nullptr);
        INFO ("knob \"" << expectation.title << "\" -> " << expectation.parameterId);

        auto* raw = processor.apvts.getRawParameterValue (expectation.parameterId);
        REQUIRE (raw != nullptr);

        knob->setValue (expectation.sliderValue, juce::sendNotificationSync);
        CHECK (raw->load() == Catch::Approx (expectation.expectedRaw).margin (1.0e-4));
    }
}

TEST_CASE ("All controls, labels and meters stay inside their panel; panels stay inside the editor", "[gui][layout]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    const auto editorBounds = editor.getLocalBounds();
    CHECK (editorBounds.getWidth() > 0);
    CHECK (editorBounds.getHeight() > 0);

    int panelsSeen = 0;

    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        ++panelsSeen;
        INFO ("panel \"" << panel.getTitle().toStdString() << "\" bounds "
              << panel.getBounds().toString().toStdString());
        CHECK (editorBounds.contains (panel.getBounds()));

        // Every direct child (knobs incl. their value boxes, attached
        // labels, toggles, the meter) must be fully inside the panel.
        for (int i = 0; i < panel.getNumChildComponents(); ++i)
        {
            const auto* child = panel.getChildComponent (i);
            INFO ("child \"" << child->getName().toStdString() << "\" bounds "
                  << child->getBounds().toString().toStdString()
                  << " in panel \"" << panel.getTitle().toStdString() << "\" "
                  << panel.getLocalBounds().toString().toStdString());
            CHECK (panel.getLocalBounds().contains (child->getBounds()));
        }
    });

    CHECK (panelsSeen == 5);
}

TEST_CASE ("No two interactive controls or meters overlap", "[gui][layout]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    struct Entry
    {
        juce::String name;
        juce::Rectangle<int> bounds;
    };

    std::vector<Entry> entries;

    const auto collect = [&] (juce::Component& component)
    {
        entries.push_back ({ component.getTitle(), boundsInEditor (component, editor) });
    };

    visitDescendants<juce::Slider> (editor, [&] (juce::Slider& s) { collect (s); });
    visitDescendants<juce::ToggleButton> (editor, [&] (juce::ToggleButton& t) { collect (t); });
    visitDescendants<basilica::gui::CorrelationMeter> (editor, [&] (basilica::gui::CorrelationMeter& m) { collect (m); });

    // 12 knobs + 6 toggles + 2 meters - the pairwise scan below must not
    // pass vacuously on an empty collection.
    REQUIRE (entries.size() == 20);

    for (size_t i = 0; i < entries.size(); ++i)
    {
        for (size_t j = i + 1; j < entries.size(); ++j)
        {
            INFO ("\"" << entries[i].name.toStdString() << "\" " << entries[i].bounds.toString().toStdString()
                  << " vs \"" << entries[j].name.toStdString() << "\" " << entries[j].bounds.toString().toStdString());
            CHECK_FALSE (entries[i].bounds.intersects (entries[j].bounds));
        }
    }
}

TEST_CASE ("Panels do not overlap each other or the preset bar", "[gui][layout]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    std::vector<juce::Rectangle<int>> panelBounds;

    visitDescendants<basilica::gui::BusPanel> (editor, [&] (basilica::gui::BusPanel& panel)
    {
        panelBounds.push_back (panel.getBounds());
    });

    REQUIRE (panelBounds.size() == 5);

    for (size_t i = 0; i < panelBounds.size(); ++i)
        for (size_t j = i + 1; j < panelBounds.size(); ++j)
            CHECK_FALSE (panelBounds[i].intersects (panelBounds[j]));

    // The preset bar band sits above all panels.
    juce::Component* presetBar = nullptr;

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
        if (dynamic_cast<basilica::presets::PresetBar*> (editor.getChildComponent (i)) != nullptr)
            presetBar = editor.getChildComponent (i);

    REQUIRE (presetBar != nullptr);

    for (const auto& bounds : panelBounds)
        CHECK_FALSE (presetBar->getBounds().intersects (bounds));
}

TEST_CASE ("Every knob's visible label text matches its accessible title (label-in-name)", "[gui][layout][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    // WCAG 2.5.3 Label in Name: the label painted next to a knob must be
    // the same string AT users hear as the control's name - a mismatch
    // breaks voice-control users ("click Width" targeting a control whose
    // accessible name is something else).
    int labelledKnobs = 0;

    visitDescendants<juce::Label> (editor, [&] (juce::Label& label)
    {
        if (auto* attached = label.getAttachedComponent())
        {
            if (auto* slider = dynamic_cast<juce::Slider*> (attached))
            {
                ++labelledKnobs;
                INFO ("label \"" << label.getText().toStdString() << "\" for knob \""
                      << slider->getTitle().toStdString() << "\"");
                CHECK (label.getText() == slider->getTitle());
            }
        }
    });

    // Every one of the 12 knobs carries an attached, matching label.
    CHECK (labelledKnobs == 12);
}
