#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <vector>

TEST_CASE ("State round-trip preserves non-default values of every parameter", "[state]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* widthParam = processor.apvts.getParameter (ParamIDs::width);
    auto* bassMonoParam = processor.apvts.getParameter (ParamIDs::bassMonoFreq);
    auto* outputParam = processor.apvts.getParameter (ParamIDs::output);

    REQUIRE (widthParam != nullptr);
    REQUIRE (bassMonoParam != nullptr);
    REQUIRE (outputParam != nullptr);

    widthParam->setValueNotifyingHost (widthParam->convertTo0to1 (175.0f));
    bassMonoParam->setValueNotifyingHost (bassMonoParam->convertTo0to1 (220.0f));
    outputParam->setValueNotifyingHost (outputParam->convertTo0to1 (-4.5f));

    const auto savedWidth = widthParam->getValue();
    const auto savedBassMono = bassMonoParam->getValue();
    const auto savedOutput = outputParam->getValue();

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);
    REQUIRE (savedState.getSize() > 0);

    // Reset every parameter back to its default before restoring, so the
    // round-trip assertion below can't pass by accident.
    widthParam->setValueNotifyingHost (widthParam->getDefaultValue());
    bassMonoParam->setValueNotifyingHost (bassMonoParam->getDefaultValue());
    outputParam->setValueNotifyingHost (outputParam->getDefaultValue());

    REQUIRE (widthParam->getValue() != Catch::Approx (savedWidth));
    REQUIRE (bassMonoParam->getValue() != Catch::Approx (savedBassMono));
    REQUIRE (outputParam->getValue() != Catch::Approx (savedOutput));

    processor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    CHECK (widthParam->getValue() == Catch::Approx (savedWidth).margin (1e-6));
    CHECK (bassMonoParam->getValue() == Catch::Approx (savedBassMono).margin (1e-6));
    CHECK (outputParam->getValue() == Catch::Approx (savedOutput).margin (1e-6));
}

TEST_CASE ("State round-trip preserves non-default values of every M1 parameter (multiband/safety/Haas)", "[state]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    struct ParamCase
    {
        const char* id;
        float nonDefaultValue;
    };

    const ParamCase cases[] = {
        { ParamIDs::width, 175.0f },
        { ParamIDs::bassMonoFreq, 220.0f },
        { ParamIDs::lowWidth, 80.0f },
        { ParamIDs::autoMonoSafety, 1.0f },
        { ParamIDs::haasEnabled, 1.0f },
        { ParamIDs::haasTimeMs, 33.0f },
        { ParamIDs::output, -4.5f },
    };

    std::vector<juce::RangedAudioParameter*> params;
    std::vector<float> savedValues;

    for (const auto& c : cases)
    {
        auto* param = processor.apvts.getParameter (c.id);
        REQUIRE (param != nullptr);

        param->setValueNotifyingHost (param->convertTo0to1 (c.nonDefaultValue));
        params.push_back (param);
        savedValues.push_back (param->getValue());
    }

    juce::MemoryBlock savedState;
    processor.getStateInformation (savedState);
    REQUIRE (savedState.getSize() > 0);

    // Reset every parameter back to its default before restoring, so the
    // round-trip assertions below can't pass by accident.
    for (auto* param : params)
        param->setValueNotifyingHost (param->getDefaultValue());

    for (size_t i = 0; i < params.size(); ++i)
        REQUIRE (params[i]->getValue() != Catch::Approx (savedValues[i]));

    processor.setStateInformation (savedState.getData(), static_cast<int> (savedState.getSize()));

    for (size_t i = 0; i < params.size(); ++i)
        CHECK (params[i]->getValue() == Catch::Approx (savedValues[i]).margin (1e-6));
}
