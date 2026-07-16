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

    // Deterministic broadband stereo noise (independent L/R, so the derived
    // Side stream carries real wideband content) - seeded per block index so
    // two engines fed the same block indices see bit-identical input.
    void fillDeterministicStereoNoise (juce::AudioBuffer<float>& buffer, int blockIndex, float amplitude = 0.5f)
    {
        juce::Random random (987654321 + static_cast<juce::int64> (blockIndex));

        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            left[i] = amplitude * (random.nextFloat() * 2.0f - 1.0f);
            right[i] = amplitude * (random.nextFloat() * 2.0f - 1.0f);
        }
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

TEST_CASE ("Bass-mono crossover: re-engaging after a disabled stretch resumes from live filter state (no stale-state transient)", "[dsp][engine][multiband]")
{
    // Regression test for GitHub issue #12: while BassMonoFreq sat at 0
    // (disabled), the crossover's internal TPT state (s1-s4) was simply never
    // touched - frozen at whatever it held when the section was last engaged,
    // not decayed or reset. Re-engaging it (e.g. BassMonoFreq automation
    // sweeping back up through 0 Hz) then resumed filtering from a stale
    // snapshot that no longer matched the live signal, producing an audible
    // transient. The engine must instead keep the crossover's state synced
    // with the live Side signal even while the section is disabled (the same
    // "always process, conditionally use" pattern the Haas delay line already
    // follows), so that after a disabled stretch a re-engaged crossover
    // behaves exactly like one that was engaged the whole time.
    const auto spec = makeTestSpec();

    // `toggled` runs 300 Hz -> 0 Hz (off) -> 300 Hz; `alwaysOn` keeps 300 Hz
    // throughout. Both see bit-identical input, so once bass-mono is
    // re-engaged the two must produce (near-)identical output - any
    // difference is exactly the stale-state transient this test guards
    // against.
    FirmamentEngine toggled;
    toggled.setWidthPercent (100.0f);
    toggled.setLowWidthPercent (0.0f);
    toggled.setBassMonoFrequencyHz (300.0f);
    toggled.setOutputDb (0.0f);
    toggled.prepare (spec);

    FirmamentEngine alwaysOn;
    alwaysOn.setWidthPercent (100.0f);
    alwaysOn.setLowWidthPercent (0.0f);
    alwaysOn.setBassMonoFrequencyHz (300.0f);
    alwaysOn.setOutputDb (0.0f);
    alwaysOn.prepare (spec);

    auto processBlockPair = [&] (int blockIndex, juce::AudioBuffer<float>& toggledOut, juce::AudioBuffer<float>& alwaysOnOut)
    {
        fillDeterministicStereoNoise (toggledOut, blockIndex);
        juce::dsp::AudioBlock<float> toggledBlock (toggledOut);
        toggled.process (toggledBlock);

        fillDeterministicStereoNoise (alwaysOnOut, blockIndex);
        juce::dsp::AudioBlock<float> alwaysOnBlock (alwaysOnOut);
        alwaysOn.process (alwaysOnBlock);
    };

    juce::AudioBuffer<float> toggledBuffer (2, testBlockSize);
    juce::AudioBuffer<float> alwaysOnBuffer (2, testBlockSize);

    int blockIndex = 0;

    // Phase 1: both engaged at 300 Hz, long enough for all smoothers and the
    // crossover state to be fully settled.
    for (int i = 0; i < 10; ++i)
        processBlockPair (blockIndex++, toggledBuffer, alwaysOnBuffer);

    // Phase 2: disable bass-mono on `toggled` only, for roughly half a
    // second of audio - plenty for the live signal to diverge completely
    // from whatever state a frozen filter would have kept.
    toggled.setBassMonoFrequencyHz (0.0f);

    for (int i = 0; i < 12; ++i)
        processBlockPair (blockIndex++, toggledBuffer, alwaysOnBuffer);

    // Phase 3: re-engage. From here on `toggled` must match `alwaysOn`.
    toggled.setBassMonoFrequencyHz (300.0f);

    float maxDifference = 0.0f;

    for (int i = 0; i < 4; ++i)
    {
        processBlockPair (blockIndex++, toggledBuffer, alwaysOnBuffer);

        for (int channel = 0; channel < 2; ++channel)
        {
            const auto* toggledData = toggledBuffer.getReadPointer (channel);
            const auto* alwaysOnData = alwaysOnBuffer.getReadPointer (channel);

            for (int sample = 0; sample < testBlockSize; ++sample)
                maxDifference = std::max (maxDifference, std::abs (toggledData[sample] - alwaysOnData[sample]));
        }
    }

    REQUIRE (TestHelpers::allSamplesFinite (toggledBuffer));

    // With the crossover state kept live while disabled, the two engines are
    // in bit-identical state at the moment of re-engagement, so their output
    // must agree to well below audibility. A frozen-state crossover fails
    // this by orders of magnitude (a genuine transient, not rounding noise).
    CHECK (maxDifference < 1.0e-6f);
}

TEST_CASE ("Bass Mono Freq range extension: forced-mono-below-crossover and magnitude-preservation hold across the extended 0-600 Hz range", "[dsp][engine][multiband][v0.2.0]")
{
    // docs/design-brief.md's lowest-confidence, most-reasoned v0.2.0 change:
    // the range ceiling moved from 500 to 600 Hz. Parametrised across a
    // spread including the old ceiling and the new one, re-running the two
    // core crossover-behaviour guarantees the v0.1/v0.1.1 tests already
    // established at 300 Hz - not just the old 0-500 Hz span.
    const float crossoverFrequenciesHz[] = { 80.0f, 150.0f, 300.0f, 450.0f, 500.0f, 600.0f };

    for (const auto crossoverHz : crossoverFrequenciesHz)
    {
        CAPTURE (crossoverHz);

        // Forced-mono-below-crossover: a test tone comfortably below the
        // crossover (a fixed fraction of it) must collapse toward mono at
        // the default Low Width (0%).
        {
            FirmamentEngine engine;
            engine.setWidthPercent (200.0f);
            engine.setBassMonoFrequencyHz (crossoverHz);
            engine.setOutputDb (0.0f);

            const auto spec = makeTestSpec();
            engine.prepare (spec);

            const auto testFreq = static_cast<double> (crossoverHz) * 0.15; // comfortably below the crossover
            juce::AudioBuffer<float> buffer (2, testBlockSize);
            TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, testFreq, testFreq * 1.1, 0.5f);

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block); // warm-up
            TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, testFreq, testFreq * 1.1, 0.5f);
            engine.process (block);

            const auto* left = buffer.getReadPointer (0);
            const auto* right = buffer.getReadPointer (1);

            constexpr int measureFrom = testBlockSize / 2;
            float maxDifference = 0.0f;

            for (int i = measureFrom; i < testBlockSize; ++i)
                maxDifference = std::max (maxDifference, std::abs (left[i] - right[i]));

            // -24 dB relative to the 0.5 amplitude input - a generous bound
            // that still clearly distinguishes "forced mono" from "left
            // wide" across the whole extended crossover range (a test tone
            // this far below the crossover, at any of these frequencies,
            // sits solidly in the LR4 stopband).
            CHECK (maxDifference < 0.5f * 0.0631f);
        }

        // Magnitude preservation: Width == Low Width == 100% still holds the
        // "flat-magnitude allpass sum" guarantee (not an exact null) at this
        // crossover frequency too.
        {
            FirmamentEngine engine;
            engine.setWidthPercent (100.0f);
            engine.setLowWidthPercent (100.0f);
            engine.setBassMonoFrequencyHz (crossoverHz);
            engine.setOutputDb (0.0f);

            const auto spec = makeTestSpec();
            engine.prepare (spec);

            juce::AudioBuffer<float> buffer (2, testBlockSize);

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

            CHECK (outputRms == Catch::Approx (referenceRms).epsilon (0.02));
        }
    }
}
