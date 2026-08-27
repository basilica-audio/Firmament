#include "PluginEditor.h"
#include "PluginEditorLayout.h"
#include "PluginProcessor.h"
#include "gui/ArcNeedleMeter.h"
#include "gui/MasterCropKnob.h"
#include "gui/SpriteToggle.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// GUI smoke + motion proofs for the wave-3 compositional editor
// (src/PluginEditor.h). juce::ScopedJuceInitialiser_GUI is installed once
// for the whole test binary in tests/TestMain.cpp.
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

    juce::Image snapshotOf (juce::Component& component)
    {
        // SoftwareImageType avoids any dependency on a native graphics
        // context/window - robust on headless CI runners.
        return component.createComponentSnapshot (component.getLocalBounds(), true, 1.0f,
                                                  juce::SoftwareImageType {});
    }

    int changedPixels (const juce::Image& a, const juce::Image& b, juce::Rectangle<int> area, int threshold = 24)
    {
        int changed = 0;

        for (int y = area.getY(); y < area.getBottom(); ++y)
        {
            for (int x = area.getX(); x < area.getRight(); ++x)
            {
                const auto ca = a.getPixelAt (x, y);
                const auto cb = b.getPixelAt (x, y);
                const auto diff = std::abs (ca.getRed() - cb.getRed())
                                 + std::abs (ca.getGreen() - cb.getGreen())
                                 + std::abs (ca.getBlue() - cb.getBlue());
                if (diff > threshold)
                    ++changed;
            }
        }

        return changed;
    }

    // A deliberately "alive-looking" state for the committed preview:
    // varied, non-default rotations, one lever down, the needle seeded to
    // a healthy positive correlation (setImmediateCorrelationForPreview()
    // bypasses the ballistic ramp this headless binary's absent message
    // loop could never pump).
    void configureLiveLookingState (FirmamentAudioProcessorEditor& editor)
    {
        struct KnobValue
        {
            const char* title;
            double proportion;
        };

        const KnobValue knobValues[] = {
            { "Bass Mono Freq", 0.45 }, { "Low Width", 0.35 }, { "Width", 0.65 },
            { "High Split", 0.55 }, { "High Width", 0.75 },
            { "Auto Mono Safety Floor", 0.40 }, { "Haas Time", 0.30 },
            { "Decorrelate Amount", 0.60 }, { "Output", 0.50 },
        };

        for (const auto& kv : knobValues)
            if (auto* knob = findChildByTitle<juce::Slider> (editor, kv.title))
                knob->setValue (knob->proportionOfLengthToValue (kv.proportion), juce::dontSendNotification);

        if (auto* meter = findChildByTitle<basilica::gui::ArcNeedleMeter> (editor, "Output Correlation meter"))
            meter->setImmediateCorrelationForPreview (0.55f);
    }
}

TEST_CASE ("Editor constructs, lays out, and destroys cleanly", "[gui]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    {
        FirmamentAudioProcessorEditor editor (processor);

        CHECK (editor.getWidth() > 0);
        CHECK (editor.getHeight() > 0);
    }
    // editor destroyed here - JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR
    // asserts at process exit in Debug builds if any tagged instance leaked.
}

TEST_CASE ("Editor snapshot at 100% is non-blank and is written for PR review", "[gui]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    FirmamentAudioProcessorEditor editor (processor);
    REQUIRE (editor.getWidth() > 0);
    REQUIRE (editor.getHeight() > 0);

    configureLiveLookingState (editor);

    const auto snapshot = snapshotOf (editor);

    REQUIRE (snapshot.isValid());
    CHECK (snapshot.getWidth() == editor.getWidth());
    CHECK (snapshot.getHeight() == editor.getHeight());

    const auto reference = snapshot.getPixelAt (0, 0);
    bool foundDifference = false;

    for (int y = 0; y < snapshot.getHeight() && ! foundDifference; y += juce::jmax (1, snapshot.getHeight() / 20))
        for (int x = 0; x < snapshot.getWidth() && ! foundDifference; x += juce::jmax (1, snapshot.getWidth() / 20))
            if (snapshot.getPixelAt (x, y) != reference)
                foundDifference = true;

    CHECK (foundDifference);

#ifdef FIRMAMENT_DOCS_DIR
    // Committed directly for PR review (docs/gui-preview.png) - a TRUE
    // render of the editor tree via the real JUCE draw chain (proof-chain
    // rule), never a hand-mocked composite.
    juce::PNGImageFormat pngFormat;
    const auto outFile = juce::File (FIRMAMENT_DOCS_DIR).getChildFile ("gui-preview.png");

    if (auto stream = std::unique_ptr<juce::FileOutputStream> (outFile.createOutputStream()))
    {
        stream->setPosition (0);
        stream->truncate();
        CHECK (pngFormat.writeImageToStream (snapshot, *stream));
    }
    else
    {
        FAIL ("could not open output stream for " << outFile.getFullPathName());
    }
#endif
}

// Proof that the rotating cap crops actually move: knobs/selectors set to
// distinctly non-default proportions must visibly differ, within their own
// bounds, from their construction-time rendering.
TEST_CASE ("Knob and selector caps visibly rotate at non-default values", "[gui]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    FirmamentAudioProcessorEditor editor (processor);
    const auto restSnapshot = snapshotOf (editor);
    REQUIRE (restSnapshot.isValid());

    struct ZoomKnob
    {
        const char* title;
        double proportion;
    };

    // Two continuous knobs at the sweep extremes plus one stepped selector
    // moved off its default detent.
    constexpr ZoomKnob zoomKnobs[] = {
        { "Width", 0.02 },
        { "Output", 0.98 },
        { "Bass Mono Mode", 1.0 },
    };

    for (const auto& zk : zoomKnobs)
    {
        auto* knob = findChildByTitle<juce::Slider> (editor, zk.title);
        REQUIRE (knob != nullptr);
        knob->setValue (knob->proportionOfLengthToValue (zk.proportion), juce::dontSendNotification);
    }

    const auto movedSnapshot = snapshotOf (editor);
    REQUIRE (movedSnapshot.isValid());

    for (const auto& zk : zoomKnobs)
    {
        auto* knob = findChildByTitle<juce::Slider> (editor, zk.title);
        REQUIRE (knob != nullptr);

        const auto area = knob->getBounds().expanded (2);
        const auto changed = changedPixels (restSnapshot, movedSnapshot, area);
        const auto total = area.getWidth() * area.getHeight();

        INFO (zk.title << ": " << changed << "/" << total << " px changed between rest and moved pose");
        CHECK (changed > total / 40);
    }
}

// Proof that the lever toggles' two states are visibly distinct (the OFF
// state is the mirrored draw - see SpriteToggle.h's asset-gap docs).
TEST_CASE ("Toggle lever states are visibly distinct", "[gui]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    FirmamentAudioProcessorEditor editor (processor);

    auto* toggle = findChildByTitle<basilica::gui::SpriteToggle> (editor, "Auto Mono Safety");
    REQUIRE (toggle != nullptr);

    toggle->setToggleState (false, juce::dontSendNotification);
    const auto offSnapshot = snapshotOf (editor);

    toggle->setToggleState (true, juce::dontSendNotification);
    const auto onSnapshot = snapshotOf (editor);

    const auto changed = changedPixels (offSnapshot, onSnapshot, toggle->getBounds());
    INFO (changed << " px changed between lever states");
    CHECK (changed > 50);
}

// Proof that the needle is a LIVE overlay reading real meter state: two
// distinct seeded correlation readings must visibly differ inside the arc
// dial bounds.
TEST_CASE ("The correlation needle visibly tracks its reading", "[gui]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    FirmamentAudioProcessorEditor editor (processor);

    auto* meter = findChildByTitle<basilica::gui::ArcNeedleMeter> (editor, "Output Correlation meter");
    REQUIRE (meter != nullptr);

    meter->setImmediateCorrelationForPreview (-0.8f);
    const auto leftSnapshot = snapshotOf (editor);

    meter->setImmediateCorrelationForPreview (+0.8f);
    const auto rightSnapshot = snapshotOf (editor);

    const auto changed = changedPixels (leftSnapshot, rightSnapshot, meter->getBounds());
    INFO (changed << " px changed between -0.8 and +0.8 correlation poses");
    CHECK (changed > 100);
}
