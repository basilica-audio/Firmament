#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Haas Mode (M1): an alternative, non-M/S widening technique - the Right
// channel is delayed by HaasTimeMs relative to Left, after M/S decode. Off
// by default; unlike Width/multiband width/Auto Mono Safety, it does NOT
// preserve the exact mono-sum invariant (that's the whole point of the
// effect - see FirmamentEngine.h's class-level comment), so these tests
// instead verify the delay mechanism itself and that the feature is fully
// transparent while disabled.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int blockSize = 2048;

    juce::dsp::ProcessSpec makeTestSpec()
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        return spec;
    }
}

TEST_CASE ("Haas Mode off (default) is a fully transparent passthrough, even with a nonzero Haas Time set", "[dsp][engine][haas][null]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setHaasEnabled (false);
    engine.setHaasTimeMs (25.0f); // set but must have no effect while disabled
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> reference (2, blockSize);
    TestHelpers::fillStereoWithDistinctSines (reference, testSampleRate, 1000.0, 1300.0, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    constexpr float tolerance = 3.1623e-5f; // < -90 dBFS, same bound as the v0.1 unity round-trip test

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

TEST_CASE ("Haas Mode delays Right by the configured time in samples, Left is unaffected", "[dsp][engine][haas]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setHaasEnabled (true);
    engine.setHaasTimeMs (10.0f); // exactly 480 samples at 48 kHz
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    constexpr int expectedDelaySamples = 480;

    // A mono impulse (L == R, so Side == 0 throughout - Width/bass-mono are
    // irrelevant here, this isolates Haas Mode's post-decode delay alone).
    juce::AudioBuffer<float> buffer (2, blockSize);
    buffer.clear();
    buffer.setSample (0, 0, 1.0f);
    buffer.setSample (1, 0, 1.0f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    const auto* left = buffer.getReadPointer (0);
    const auto* right = buffer.getReadPointer (1);

    // Left is never touched by Haas Mode: the impulse must still be exactly
    // at sample 0.
    CHECK (left[0] == Catch::Approx (1.0f).margin (1e-6f));

    for (int i = 1; i < blockSize; ++i)
        CHECK (std::abs (left[i]) < 1.0e-6f);

    // Right must show (near-)silence until the delayed impulse arrives...
    for (int i = 0; i < expectedDelaySamples; ++i)
        CHECK (std::abs (right[i]) < 1.0e-4f);

    // ...and a peak at (very close to) the expected delay, at close to unity
    // amplitude (integer sample delay via linear interpolation is exact).
    int peakIndex = 0;
    float peakValue = 0.0f;

    for (int i = 0; i < blockSize; ++i)
    {
        if (std::abs (right[i]) > peakValue)
        {
            peakValue = std::abs (right[i]);
            peakIndex = i;
        }
    }

    CHECK (std::abs (peakIndex - expectedDelaySamples) <= 1);
    CHECK (peakValue > 0.9f);
}

TEST_CASE ("Haas Mode: reset() clears delay-line state without leaking stale audio", "[dsp][engine][haas]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setHaasEnabled (true);
    engine.setHaasTimeMs (15.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> loudBuffer (2, blockSize);
    TestHelpers::fillStereoWithDistinctSines (loudBuffer, testSampleRate, 1000.0, 1300.0, 0.9f);

    {
        juce::dsp::AudioBlock<float> block (loudBuffer);
        engine.process (block);
    }

    CHECK_NOTHROW (engine.reset());

    juce::AudioBuffer<float> silentBuffer (2, blockSize);
    silentBuffer.clear();

    {
        juce::dsp::AudioBlock<float> block (silentBuffer);
        CHECK_NOTHROW (engine.process (block));
    }

    CHECK (TestHelpers::allSamplesFinite (silentBuffer));
    CHECK (TestHelpers::peakAbsolute (silentBuffer) < 1.0e-6f);
}
