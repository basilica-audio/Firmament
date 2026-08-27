#include "PluginEditor.h"
#include "PluginProcessor.h"
#include "gui/ArcNeedleMeter.h"
#include "gui/MasterCropKnob.h"
#include "gui/SpriteToggle.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

// Accessibility tests for the wave-3 compositional editor, carrying over
// the suite's M3 a11y review contract: assert the actual
// AccessibilityHandler-level behaviour, not just that the editor
// constructs. juce::ScopedJuceInitialiser_GUI is installed once for the
// whole test binary in tests/TestMain.cpp.
//
// createAccessibilityHandler() is called directly rather than
// getAccessibilityHandler(): the latter (JUCE 8.0.14
// juce_Component.cpp:3323-3326) only returns a handler once the component
// has a live native window peer, which this headless test binary never
// has.
namespace
{
    template <typename ComponentType>
    ComponentType* findChildByTitle (juce::Component& parent, const juce::String& title)
    {
        for (int i = 0; i < parent.getNumChildComponents(); ++i)
            if (auto* typed = dynamic_cast<ComponentType*> (parent.getChildComponent (i)))
                if (typed->getTitle() == title)
                    return typed;

        return nullptr;
    }

    std::unique_ptr<juce::AccessibilityHandler> createHandlerForTest (juce::Component& component)
    {
        return component.createAccessibilityHandler();
    }
}

TEST_CASE ("Knob accessibility value strings include their declared unit", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* title;
        const char* unitSuffix;
    };

    // One representative knob per unit declared in ParameterLayout.cpp
    // (.withLabel("dB"/"Hz"/"%"/"ms")).
    const Expectation expectations[] = {
        { "Width", "%" },
        { "Bass Mono Freq", "Hz" },
        { "Output", "dB" },
        { "Haas Time", "ms" },
    };

    for (const auto& expectation : expectations)
    {
        auto* knob = findChildByTitle<basilica::gui::MasterCropKnob> (editor, expectation.title);
        REQUIRE (knob != nullptr);

        const auto handler = createHandlerForTest (*knob);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("knob \"" << expectation.title << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText.endsWith (expectation.unitSuffix));
    }
}

TEST_CASE ("Selector knobs announce their choice NAME, not a bare index", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    struct Expectation
    {
        const char* title;
        const char* defaultChoiceName;
    };

    // All three discrete parameters (ParameterLayout.cpp defaults).
    const Expectation expectations[] = {
        { "Bass Mono Mode", "Classic" },
        { "Safety Response", "Smooth" },
        { "Decorrelate Mode", "Classic" },
    };

    for (const auto& expectation : expectations)
    {
        auto* selector = findChildByTitle<basilica::gui::MasterCropKnob> (editor, expectation.title);
        REQUIRE (selector != nullptr);

        const auto handler = createHandlerForTest (*selector);
        REQUIRE (handler != nullptr);

        auto* valueInterface = handler->getValueInterface();
        REQUIRE (valueInterface != nullptr);

        const auto valueText = valueInterface->getCurrentValueAsString();
        INFO ("selector \"" << expectation.title << "\" accessible value = \"" << valueText.toStdString() << "\"");
        CHECK (valueText.contains (expectation.defaultChoiceName));

        // The stepped selector really snaps: its slider interval is 1.
        CHECK (selector->getInterval() == Catch::Approx (1.0));
    }
}

TEST_CASE ("Every toggle exposes its parameter name and a checkable state", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    // The 6 D4 toggles carry no engraved labels (master-05 family
    // precedent; group rules + captions dress the zones instead), so
    // their accessible titles are the ONLY name surface - assert every
    // single one.
    const char* titles[] = {
        "Auto Mono Safety", "Auto Mono Safety Multiband", "Width Compensation",
        "Haas Mode", "Decorrelate", "Mono Audition",
    };

    for (const auto* title : titles)
    {
        auto* toggle = findChildByTitle<basilica::gui::SpriteToggle> (editor, title);
        INFO ("toggle \"" << title << "\"");
        REQUIRE (toggle != nullptr);

        const auto handler = createHandlerForTest (*toggle);
        REQUIRE (handler != nullptr);
        CHECK (handler->getCurrentState().isCheckable());
    }
}

TEST_CASE ("The correlation arc meter is a titled display-only element with a read-only value", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    auto* meter = findChildByTitle<basilica::gui::ArcNeedleMeter> (editor, "Output Correlation meter");
    REQUIRE (meter != nullptr);

    CHECK_FALSE (meter->getWantsKeyboardFocus());

    bool clicksOnSelf = true, clicksOnChildren = true;
    meter->getInterceptsMouseClicks (clicksOnSelf, clicksOnChildren);
    CHECK_FALSE (clicksOnSelf);

    const auto handler = createHandlerForTest (*meter);
    REQUIRE (handler != nullptr);

    auto* valueInterface = handler->getValueInterface();
    REQUIRE (valueInterface != nullptr);
    CHECK (valueInterface->isReadOnly());

    meter->setImmediateCorrelationForPreview (0.5f);
    CHECK (valueInterface->getCurrentValueAsString().contains ("0.5"));
}

TEST_CASE ("Scale button's accessible title reflects the current scale percentage, not a static string", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    auto* scaleButton = dynamic_cast<juce::TextButton*> (editor.findChildWithID ("scaleButton"));
    REQUIRE (scaleButton != nullptr);

    CHECK (scaleButton->getTitle().contains ("100%"));

    REQUIRE (scaleButton->onClick);
    scaleButton->onClick();

    CHECK (scaleButton->getButtonText() == "150%");
    CHECK (scaleButton->getTitle().contains ("150%"));
    CHECK_FALSE (scaleButton->getTitle().contains ("100%"));
}

TEST_CASE ("Every interactive control is keyboard-focusable", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    int slidersSeen = 0, togglesSeen = 0;

    for (int i = 0; i < editor.getNumChildComponents(); ++i)
    {
        auto* child = editor.getChildComponent (i);

        if (auto* slider = dynamic_cast<juce::Slider*> (child))
        {
            ++slidersSeen;
            INFO ("slider \"" << slider->getTitle().toStdString() << "\"");
            CHECK (slider->getWantsKeyboardFocus());
        }
        else if (auto* toggle = dynamic_cast<basilica::gui::SpriteToggle*> (child))
        {
            ++togglesSeen;
            CHECK (toggle->getWantsKeyboardFocus());
        }
    }

    // All 9 knobs + the 3 stepped selectors are sliders; the 6 D4 levers
    // are SpriteToggles. A zero-match loop must not pass vacuously.
    CHECK (slidersSeen == 12);
    CHECK (togglesSeen == 6);

    auto* scaleButton = editor.findChildWithID ("scaleButton");
    REQUIRE (scaleButton != nullptr);
    CHECK (scaleButton->getWantsKeyboardFocus());
}

TEST_CASE ("Arrow keys step knobs by a practical amount, Shift+Arrow steps finer", "[gui][a11y]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);
    FirmamentAudioProcessorEditor editor (processor);

    auto* knob = findChildByTitle<basilica::gui::MasterCropKnob> (editor, "Width");
    REQUIRE (knob != nullptr);

    const auto range = knob->getMaximum() - knob->getMinimum();
    knob->setValue (knob->getMinimum() + range * 0.5, juce::dontSendNotification);
    const auto before = knob->getValue();

    REQUIRE (knob->keyPressed (juce::KeyPress (juce::KeyPress::rightKey)));
    const auto coarseStep = knob->getValue() - before;

    // WAI-ARIA slider pattern: Arrow ~1% of the range.
    CHECK (coarseStep > range * 0.005);
    CHECK (coarseStep < range * 0.02);

    const auto beforeFine = knob->getValue();
    REQUIRE (knob->keyPressed (juce::KeyPress (juce::KeyPress::rightKey,
                                               juce::ModifierKeys::shiftModifier, 0)));
    const auto fineStep = knob->getValue() - beforeFine;

    CHECK (fineStep > 0.0);
    CHECK (fineStep < coarseStep);
}
