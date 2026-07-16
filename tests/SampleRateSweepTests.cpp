#include "PluginProcessor.h"
#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Broadened test coverage (M1, issue "Broaden test coverage"): sample-rate
// sweep from 44.1 kHz through 192 kHz, exercising every DSP stage (Width,
// multiband width via bass-mono, Auto Mono Safety, Haas Mode, output trim)
// at each rate, since sample-rate-dependent coefficient computation (the
// crossover's tan()-based coefficients, the correlation meter's leaky-
// integrator coefficient, Haas Mode's ms-to-samples conversion, the
// clampBelowNyquist() guard) is exactly the kind of thing that silently
// breaks only at unusual rates.
namespace
{
    constexpr double sweepRates[] = { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
    constexpr int blockSize = 512;
    constexpr int numBlocksPerRate = 4;
}

TEST_CASE ("FirmamentEngine stays finite, zero-latency, and correctly bounded across the full 44.1-192 kHz sample-rate sweep", "[dsp][engine][sweep]")
{
    for (const auto rate : sweepRates)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (150.0f);
        engine.setLowWidthPercent (60.0f);
        engine.setBassMonoFrequencyHz (200.0f);
        engine.setAutoMonoSafetyEnabled (true);
        engine.setHaasEnabled (true);
        engine.setHaasTimeMs (15.0f);
        engine.setOutputDb (6.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = rate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        engine.prepare (spec);

        CHECK (FirmamentEngine::getLatencySamples() == 0);

        for (int block = 0; block < numBlocksPerRate; ++block)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            TestHelpers::fillStereoWithDistinctSines (buffer, rate, 200.0, 260.0, 0.6f);

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            CHECK_NOTHROW (engine.process (audioBlock));

            CHECK (TestHelpers::allSamplesFinite (buffer));
            CHECK (TestHelpers::peakAbsolute (buffer) < 100.0f);
        }

        CHECK (std::isfinite (engine.getCorrelationValue()));
    }
}

TEST_CASE ("FirmamentEngine v0.2.0 additions (multiband safety, Decorrelate, extended Bass Mono Freq) stay finite and zero-latency across the sample-rate sweep", "[dsp][engine][sweep][v0.2.0]")
{
    for (const auto rate : sweepRates)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (140.0f);
        engine.setLowWidthPercent (40.0f);
        engine.setBassMonoFrequencyHz (600.0f); // the extended v0.2.0 ceiling
        engine.setAutoMonoSafetyEnabled (true);
        engine.setAutoMonoSafetyFloorDb (-15.0f);
        engine.setAutoMonoSafetyMultibandEnabled (true);
        engine.setDecorrelateEnabled (true);
        engine.setDecorrelateAmountPercent (65.0f);
        engine.setHaasEnabled (true); // mutual exclusivity: Decorrelate must win, Haas delay pinned to 0
        engine.setHaasTimeMs (25.0f);
        engine.setOutputDb (3.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = rate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        engine.prepare (spec);

        CHECK (FirmamentEngine::getLatencySamples() == 0);

        for (int block = 0; block < numBlocksPerRate; ++block)
        {
            juce::AudioBuffer<float> buffer (2, blockSize);
            TestHelpers::fillStereoWithDistinctSines (buffer, rate, 150.0, 3200.0, 0.6f);

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            CHECK_NOTHROW (engine.process (audioBlock));

            CHECK (TestHelpers::allSamplesFinite (buffer));
            CHECK (TestHelpers::peakAbsolute (buffer) < 100.0f);
        }

        CHECK (std::isfinite (engine.getCorrelationValue()));
    }
}

TEST_CASE ("FirmamentAudioProcessor: unity Width/BassMono-off round trip holds across the full sample-rate sweep", "[processor][sweep][null]")
{
    // Extends the v0.1 48 kHz-only unity round-trip test (EngineTests.cpp)
    // to every sample rate in the sweep, run through the full processor
    // (APVTS + prepareToPlay), not just the bare engine.
    for (const auto rate : sweepRates)
    {
        FirmamentAudioProcessor processor;
        processor.prepareToPlay (rate, blockSize);

        CHECK (processor.getLatencySamples() == 0);

        juce::AudioBuffer<float> reference (2, blockSize);
        TestHelpers::fillStereoWithDistinctSines (reference, rate, 1000.0, 1300.0, 0.5f);

        juce::AudioBuffer<float> processed;
        processed.makeCopyOf (reference);

        juce::MidiBuffer midi;
        processor.processBlock (processed, midi);

        constexpr float tolerance = 3.1623e-5f; // < -90 dBFS

        for (int channel = 0; channel < reference.getNumChannels(); ++channel)
        {
            const auto* refData = reference.getReadPointer (channel);
            const auto* outData = processed.getReadPointer (channel);

            float maxResidual = 0.0f;

            for (int i = 0; i < blockSize; ++i)
                maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

            CHECK (maxResidual < tolerance);
        }
    }
}

TEST_CASE ("FirmamentEngine handles a sample-rate change (re-prepare) mid-session without NaN/Inf", "[dsp][engine][sweep]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (175.0f);
    engine.setBassMonoFrequencyHz (250.0f);
    engine.setHaasEnabled (true);
    engine.setHaasTimeMs (20.0f);

    for (const auto rate : sweepRates)
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = rate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;

        CHECK_NOTHROW (engine.prepare (spec));

        juce::AudioBuffer<float> buffer (2, blockSize);
        TestHelpers::fillStereoWithDistinctSines (buffer, rate, 300.0, 340.0, 0.5f);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        CHECK_NOTHROW (engine.process (audioBlock));

        CHECK (TestHelpers::allSamplesFinite (buffer));
        CHECK (engine.getLatencySamples() == 0);
    }
}
