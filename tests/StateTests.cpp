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

TEST_CASE ("State round-trip preserves non-default values of every M1/v0.2.0 parameter (multiband/safety/Haas/Decorrelate)", "[state]")
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
        { ParamIDs::autoMonoSafetyFloorDb, -16.0f },
        { ParamIDs::autoMonoSafetyMultiband, 1.0f },
        { ParamIDs::decorrelateEnabled, 1.0f },
        { ParamIDs::decorrelateAmount, 72.0f },
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

TEST_CASE ("State round-trip: a v0.1.1-style state missing the four v0.2.0 parameters loads cleanly with new params at their defaults", "[state]")
{
    // docs/design-brief.md's Versioning section: v0.1.1 states load cleanly
    // with autoMonoSafetyFloorDb/autoMonoSafetyMultiband/decorrelateEnabled/
    // decorrelateAmount entirely absent, defaulting to values that reproduce
    // v0.1.1 behaviour exactly (Floor -9.1 dB matches the old hardcoded 0.35
    // linear floor, Multiband/Decorrelate off) - the AudioProcessorValueTreeState
    // tolerant-load behaviour already relied on elsewhere in the suite.
    FirmamentAudioProcessor sourceProcessor;
    sourceProcessor.prepareToPlay (48000.0, 512);

    auto setSourceParam = [&] (const char* id, float realValue)
    {
        auto* param = sourceProcessor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    };

    setSourceParam (ParamIDs::width, 165.0f);
    setSourceParam (ParamIDs::bassMonoFreq, 175.0f);
    setSourceParam (ParamIDs::output, 2.5f);

    auto state = sourceProcessor.apvts.copyState();

    // Simulate a genuine v0.1.1 state: strip the four v0.2.0-only parameter
    // children entirely, as a state saved before those IDs existed would
    // never have carried them.
    static constexpr const char* v020OnlyIds[] = {
        ParamIDs::autoMonoSafetyFloorDb, ParamIDs::autoMonoSafetyMultiband,
        ParamIDs::decorrelateEnabled, ParamIDs::decorrelateAmount,
    };

    for (const auto* id : v020OnlyIds)
    {
        const auto child = state.getChildWithProperty ("id", juce::String (id));
        REQUIRE (child.isValid());
        state.removeChild (child, nullptr);
    }

    for (const auto* id : v020OnlyIds)
        REQUIRE (! state.getChildWithProperty ("id", juce::String (id)).isValid());

    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    juce::MemoryBlock v011StyleState;
    juce::AudioProcessor::copyXmlToBinary (*xml, v011StyleState);

    // Load into a fresh processor, as a host would on session open.
    FirmamentAudioProcessor destProcessor;
    destProcessor.prepareToPlay (48000.0, 512);

    CHECK_NOTHROW (destProcessor.setStateInformation (v011StyleState.getData(), static_cast<int> (v011StyleState.getSize())));

    auto getDestParam = [&] (const char* id)
    {
        auto* param = destProcessor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        return param->convertFrom0to1 (param->getValue());
    };

    // The surviving "old" parameters loaded their saved values...
    CHECK (getDestParam (ParamIDs::width) == Catch::Approx (165.0f).margin (1e-3));
    CHECK (getDestParam (ParamIDs::bassMonoFreq) == Catch::Approx (175.0f).margin (1e-3));
    CHECK (getDestParam (ParamIDs::output) == Catch::Approx (2.5f).margin (1e-3));

    // ...and the four v0.2.0-only parameters, entirely absent from the
    // loaded state, are left at their ParameterLayout defaults.
    for (const auto* id : v020OnlyIds)
    {
        auto* param = destProcessor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        CHECK (param->getValue() == Catch::Approx (param->getDefaultValue()).margin (1e-6));
    }
}
