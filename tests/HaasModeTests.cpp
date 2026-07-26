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

// ===========================================================================
// v0.3.0 Haas & toggle polish (binding brief, section 3.7).

TEST_CASE ("Haas polish: toggling haasEnabled mid-stream crossfades instead of stepping the Right channel", "[dsp][engine][haas][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setHaasEnabled (false);
    engine.setHaasTimeMs (25.0f);
    engine.setOutputDb (0.0f);
    engine.prepare (makeTestSpec());

    // Steady mono program (Side == 0): with a 25 ms delay engaged, dry and
    // delayed Right differ substantially, so a v0.2.0-style instant gate
    // would step the output by up to the full signal scale in one sample.
    juce::AudioBuffer<float> buffer (2, blockSize);

    float maxStep = 0.0f;
    float previousRight = 0.0f;
    bool first = true;
    float maxInputStep = 0.0f;

    for (int block = 0; block < 12; ++block)
    {
        if (block == 4)
            engine.setHaasEnabled (true); // toggle mid-stream
        if (block == 8)
            engine.setHaasEnabled (false); // and back

        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);
        float previousInput = 0.0f;

        for (int i = 0; i < blockSize; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 620.0
                                * static_cast<double> (block * blockSize + i) / testSampleRate;
            const auto value = 0.5f * static_cast<float> (std::sin (phase));
            left[i] = value;
            right[i] = value;

            if (i > 0)
                maxInputStep = std::max (maxInputStep, std::abs (value - previousInput));
            previousInput = value;
        }

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = buffer.getSample (1, i);

            if (! first)
                maxStep = std::max (maxStep, std::abs (sample - previousRight));

            previousRight = sample;
            first = false;
        }
    }

    // The output's sample-to-sample steps must stay on the scale of the
    // program's own slew (620 Hz sine at 0.5 -> ~0.04/sample), plus the
    // crossfade's gentle slope - far below the ~1.0 step an instant gate on
    // an anti-phase-delayed copy could produce.
    CAPTURE (maxStep, maxInputStep);
    CHECK (maxStep < maxInputStep * 2.0f);
}

TEST_CASE ("Haas polish: automating Haas Time is applied per sample (no per-block zipper steps)", "[dsp][engine][haas][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setHaasEnabled (true);
    engine.setHaasTimeMs (10.0f);
    engine.setOutputDb (0.0f);
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> buffer (2, blockSize);

    // Settle at 10 ms first.
    for (int block = 0; block < 4; ++block)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, static_cast<juce::int64> (block) * blockSize);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    // Sweep the delay time hard while feeding a steady sine; the per-sample
    // smoothed delay through Lagrange interpolation must keep the output
    // slew bounded (a once-per-block setDelay() step of ~0.6 ms jumps the
    // read position by ~30 samples at once - an audible zipper click).
    float maxStep = 0.0f;
    float previousRight = 0.0f;
    bool first = true;

    for (int block = 0; block < 12; ++block)
    {
        engine.setHaasTimeMs (10.0f + 2.0f * static_cast<float> (block)); // 10 -> 32 ms sweep

        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, static_cast<juce::int64> (4 + block) * blockSize);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        for (int i = 0; i < blockSize; ++i)
        {
            const auto sample = buffer.getSample (1, i);

            if (! first)
                maxStep = std::max (maxStep, std::abs (sample - previousRight));

            previousRight = sample;
            first = false;
        }

        REQUIRE (TestHelpers::allSamplesFinite (buffer));
    }

    // A 500 Hz sine at 0.5 slews ~0.033/sample; pitch modulation from the
    // sweeping delay raises that somewhat, but nothing near the ~1.0-scale
    // discontinuities of per-block delay jumps.
    CAPTURE (maxStep);
    CHECK (maxStep < 0.15f);
}
