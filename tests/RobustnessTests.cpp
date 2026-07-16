#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <limits>
#include <random>

namespace
{
    void setParam (FirmamentAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }
}

TEST_CASE ("Silence produces silence (and no NaN/Inf)", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::width, 200.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 150.0f);
    setParam (processor, ParamIDs::output, 12.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();

    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
    CHECK (TestHelpers::peakAbsolute (buffer) < 1.0e-6f);
}

TEST_CASE ("Full-scale input at maximum width and output trim produces no NaN/Inf", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::width, 200.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 500.0f);
    setParam (processor, ParamIDs::output, 24.0f);

    juce::MidiBuffer midi;

    // A fresh full-scale block every iteration, as a real host would supply
    // - Firmament is a purely linear signal path (no saturating stage, see
    // GainStagingTests.cpp) with no built-in ceiling, so repeatedly
    // reprocessing the *same* buffer (i.e. feeding a prior block's already
    // Width/Output-amplified output back in as new "full-scale" input) would
    // compound geometrically block over block - correctly, but not
    // representative of anything a host actually does.
    for (int i = 0; i < 8; ++i)
    {
        juce::AudioBuffer<float> buffer (2, 512);
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 1000.0, 1300.0, 1.0f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));

        CHECK (TestHelpers::allSamplesFinite (buffer));
        CHECK (TestHelpers::peakAbsolute (buffer) < 100.0f); // sane bound, not just "finite"
    }
}

TEST_CASE ("NaN/Inf input sweep does not propagate to output", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::width, 150.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 200.0f);
    setParam (processor, ParamIDs::output, 0.0f);

    constexpr int numSamples = 512;
    juce::MidiBuffer midi;

    const float poisonValues[] = {
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::infinity(),
        -std::numeric_limits<float>::infinity(),
    };

    for (const auto poison : poisonValues)
    {
        juce::AudioBuffer<float> buffer (2, numSamples);
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 1000.0, 1300.0, 0.5f);

        // Poison a handful of samples spread across both channels.
        buffer.setSample (0, 0, poison);
        buffer.setSample (1, numSamples / 2, poison);
        buffer.setSample (0, numSamples - 1, poison);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));

        // The engine has no feedback path that could indefinitely retain a
        // poisoned value, but a single corrupted block is allowed to leave a
        // finite-but-wrong output for that block; what must never happen is
        // NaN/Inf surviving into the *next*, clean block.
        juce::AudioBuffer<float> cleanBuffer (2, numSamples);
        TestHelpers::fillStereoWithDistinctSines (cleanBuffer, 48000.0, 1000.0, 1300.0, 0.5f);
        CHECK_NOTHROW (processor.processBlock (cleanBuffer, midi));
        CHECK (TestHelpers::allSamplesFinite (cleanBuffer));
    }
}

TEST_CASE ("Denormal-range input produces no NaN/Inf output", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::width, 200.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 100.0f);

    constexpr int numSamples = 512;
    juce::AudioBuffer<float> buffer (2, numSamples);

    const auto denormalValue = std::numeric_limits<float>::denorm_min() * 4.0f;

    for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
    {
        auto* data = buffer.getWritePointer (channel);

        for (int sample = 0; sample < numSamples; ++sample)
            data[sample] = (sample % 2 == 0) ? denormalValue : -denormalValue;
    }

    juce::MidiBuffer midi;

    for (int i = 0; i < 8; ++i)
        CHECK_NOTHROW (processor.processBlock (buffer, midi));

    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Zero-sample buffer does not crash processBlock", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    juce::AudioBuffer<float> buffer (2, 0);
    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (buffer.getNumSamples() == 0);
}

TEST_CASE ("Extreme parameter values at both range edges produce no NaN/Inf", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (44100.0, 256);

    juce::AudioBuffer<float> buffer (2, 256);
    juce::MidiBuffer midi;

    for (bool useMinimum : { true, false })
    {
        setParam (processor, ParamIDs::width, useMinimum ? 0.0f : 200.0f);
        setParam (processor, ParamIDs::bassMonoFreq, useMinimum ? 0.0f : 600.0f);
        setParam (processor, ParamIDs::lowWidth, useMinimum ? 0.0f : 200.0f);
        setParam (processor, ParamIDs::autoMonoSafety, useMinimum ? 0.0f : 1.0f);
        setParam (processor, ParamIDs::haasEnabled, useMinimum ? 0.0f : 1.0f);
        setParam (processor, ParamIDs::haasTimeMs, useMinimum ? 0.0f : 40.0f);
        setParam (processor, ParamIDs::output, useMinimum ? -24.0f : 24.0f);
        setParam (processor, ParamIDs::autoMonoSafetyFloorDb, useMinimum ? -24.0f : 0.0f);
        setParam (processor, ParamIDs::autoMonoSafetyMultiband, useMinimum ? 0.0f : 1.0f);
        setParam (processor, ParamIDs::decorrelateEnabled, useMinimum ? 0.0f : 1.0f);
        setParam (processor, ParamIDs::decorrelateAmount, useMinimum ? 0.0f : 100.0f);

        TestHelpers::fillStereoWithDistinctSines (buffer, 44100.0, 440.0, 550.0, 0.8f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Rapid parameter automation across many blocks produces no NaN/Inf", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    std::mt19937 rng (1234);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);

    juce::MidiBuffer midi;

    for (int block = 0; block < 100; ++block)
    {
        setParam (processor, ParamIDs::width, unit (rng) * 200.0f);
        setParam (processor, ParamIDs::bassMonoFreq, unit (rng) * 600.0f);
        setParam (processor, ParamIDs::lowWidth, unit (rng) * 200.0f);
        setParam (processor, ParamIDs::autoMonoSafety, unit (rng) > 0.5f ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::haasEnabled, unit (rng) > 0.5f ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::haasTimeMs, unit (rng) * 40.0f);
        setParam (processor, ParamIDs::output, -24.0f + unit (rng) * 48.0f);
        setParam (processor, ParamIDs::autoMonoSafetyFloorDb, -24.0f + unit (rng) * 24.0f);
        setParam (processor, ParamIDs::autoMonoSafetyMultiband, unit (rng) > 0.5f ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::decorrelateEnabled, unit (rng) > 0.5f ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::decorrelateAmount, unit (rng) * 100.0f);

        juce::AudioBuffer<float> buffer (2, 256);
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 200.0 + unit (rng) * 4000.0, 300.0 + unit (rng) * 4000.0, 0.7f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("reset() followed by processBlock does not crash", "[robustness]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::width, 150.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 200.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 1000.0, 1300.0, 0.6f);
    juce::MidiBuffer midi;

    processor.processBlock (buffer, midi);

    CHECK_NOTHROW (processor.reset());

    TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 1000.0, 1300.0, 0.6f);
    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

TEST_CASE ("Mono input bus is duplicated gracefully: no crash, L == R regardless of Width", "[robustness][mono]")
{
    FirmamentAudioProcessor processor;

    juce::AudioProcessor::BusesLayout monoInStereoOut;
    monoInStereoOut.inputBuses.add (juce::AudioChannelSet::mono());
    monoInStereoOut.outputBuses.add (juce::AudioChannelSet::stereo());

    REQUIRE (processor.checkBusesLayoutSupported (monoInStereoOut));
    REQUIRE (processor.setBusesLayout (monoInStereoOut));

    processor.prepareToPlay (48000.0, 512);

    setParam (processor, ParamIDs::width, 200.0f); // maximally wide - must still collapse to mono
    setParam (processor, ParamIDs::bassMonoFreq, 0.0f);
    setParam (processor, ParamIDs::output, 0.0f);

    // Buffer sized for the output bus (2 channels); only channel 0 holds the
    // "real" mono input content, mirroring how JUCE presents a mono-in/
    // stereo-out bus configuration's buffer to processBlock().
    juce::AudioBuffer<float> buffer (2, 512);
    buffer.clear();
    TestHelpers::fillWithSine (buffer, 48000.0, 1000.0, 0.7f);
    buffer.clear (1, 0, 512); // simulate an unpopulated second channel

    juce::MidiBuffer midi;

    CHECK_NOTHROW (processor.processBlock (buffer, midi));
    CHECK (TestHelpers::allSamplesFinite (buffer));

    const auto* left = buffer.getReadPointer (0);
    const auto* right = buffer.getReadPointer (1);

    for (int i = 0; i < 512; ++i)
        CHECK (std::abs (left[i] - right[i]) < 1.0e-6f);
}
