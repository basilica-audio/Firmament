#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

namespace
{
    // Convenience wrapper: fetches a parameter by ID and requires it to
    // exist before returning, so every SECTION below fails loudly (not with
    // a null-deref) if an ID typo ever creeps in.
    juce::RangedAudioParameter* requireParam (juce::AudioProcessorValueTreeState& apvts, const juce::String& id)
    {
        auto* param = apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param;
    }

    // Checks that a float parameter's underlying NormalisableRange covers
    // [expectedMin, expectedMax], independent of any skew/log mapping.
    void checkFloatRange (juce::AudioProcessorValueTreeState& apvts,
                           const juce::String& id,
                           float expectedMin,
                           float expectedMax)
    {
        auto* param = dynamic_cast<juce::AudioParameterFloat*> (apvts.getParameter (id));
        REQUIRE (param != nullptr);

        const auto range = param->getNormalisableRange().getRange();
        CHECK (range.getStart() == Catch::Approx (expectedMin));
        CHECK (range.getEnd() == Catch::Approx (expectedMax));
    }

    // Checks a float parameter's default value in real (non-normalised)
    // units, going through convertTo0to1 so skewed ranges are handled the
    // same way as linear ones.
    void checkFloatDefault (juce::AudioProcessorValueTreeState& apvts,
                             const juce::String& id,
                             float expectedDefault)
    {
        auto* param = requireParam (apvts, id);
        CHECK (param->getDefaultValue() == Catch::Approx (param->convertTo0to1 (expectedDefault)).margin (1e-4));
    }
}

TEST_CASE ("Processor instantiates with the expected parameters", "[processor][parameters]")
{
    FirmamentAudioProcessor processor;
    auto& apvts = processor.apvts;

    SECTION ("plugin name")
    {
        CHECK (processor.getName() == juce::String ("Firmament"));
    }

    SECTION ("all documented parameter IDs resolve")
    {
        static constexpr const char* allIds[] = {
            ParamIDs::width, ParamIDs::bassMonoFreq, ParamIDs::output,
            ParamIDs::lowWidth, ParamIDs::autoMonoSafety, ParamIDs::haasEnabled, ParamIDs::haasTimeMs,
            ParamIDs::autoMonoSafetyFloorDb, ParamIDs::autoMonoSafetyMultiband,
            ParamIDs::decorrelateEnabled, ParamIDs::decorrelateAmount,
            ParamIDs::decorrelateMode, ParamIDs::bassMonoMode, ParamIDs::highSplitFreq,
            ParamIDs::highWidth, ParamIDs::safetyMode, ParamIDs::widthComp, ParamIDs::monoAudition,
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the v0.3.0 layout (11 v0.2.0 + 7 v0.3.0)")
    {
        CHECK (apvts.processor.getParameters().size() == 18);
    }

    SECTION ("Width: M/S width scale defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::width, 100.0f);
        checkFloatRange (apvts, ParamIDs::width, 0.0f, 200.0f);
    }

    SECTION ("Bass Mono Freq: crossover frequency defaults and range (v0.2.0: 600 Hz ceiling)")
    {
        checkFloatDefault (apvts, ParamIDs::bassMonoFreq, 0.0f);
        checkFloatRange (apvts, ParamIDs::bassMonoFreq, 0.0f, 600.0f);
    }

    SECTION ("Output: trim defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::output, 0.0f);
        checkFloatRange (apvts, ParamIDs::output, -24.0f, 24.0f);
    }

    SECTION ("Low Width: multiband low-band width scale defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::lowWidth, 0.0f);
        checkFloatRange (apvts, ParamIDs::lowWidth, 0.0f, 200.0f);
    }

    SECTION ("Auto Mono Safety: bool parameter defaults off")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::autoMonoSafety));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }

    SECTION ("Haas Mode: bool parameter defaults off")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::haasEnabled));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }

    SECTION ("Haas Time: defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::haasTimeMs, 20.0f);
        checkFloatRange (apvts, ParamIDs::haasTimeMs, 0.0f, 40.0f);
    }

    SECTION ("Auto Mono Safety Floor: defaults and range (v0.2.0)")
    {
        checkFloatDefault (apvts, ParamIDs::autoMonoSafetyFloorDb, -9.1f);
        checkFloatRange (apvts, ParamIDs::autoMonoSafetyFloorDb, -24.0f, 0.0f);
    }

    SECTION ("Auto Mono Safety Multiband: bool parameter defaults off (v0.2.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::autoMonoSafetyMultiband));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }

    SECTION ("Decorrelate: bool parameter defaults off (v0.2.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::decorrelateEnabled));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }

    SECTION ("Decorrelate Amount: defaults and range (v0.2.0)")
    {
        checkFloatDefault (apvts, ParamIDs::decorrelateAmount, 50.0f);
        checkFloatRange (apvts, ParamIDs::decorrelateAmount, 0.0f, 100.0f);
    }

    // v0.3.0 additions below - every default must be the neutral (exact
    // v0.2.0 behaviour) value; the migration null in StateTests.cpp proves
    // the neutrality end-to-end, these prove the layout contract.

    SECTION ("Decorrelate Mode: choice parameter with the three documented modes, defaults to Classic (v0.3.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ParamIDs::decorrelateMode));
        REQUIRE (param != nullptr);
        CHECK (param->choices.size() == 3);
        CHECK (param->choices[0] == "Classic");
        CHECK (param->choices[1] == "Velvet Dense");
        CHECK (param->choices[2] == "Velvet Sparse");
        CHECK (param->getIndex() == 0);
    }

    SECTION ("Bass Mono Mode: choice parameter with the three documented modes, defaults to Classic (v0.3.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ParamIDs::bassMonoMode));
        REQUIRE (param != nullptr);
        CHECK (param->choices.size() == 3);
        CHECK (param->choices[0] == "Classic");
        CHECK (param->choices[1] == "Phase Matched");
        CHECK (param->choices[2] == "Linear Phase");
        CHECK (param->getIndex() == 0);
    }

    SECTION ("High Split: defaults to the 0 Hz off sentinel, range 0-8000 Hz (v0.3.0)")
    {
        checkFloatDefault (apvts, ParamIDs::highSplitFreq, 0.0f);
        checkFloatRange (apvts, ParamIDs::highSplitFreq, 0.0f, 8000.0f);
    }

    SECTION ("High Width: defaults to unity 100%, range 0-200% (v0.3.0)")
    {
        checkFloatDefault (apvts, ParamIDs::highWidth, 100.0f);
        checkFloatRange (apvts, ParamIDs::highWidth, 0.0f, 200.0f);
    }

    SECTION ("Safety Response: choice parameter Smooth/Dynamic, defaults to Smooth (v0.3.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterChoice*> (apvts.getParameter (ParamIDs::safetyMode));
        REQUIRE (param != nullptr);
        CHECK (param->choices.size() == 2);
        CHECK (param->choices[0] == "Smooth");
        CHECK (param->choices[1] == "Dynamic");
        CHECK (param->getIndex() == 0);
    }

    SECTION ("Width Compensation: bool parameter defaults off (v0.3.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::widthComp));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }

    SECTION ("Mono Audition: bool parameter defaults off (v0.3.0)")
    {
        auto* param = dynamic_cast<juce::AudioParameterBool*> (apvts.getParameter (ParamIDs::monoAudition));
        REQUIRE (param != nullptr);
        CHECK (param->get() == false);
    }
}
