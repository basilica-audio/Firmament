#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
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

// ===========================================================================
// v0.3.0 state-schema migration nulls (binding brief, section 6.1).
//
// Fixtures (tests/fixtures/, generated ONCE from the actual v0.2.0 binary at
// origin/main commit 0bc7b4e and frozen - see tests/fixtures/README.md):
//   - v020-state.bin: the exact getStateInformation() bytes a v0.2.0 host
//     session would have saved with width=140%, bassMonoFreq=120 Hz,
//     autoMonoSafety on, decorrelateEnabled on (Classic).
//   - v020-reference-render.f32: the v0.2.0 binary's render of 5 s (469
//     blocks of 512 samples @48 kHz) of the deterministic pink-noise
//     stimulus below, stored as raw little-endian float32, all left samples
//     then all right samples.
//
// The stimulus (TestHelpers::DeterministicPinkNoise, seeds below) is
// bit-exact cross-platform by construction, so any residual against the
// reference measures the *processing*, not the stimulus.
namespace
{
    juce::File fixturesDirectory()
    {
        return juce::File (__FILE__).getSiblingFile ("fixtures");
    }

    void setPlainParam (FirmamentAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    // Applies the v0.2.0-equivalent parameter set programmatically (all
    // v0.3.0 parameters left at their neutral defaults) - the exact values
    // the fixture state was generated with.
    void applyFixtureEquivalentParameters (FirmamentAudioProcessor& processor)
    {
        setPlainParam (processor, ParamIDs::width, 140.0f);
        setPlainParam (processor, ParamIDs::bassMonoFreq, 120.0f);
        setPlainParam (processor, ParamIDs::autoMonoSafety, 1.0f);
        setPlainParam (processor, ParamIDs::decorrelateEnabled, 1.0f);
    }

    // Renders the deterministic stimulus through a processor - see
    // TestHelpers::MigrationProtocol (shared with MultibandWidthTests.cpp).
    std::vector<float> renderMigrationStimulus (FirmamentAudioProcessor& processor)
    {
        return TestHelpers::MigrationProtocol::render (processor);
    }

    float peakAbsoluteDifference (const std::vector<float>& a, const std::vector<float>& b)
    {
        REQUIRE (a.size() == b.size());

        float peak = 0.0f;

        for (size_t i = 0; i < a.size(); ++i)
            peak = std::max (peak, std::abs (a[i] - b[i]));

        return peak;
    }
}

TEST_CASE ("v0.2.0 state migration (a): fixture-loaded state nulls BIT-EXACTLY against the same binary with programmatically set v0.2.0-equivalent parameters", "[state][migration][v0.3.0]")
{
    const auto stateFile = fixturesDirectory().getChildFile ("v020-state.bin");
    REQUIRE (stateFile.existsAsFile());

    juce::MemoryBlock stateBytes;
    REQUIRE (stateFile.loadFileAsData (stateBytes));

    // Loading the fixture must succeed, report schema version 1 (no
    // stateVersion attribute existed before v0.3.0)...
    FirmamentAudioProcessor fixtureProcessor;
    CHECK_NOTHROW (fixtureProcessor.setStateInformation (stateBytes.getData(), static_cast<int> (stateBytes.getSize())));
    CHECK (fixtureProcessor.getLoadedStateVersion() == 1);

    // ...and leave every v0.3.0-only parameter at its neutral default.
    for (const auto* id : { ParamIDs::decorrelateMode, ParamIDs::bassMonoMode, ParamIDs::highSplitFreq,
                            ParamIDs::highWidth, ParamIDs::safetyMode, ParamIDs::widthComp, ParamIDs::monoAudition })
    {
        auto* param = fixtureProcessor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        CHECK (param->getValue() == Catch::Approx (param->getDefaultValue()).margin (1.0e-6));
    }

    const auto fixtureRender = renderMigrationStimulus (fixtureProcessor);

    FirmamentAudioProcessor programmaticProcessor;
    applyFixtureEquivalentParameters (programmaticProcessor);
    const auto programmaticRender = renderMigrationStimulus (programmaticProcessor);

    // Tolerance 0: this same-binary null proves the neutral-default
    // migration bit-exactly and is platform-independent (brief 6.1a).
    const auto peakDifference = peakAbsoluteDifference (fixtureRender, programmaticRender);
    CHECK (peakDifference <= 0.0f);
}

TEST_CASE ("v0.2.0 state migration (b): fixture-loaded v0.3.0 render nulls against the frozen v0.2.0 reference render", "[state][migration][v0.3.0]")
{
    const auto stateFile = fixturesDirectory().getChildFile ("v020-state.bin");
    const auto referenceFile = fixturesDirectory().getChildFile ("v020-reference-render.f32");
    REQUIRE (stateFile.existsAsFile());
    REQUIRE (referenceFile.existsAsFile());

    juce::MemoryBlock stateBytes;
    REQUIRE (stateFile.loadFileAsData (stateBytes));

    juce::MemoryBlock referenceBytes;
    REQUIRE (referenceFile.loadFileAsData (referenceBytes));

    const auto expectedSamples = TestHelpers::MigrationProtocol::totalSamples * 2;
    REQUIRE (referenceBytes.getSize() == expectedSamples * sizeof (float));

    std::vector<float> reference (expectedSamples);
    std::memcpy (reference.data(), referenceBytes.getData(), referenceBytes.getSize());

    FirmamentAudioProcessor processor;
    processor.setStateInformation (stateBytes.getData(), static_cast<int> (stateBytes.getSize()));
    const auto render = renderMigrationStimulus (processor);

    const auto peakResidual = peakAbsoluteDifference (render, reference);
    CAPTURE (peakResidual);

    // Cross-version null against a frozen reference file cannot be
    // tolerance 0 on every CI platform simultaneously: std::tan (LR4
    // coefficients), std::exp (correlation estimator) and std::pow
    // (multiplicative frequency smoother) differ by ULPs across
    // platforms/compilers and are amplified through IIR feedback (brief
    // 6.1b). See TestHelpers::MigrationProtocol::crossVersionTolerance():
    // -140 dBFS on the reference architecture (macOS on Apple Silicon), the
    // measured x86 floor of -108 dBFS everywhere else - gated on the
    // executing architecture, not the OS (issue #36: the x86_64 slice under
    // Rosetta drifts exactly like the Windows leg), and never looser (move
    // to per-platform reference files if even that proves flaky). Measured
    // on the reference architecture at the time of writing: peak residual
    // exactly 0 (bit-exact).
    CHECK (peakResidual <= TestHelpers::MigrationProtocol::crossVersionTolerance());
}

TEST_CASE ("v0.2.0 state migration: re-saving a version-1 state stamps it as stateVersion 2", "[state][migration][v0.3.0]")
{
    const auto stateFile = fixturesDirectory().getChildFile ("v020-state.bin");
    REQUIRE (stateFile.existsAsFile());

    juce::MemoryBlock stateBytes;
    REQUIRE (stateFile.loadFileAsData (stateBytes));

    FirmamentAudioProcessor processor;
    processor.setStateInformation (stateBytes.getData(), static_cast<int> (stateBytes.getSize()));
    REQUIRE (processor.getLoadedStateVersion() == 1);

    // Re-save and reload into a second instance, as a host would.
    juce::MemoryBlock resaved;
    processor.getStateInformation (resaved);

    FirmamentAudioProcessor reloaded;
    reloaded.setStateInformation (resaved.getData(), static_cast<int> (resaved.getSize()));
    CHECK (reloaded.getLoadedStateVersion() == FirmamentAudioProcessor::currentStateVersion);
    CHECK (reloaded.getLoadedStateVersion() == 2);

    // The re-saved state still carries the fixture's v0.2.0 values.
    auto* widthParam = reloaded.apvts.getParameter (ParamIDs::width);
    REQUIRE (widthParam != nullptr);
    CHECK (widthParam->convertFrom0to1 (widthParam->getValue()) == Catch::Approx (140.0f).margin (1.0e-3));
}
