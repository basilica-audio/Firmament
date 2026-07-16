#include "PluginProcessor.h"
#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Correlation/phase meter (M1): FirmamentEngine::getCorrelationValue() is a
// running, leaky-integrated (200 ms) estimate of the plugin's input L/R
// correlation, in [-1, 1]. It drives Auto Mono Safety internally and is
// exposed for a future GUI meter (M3 scope - see FirmamentAudioProcessor::
// getCorrelationMeterValue()).
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int blockSize = 2048;

    // v0.2.0: bumped from 30 to 45 blocks to keep the same settling margin
    // now that the ballistics time constant moved from 200ms to 300ms (see
    // docs/design-brief.md) - 45 * 2048 / 48000 ~= 1.92 s ~= 6.4 time
    // constants at 300 ms.
    constexpr int settleBlocks = 45;

    juce::dsp::ProcessSpec makeTestSpec()
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        return spec;
    }
}

TEST_CASE ("Correlation estimate approaches +1 for identical (in-phase) L/R", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, static_cast<juce::int64> (block) * blockSize);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (engine.getCorrelationValue() > 0.9f);
}

TEST_CASE ("Correlation estimate approaches -1 for inverted (anti-phase) L/R", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        const auto startSample = static_cast<juce::int64> (block) * blockSize;
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 500.0 * static_cast<double> (startSample + i) / testSampleRate;
            const auto value = 0.5f * static_cast<float> (std::sin (phase));
            left[i] = value;
            right[i] = -value;
        }

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (engine.getCorrelationValue() < -0.9f);
}

TEST_CASE ("Correlation estimate stays finite and near zero for silence", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();

    for (int block = 0; block < 4; ++block)
    {
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    CHECK (std::isfinite (engine.getCorrelationValue()));
    CHECK (std::abs (engine.getCorrelationValue()) < 1.0e-3f);
}

TEST_CASE ("Correlation estimate stays within [-1, 1] and finite across a randomised sweep", "[dsp][engine][correlation]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (150.0f);
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    for (int block = 0; block < 50; ++block)
    {
        const auto leftFreq = 100.0 + 37.0 * block;
        const auto rightFreq = 150.0 + 53.0 * block;
        TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, leftFreq, rightFreq, 0.7f);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        const auto correlation = engine.getCorrelationValue();
        CHECK (std::isfinite (correlation));
        CHECK (correlation >= -1.0f);
        CHECK (correlation <= 1.0f);
    }
}

TEST_CASE ("FirmamentAudioProcessor::getCorrelationMeterValue() reflects the engine after processBlock", "[processor][correlation]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (testSampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    for (int block = 0; block < settleBlocks; ++block)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (buffer, midi);
    }

    CHECK (processor.getCorrelationMeterValue() > 0.9f);
}
