#include "dsp/FirmamentEngine.h"
#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <cstring>
#include <vector>

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

// ===========================================================================
// v0.3.0 3-band width (binding brief, sections 3.3/6.12).

TEST_CASE ("3-band flat sum: with all widths at 100% and the high split active, the Side band sum (incl. low-band AP2 compensation) is flat within +/-0.1 dB", "[dsp][engine][multiband][highsplit][v0.3.0]")
{
    constexpr int analysisOrder = 15;
    constexpr int analysisSize = 1 << analysisOrder;

    const auto measureSideFlatness = [] (float bassMonoHz, float highSplitHz)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (100.0f);
        engine.setLowWidthPercent (100.0f);
        engine.setHighWidthPercent (100.0f);
        engine.setBassMonoFrequencyHz (bassMonoHz);
        engine.setHighSplitFrequencyHz (highSplitHz);
        engine.setOutputDb (0.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = 512;
        spec.numChannels = 2;
        engine.prepare (spec);

        // Anti-phase impulse: Mid == 0, so the Left output is exactly the
        // Side band-sum path.
        std::vector<float> response;
        juce::AudioBuffer<float> buffer (2, 512);
        bool sent = false;

        while (static_cast<int> (response.size()) < analysisSize)
        {
            buffer.clear();

            if (! sent)
            {
                buffer.setSample (0, 0, 0.5f);
                buffer.setSample (1, 0, -0.5f);
                sent = true;
            }

            juce::dsp::AudioBlock<float> block (buffer);
            engine.process (block);
            response.insert (response.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 512);
        }

        response.resize (analysisSize);
        const auto spectrum = TestHelpers::magnitudeSpectrum (response, analysisOrder, false);

        const auto binLow = static_cast<int> (std::ceil (20.0 * analysisSize / testSampleRate));
        const auto binHigh = static_cast<int> (std::floor (20000.0 * analysisSize / testSampleRate));

        double maxDeviationDb = 0.0;

        for (int bin = binLow; bin <= binHigh; ++bin)
        {
            // Relative to the input impulse's 0.5 amplitude (unity gain).
            const auto magnitudeDb = 20.0 * std::log10 (static_cast<double> (spectrum[static_cast<size_t> (bin)]) / 0.5 + 1.0e-30);
            maxDeviationDb = std::max (maxDeviationDb, std::abs (magnitudeDb));
        }

        return maxDeviationDb;
    };

    // Both crossovers active (3 bands): the low band's AP2(highSplit)
    // compensation is what keeps this flat - without it the sum would ripple
    // around the second crossover.
    const auto threeBand = measureSideFlatness (120.0f, 2500.0f);
    CAPTURE (threeBand);
    CHECK (threeBand < 0.1);

    // High split alone (bass-mono off, 2 bands at the new crossover).
    const auto highSplitOnly = measureSideFlatness (0.0f, 2500.0f);
    CAPTURE (highSplitOnly);
    CHECK (highSplitOnly < 0.1);
}

TEST_CASE ("High Width narrows/widens only the band above the high split", "[dsp][engine][multiband][highsplit][v0.3.0]")
{
    // High Width 0% with width 100%: content above the split collapses to
    // mono, content between the crossovers keeps its width.
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setLowWidthPercent (100.0f);
    engine.setHighWidthPercent (0.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setHighSplitFrequencyHz (2000.0f);
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = testSampleRate;
    spec.maximumBlockSize = 2048;
    spec.numChannels = 2;
    engine.prepare (spec);

    // Two anti-phase (pure Side) tones: one well below the split, one well
    // above it.
    juce::AudioBuffer<float> buffer (2, 2048);

    auto renderAndMeasureSideTone = [&] (double frequency)
    {
        double sidePower = 0.0;

        for (int block = 0; block < 24; ++block)
        {
            auto* left = buffer.getWritePointer (0);
            auto* right = buffer.getWritePointer (1);

            for (int i = 0; i < 2048; ++i)
            {
                const auto phase = juce::MathConstants<double>::twoPi * frequency
                                    * static_cast<double> (block * 2048 + i) / testSampleRate;
                const auto value = 0.4f * static_cast<float> (std::sin (phase));
                left[i] = value;
                right[i] = -value;
            }

            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);

            if (block >= 20) // settled tail only
            {
                for (int i = 0; i < 2048; ++i)
                {
                    const auto side = 0.5f * (buffer.getSample (0, i) - buffer.getSample (1, i));
                    sidePower += static_cast<double> (side) * side;
                }
            }
        }

        return std::sqrt (sidePower);
    };

    const auto lowToneSide = renderAndMeasureSideTone (300.0);
    engine.reset();
    const auto highToneSide = renderAndMeasureSideTone (8000.0);

    CAPTURE (lowToneSide, highToneSide);

    // The 300 Hz tone (below the split) keeps its Side energy; the 8 kHz
    // tone (above the split, High Width 0%) loses at least 30 dB of it.
    CHECK (lowToneSide > 0.1);
    CHECK (highToneSide < lowToneSide * 0.0316);
}

TEST_CASE ("High split sentinel null: highSplitFreq == 0 renders BIT-IDENTICALLY to the same binary with the v0.3.0 parameters never touched, and nulls against the frozen v0.2.0 reference", "[dsp][engine][multiband][highsplit][state][v0.3.0]")
{
    // Part 1 (engine-level, tolerance 0): explicitly setting the v0.3.0
    // parameters to their neutral values (high split off, High Width
    // anything - it is inert at the sentinel) must be indistinguishable
    // from never touching them.
    const auto render = [] (bool touchNewParameters)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (140.0f);
        engine.setLowWidthPercent (30.0f);
        engine.setBassMonoFrequencyHz (120.0f);
        engine.setAutoMonoSafetyEnabled (true);
        engine.setOutputDb (0.0f);

        if (touchNewParameters)
        {
            engine.setHighSplitFrequencyHz (0.0f); // sentinel
            engine.setHighWidthPercent (137.0f); // inert while the sentinel is 0
            engine.setBassMonoMode (static_cast<int> (FirmamentEngine::BassMonoMode::classic));
            engine.setDecorrelateMode (static_cast<int> (FirmamentEngine::DecorrelateMode::classic));
            engine.setSafetyMode (static_cast<int> (FirmamentEngine::SafetyMode::smooth));
        }

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = 512;
        spec.numChannels = 2;
        engine.prepare (spec);

        TestHelpers::DeterministicPinkNoise pinkLeft (44444u), pinkRight (55555u);
        std::vector<float> output;
        juce::AudioBuffer<float> buffer (2, 512);

        for (int block = 0; block < 200; ++block)
        {
            TestHelpers::fillStereoWithDeterministicPinkNoise (buffer, pinkLeft, pinkRight, 0.35f);
            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);

            output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + 512);
            output.insert (output.end(), buffer.getReadPointer (1), buffer.getReadPointer (1) + 512);
        }

        return output;
    };

    const auto untouched = render (false);
    const auto sentinel = render (true);

    REQUIRE (untouched.size() == sentinel.size());

    float peakDifference = 0.0f;

    for (size_t i = 0; i < untouched.size(); ++i)
        peakDifference = std::max (peakDifference, std::abs (untouched[i] - sentinel[i]));

    CHECK (peakDifference <= 0.0f); // bit-identical (brief 6.12/6.1a)

    // Part 2 (processor-level, cross-version): the same sentinel state must
    // null against the frozen v0.2.0 reference render within the documented
    // platform tolerance (see TestHelpers::MigrationProtocol).
    const auto referenceFile = juce::File (__FILE__).getSiblingFile ("fixtures").getChildFile ("v020-reference-render.f32");
    REQUIRE (referenceFile.existsAsFile());

    juce::MemoryBlock referenceBytes;
    REQUIRE (referenceFile.loadFileAsData (referenceBytes));
    REQUIRE (referenceBytes.getSize() == TestHelpers::MigrationProtocol::totalSamples * 2 * sizeof (float));

    std::vector<float> reference (TestHelpers::MigrationProtocol::totalSamples * 2);
    std::memcpy (reference.data(), referenceBytes.getData(), referenceBytes.getSize());

    FirmamentAudioProcessor processor;

    const auto setParam = [&] (const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    };

    // The frozen fixture settings plus every v0.3.0 parameter explicitly
    // set to its neutral value (High Width deliberately non-default - inert
    // at the sentinel).
    setParam (ParamIDs::width, 140.0f);
    setParam (ParamIDs::bassMonoFreq, 120.0f);
    setParam (ParamIDs::autoMonoSafety, 1.0f);
    setParam (ParamIDs::decorrelateEnabled, 1.0f);
    setParam (ParamIDs::highSplitFreq, 0.0f);
    setParam (ParamIDs::highWidth, 137.0f);
    setParam (ParamIDs::bassMonoMode, 0.0f);
    setParam (ParamIDs::decorrelateMode, 0.0f);
    setParam (ParamIDs::safetyMode, 0.0f);

    const auto processed = TestHelpers::MigrationProtocol::render (processor);

    float peakResidual = 0.0f;

    for (size_t i = 0; i < processed.size(); ++i)
        peakResidual = std::max (peakResidual, std::abs (processed[i] - reference[i]));

    CAPTURE (peakResidual);
    CHECK (peakResidual <= TestHelpers::MigrationProtocol::crossVersionTolerance());
}
