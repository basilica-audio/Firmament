#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>

// Multiband width (M1): with the bass-mono crossover engaged (BassMonoFreq >
// 0), the Side signal is split into a low and a high band, each scaled by
// its own independent width control - Low Width below the crossover, Width
// above it - before being summed back together. See FirmamentEngine.h's
// class-level comment for the full rationale, including why Low Width's
// default of 0% exactly reproduces the v0.1 "bass mono" behaviour.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int testBlockSize = 2048;

    juce::dsp::ProcessSpec makeTestSpec()
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (testBlockSize);
        spec.numChannels = 2;
        return spec;
    }
}

TEST_CASE ("Multiband width: Low Width > 0% keeps the low band wide while Width independently collapses the high band", "[dsp][engine][multiband]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (0.0f); // high band forced to mono
    engine.setLowWidthPercent (200.0f); // low band pushed maximally wide
    engine.setBassMonoFrequencyHz (300.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    // Low-frequency content (well below the 300 Hz crossover): must stay
    // wide, since Low Width is 200%.
    juce::AudioBuffer<float> lowBuffer (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (lowBuffer, testSampleRate, 80.0, 90.0, 0.4f);

    {
        juce::dsp::AudioBlock<float> block (lowBuffer);
        // Warm-up block to let the crossover's TPT state settle.
        engine.process (block);
    }
    TestHelpers::fillStereoWithDistinctSines (lowBuffer, testSampleRate, 80.0, 90.0, 0.4f);
    {
        juce::dsp::AudioBlock<float> block (lowBuffer);
        engine.process (block);
    }

    const auto* lowLeft = lowBuffer.getReadPointer (0);
    const auto* lowRight = lowBuffer.getReadPointer (1);

    constexpr int measureFrom = testBlockSize / 2;
    float maxLowDifference = 0.0f;

    for (int i = measureFrom; i < testBlockSize; ++i)
        maxLowDifference = std::max (maxLowDifference, std::abs (lowLeft[i] - lowRight[i]));

    // A wide low band must show a clear L != R difference (well above the
    // "forced mono" noise floor used elsewhere in this suite).
    CHECK (maxLowDifference > 0.05f);

    // High-frequency content (well above the crossover): must collapse to
    // mono, since Width is 0%.
    FirmamentEngine highEngine;
    highEngine.setWidthPercent (0.0f);
    highEngine.setLowWidthPercent (200.0f);
    highEngine.setBassMonoFrequencyHz (300.0f);
    highEngine.setOutputDb (0.0f);
    highEngine.prepare (spec);

    juce::AudioBuffer<float> highBuffer (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (highBuffer, testSampleRate, 2000.0, 2300.0, 0.4f);

    {
        juce::dsp::AudioBlock<float> block (highBuffer);
        highEngine.process (block); // warm-up
    }
    TestHelpers::fillStereoWithDistinctSines (highBuffer, testSampleRate, 2000.0, 2300.0, 0.4f);
    {
        juce::dsp::AudioBlock<float> block (highBuffer);
        highEngine.process (block);
    }

    const auto* highLeft = highBuffer.getReadPointer (0);
    const auto* highRight = highBuffer.getReadPointer (1);

    float maxHighDifference = 0.0f;

    for (int i = measureFrom; i < testBlockSize; ++i)
        maxHighDifference = std::max (maxHighDifference, std::abs (highLeft[i] - highRight[i]));

    // -30 dB relative to the 0.4 amplitude input is the same generous bound
    // used by the v0.1 bass-mono test to distinguish "forced mono" from
    // "left wide".
    CHECK (maxHighDifference < 0.4f * 0.0316f);
}

TEST_CASE ("Multiband width: Low Width 0% (default) with bass-mono engaged reproduces the v0.1 forced-mono-below-crossover behaviour", "[dsp][engine][multiband]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (200.0f);
    engine.setLowWidthPercent (0.0f); // default
    engine.setBassMonoFrequencyHz (300.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 80.0, 90.0, 0.5f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block); // warm-up
    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 80.0, 90.0, 0.5f);
    engine.process (block);

    const auto* left = buffer.getReadPointer (0);
    const auto* right = buffer.getReadPointer (1);

    constexpr int measureFrom = testBlockSize / 2;
    float maxDifference = 0.0f;

    for (int i = measureFrom; i < testBlockSize; ++i)
        maxDifference = std::max (maxDifference, std::abs (left[i] - right[i]));

    CHECK (maxDifference < 0.5f * 0.0316f);
}

TEST_CASE ("Multiband width: Width = Low Width = 100% with bass-mono engaged preserves signal magnitude (flat-magnitude allpass sum, not an exact null)", "[dsp][engine][multiband]")
{
    // Per JUCE 8.0.14's own documentation (juce_dsp/processors/
    // juce_LinkwitzRileyFilter.h: "their sum is equivalent to an all-pass
    // filter with a flat magnitude frequency response"), a Linkwitz-Riley
    // crossover's low+high bands sum to an ALLPASS version of the input, not
    // an exact identity/null - confirmed empirically during development:
    // summing the unscaled low/high bands of this filter reproduces the
    // input's RMS level to high precision but NOT its sample-domain values
    // (a real, audible phase shift through the crossover region). This is
    // standard, expected behaviour for any Linkwitz-Riley-crossover-based
    // multiband processor (this is exactly why the v0.1 bass-mono feature
    // only ever *discards* the low band rather than re-summing it - see
    // docs/architecture.md). This test documents that reality: magnitude is
    // preserved even with both bands at unity gain, but the result is
    // intentionally NOT asserted to null against the input.
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setLowWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (300.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);

    // Warm-up blocks so the crossover's TPT state (and the allpass phase
    // response it implies) is fully settled before measuring.
    for (int warmup = 0; warmup < 4; ++warmup)
    {
        TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 1000.0, 1300.0, 0.5f);
        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
    }

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (reference, testSampleRate, 1000.0, 1300.0, 0.5f);
    buffer.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    const auto referenceRms = TestHelpers::rms (reference);
    const auto outputRms = TestHelpers::rms (buffer);

    // Magnitude preserved to within 1%, as the "flat magnitude" half of
    // JUCE's documented guarantee predicts.
    CHECK (outputRms == Catch::Approx (referenceRms).epsilon (0.01));
}

TEST_CASE ("Multiband width: independent Low Width/Width never breaks the mono-sum invariant, even with bass-mono engaged", "[dsp][engine][multiband]")
{
    // Regardless of what happens to Side (single-band Width, multiband
    // Low Width/Width, or the crossover's allpass characteristic discussed
    // above), Mid is never touched, so L + R == 2 * Mid must hold exactly -
    // this is the multiband generalisation of EngineTests.cpp's mono-sum
    // invariant test.
    FirmamentEngine engine;
    engine.setWidthPercent (0.0f);
    engine.setLowWidthPercent (200.0f);
    engine.setBassMonoFrequencyHz (250.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (reference, testSampleRate, 1000.0, 1300.0, 0.4f);

    juce::AudioBuffer<float> referenceMonoSum (1, testBlockSize);
    {
        const auto* left = reference.getReadPointer (0);
        const auto* right = reference.getReadPointer (1);
        auto* sum = referenceMonoSum.getWritePointer (0);

        for (int i = 0; i < testBlockSize; ++i)
            sum[i] = left[i] + right[i];
    }

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    const auto* left = processed.getReadPointer (0);
    const auto* right = processed.getReadPointer (1);
    const auto* expectedSum = referenceMonoSum.getReadPointer (0);

    float maxResidual = 0.0f;

    for (int i = 0; i < testBlockSize; ++i)
        maxResidual = std::max (maxResidual, std::abs ((left[i] + right[i]) - expectedSum[i]));

    CHECK (maxResidual < 1.0e-4f);
}
