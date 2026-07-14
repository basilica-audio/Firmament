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
        };

        for (const auto* id : allIds)
            CHECK (apvts.getParameter (id) != nullptr);
    }

    SECTION ("total parameter count matches the M1 layout")
    {
        CHECK (apvts.processor.getParameters().size() == 7);
    }

    SECTION ("Width: M/S width scale defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::width, 100.0f);
        checkFloatRange (apvts, ParamIDs::width, 0.0f, 200.0f);
    }

    SECTION ("Bass Mono Freq: crossover frequency defaults and range")
    {
        checkFloatDefault (apvts, ParamIDs::bassMonoFreq, 0.0f);
        checkFloatRange (apvts, ParamIDs::bassMonoFreq, 0.0f, 500.0f);
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
}
