#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_approx.hpp>
#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <memory>
#include <utility>
#include <vector>

// Auto Mono Safety (M1): when enabled, a running correlation estimate of the
// plugin's input is used to attenuate the Side channel when the input is
// heavily out-of-phase. It only ever scales Side, so - like Width/Low Width
// - it can never break the L + R == 2 * Mid mono-sum invariant, regardless
// of how aggressively it reacts.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int blockSize = 2048;

    // Enough blocks for the 300 ms leaky-integrator correlation estimate
    // (v0.2.0 - moved from 200 ms) to settle close to its steady-state value
    // (a handful of time constants). 45 blocks * 2048 samples / 48 kHz =
    // 1.92 s = ~6.4 time constants at 300 ms, comfortably settled.
    constexpr int settleBlocks = 45;

    juce::dsp::ProcessSpec makeTestSpec()
    {
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 2;
        return spec;
    }

    // Fills a perfectly out-of-phase (anti-correlated) stereo buffer: L =
    // +sine, R = -sine.
    void fillAntiPhase (juce::AudioBuffer<float>& buffer, juce::int64 startSample, float amplitude = 0.5f)
    {
        const auto numSamples = buffer.getNumSamples();
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 300.0 * static_cast<double> (startSample + i) / testSampleRate;
            const auto value = amplitude * static_cast<float> (std::sin (phase));
            left[i] = value;
            right[i] = -value;
        }
    }

    // Fills a stereo buffer with a fixed, controlled correlation coefficient
    // (v0.2.0 dead-zone/floor tests): Right is a weighted sum of a component
    // in phase with Left (weight == targetCorrelation) and a component in
    // quadrature with it (weight == sqrt(1 - targetCorrelation^2)) - sine and
    // cosine of the same frequency are orthogonal over an integer number of
    // periods, so this produces a running correlation estimate that
    // converges to (very close to) targetCorrelation once settled. 375 Hz is
    // chosen so 48 kHz / 375 Hz == 128 samples/period divides the 2048-sample
    // blockSize evenly (16 exact periods/block), avoiding partial-period
    // averaging error.
    void fillControlledCorrelation (juce::AudioBuffer<float>& buffer, juce::int64 startSample, float targetCorrelation, float amplitude = 0.5f)
    {
        const auto numSamples = buffer.getNumSamples();
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        const auto orthogonalWeight = std::sqrt (juce::jmax (0.0f, 1.0f - targetCorrelation * targetCorrelation));

        for (int i = 0; i < numSamples; ++i)
        {
            const auto phase = juce::MathConstants<double>::twoPi * 375.0 * static_cast<double> (startSample + i) / testSampleRate;
            const auto sinValue = static_cast<float> (std::sin (phase));
            const auto cosValue = static_cast<float> (std::cos (phase));

            left[i] = amplitude * sinValue;
            right[i] = amplitude * (targetCorrelation * sinValue + orthogonalWeight * cosValue);
        }
    }
}

TEST_CASE ("Auto Mono Safety attenuates Side amplitude for strongly out-of-phase input once engaged", "[dsp][engine][automono]")
{
    FirmamentEngine safetyOff;
    safetyOff.setWidthPercent (200.0f);
    safetyOff.setAutoMonoSafetyEnabled (false);
    safetyOff.setOutputDb (0.0f);
    safetyOff.prepare (makeTestSpec());

    FirmamentEngine safetyOn;
    safetyOn.setWidthPercent (200.0f);
    safetyOn.setAutoMonoSafetyEnabled (true);
    safetyOn.setOutputDb (0.0f);
    safetyOn.prepare (makeTestSpec());

    float lastOffDifference = 0.0f;
    float lastOnDifference = 0.0f;

    for (int block = 0; block < settleBlocks; ++block)
    {
        juce::AudioBuffer<float> offBuffer (2, blockSize);
        fillAntiPhase (offBuffer, static_cast<juce::int64> (block) * blockSize);
        juce::dsp::AudioBlock<float> offBlock (offBuffer);
        safetyOff.process (offBlock);

        juce::AudioBuffer<float> onBuffer (2, blockSize);
        fillAntiPhase (onBuffer, static_cast<juce::int64> (block) * blockSize);
        juce::dsp::AudioBlock<float> onBlock (onBuffer);
        safetyOn.process (onBlock);

        if (block == settleBlocks - 1)
        {
            lastOffDifference = TestHelpers::peakAbsolute (offBuffer);
            lastOnDifference = TestHelpers::peakAbsolute (onBuffer);
        }
    }

    // Once the correlation estimate has settled near -1 (fully anti-phase),
    // Auto Mono Safety must measurably reduce output amplitude relative to
    // the same input with safety off.
    CHECK (lastOnDifference < lastOffDifference);

    // And it should be reined in noticeably, not just by rounding error.
    CHECK (lastOnDifference < lastOffDifference * 0.9f);
}

TEST_CASE ("Auto Mono Safety is a no-op for in-phase input (correlation >= 0 keeps full Side gain)", "[dsp][engine][automono]")
{
    FirmamentEngine safetyOff;
    safetyOff.setWidthPercent (150.0f);
    safetyOff.setAutoMonoSafetyEnabled (false);
    safetyOff.prepare (makeTestSpec());

    FirmamentEngine safetyOn;
    safetyOn.setWidthPercent (150.0f);
    safetyOn.setAutoMonoSafetyEnabled (true);
    safetyOn.prepare (makeTestSpec());

    juce::AudioBuffer<float> offBuffer (2, blockSize);
    TestHelpers::fillStereoWithDistinctSines (offBuffer, testSampleRate, 1000.0, 1300.0, 0.4f);
    // Force perfectly in-phase content (L == R) so correlation is +1
    // throughout, not the genuinely distinct stereo content the helper
    // produces by default.
    {
        auto* left = offBuffer.getWritePointer (0);
        auto* right = offBuffer.getWritePointer (1);
        for (int i = 0; i < blockSize; ++i)
            right[i] = left[i];
    }

    juce::AudioBuffer<float> onBuffer;
    onBuffer.makeCopyOf (offBuffer);

    for (int block = 0; block < settleBlocks; ++block)
    {
        juce::dsp::AudioBlock<float> offBlock (offBuffer);
        safetyOff.process (offBlock);

        juce::dsp::AudioBlock<float> onBlock (onBuffer);
        safetyOn.process (onBlock);
    }

    const auto* offLeft = offBuffer.getReadPointer (0);
    const auto* onLeft = onBuffer.getReadPointer (0);

    float maxResidual = 0.0f;

    for (int i = 0; i < blockSize; ++i)
        maxResidual = std::max (maxResidual, std::abs (offLeft[i] - onLeft[i]));

    CHECK (maxResidual < 1.0e-5f);
}

TEST_CASE ("Auto Mono Safety never breaks the mono-sum invariant (only scales Side, never Mid)", "[dsp][engine][automono]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (200.0f);
    engine.setAutoMonoSafetyEnabled (true);
    engine.prepare (makeTestSpec());

    juce::AudioBuffer<float> reference (2, blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        fillAntiPhase (reference, static_cast<juce::int64> (block) * blockSize, 0.6f);

        juce::AudioBuffer<float> referenceMonoSum (1, blockSize);
        {
            const auto* left = reference.getReadPointer (0);
            const auto* right = reference.getReadPointer (1);
            auto* sum = referenceMonoSum.getWritePointer (0);

            for (int i = 0; i < blockSize; ++i)
                sum[i] = left[i] + right[i];
        }

        juce::AudioBuffer<float> processed;
        processed.makeCopyOf (reference);

        juce::dsp::AudioBlock<float> block2 (processed);
        engine.process (block2);

        const auto* left = processed.getReadPointer (0);
        const auto* right = processed.getReadPointer (1);
        const auto* expectedSum = referenceMonoSum.getReadPointer (0);

        float maxResidual = 0.0f;

        for (int i = 0; i < blockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs ((left[i] + right[i]) - expectedSum[i]));

        CHECK (maxResidual < 1.0e-4f);
    }
}

TEST_CASE ("Auto Mono Safety toggle ramps the Side gain smoothly instead of stepping (engage and disengage)", "[dsp][engine][automono]")
{
    // Regression test for GitHub issue #13: the correlation estimate keeps
    // running while Auto Mono Safety is disabled, so by the time the boolean
    // flips on against strongly out-of-phase material the derived safety
    // gain can already be sitting at its 0.35 floor (~-9 dB). Applying that
    // as an instant per-block gate stepped the Side gain by up to ~9 dB in a
    // single sample. The toggle must instead crossfade between bypassed and
    // engaged.
    //
    // Method: `toggled` flips the feature mid-stream; `reference` stays off
    // permanently. Both process bit-identical anti-phase input (Mid == 0,
    // Side == Left), so toggled/reference is exactly the effective Side gain
    // trajectory, sampled wherever the reference is safely away from a zero
    // crossing. The observed gain must never jump between neighbouring
    // samples faster than a genuine ramp allows.
    FirmamentEngine toggled;
    toggled.setWidthPercent (100.0f);
    toggled.setAutoMonoSafetyEnabled (false);
    toggled.setOutputDb (0.0f);
    toggled.prepare (makeTestSpec());

    FirmamentEngine reference;
    reference.setWidthPercent (100.0f);
    reference.setAutoMonoSafetyEnabled (false);
    reference.setOutputDb (0.0f);
    reference.prepare (makeTestSpec());

    juce::int64 sampleIndex = 0;

    auto processBlockPair = [&] (juce::AudioBuffer<float>& toggledOut, juce::AudioBuffer<float>& referenceOut)
    {
        fillAntiPhase (toggledOut, sampleIndex);
        juce::dsp::AudioBlock<float> toggledBlock (toggledOut);
        toggled.process (toggledBlock);

        fillAntiPhase (referenceOut, sampleIndex);
        juce::dsp::AudioBlock<float> referenceBlock (referenceOut);
        reference.process (referenceBlock);

        sampleIndex += blockSize;
    };

    juce::AudioBuffer<float> toggledBuffer (2, blockSize);
    juce::AudioBuffer<float> referenceBuffer (2, blockSize);

    // Let the (always-running) correlation estimate settle near -1 while the
    // feature is still off - the worst case called out in the issue.
    for (int block = 0; block < settleBlocks; ++block)
        processBlockPair (toggledBuffer, referenceBuffer);

    // Measures the effective gain trajectory over `numBlocks` blocks and
    // returns {max gain slope per sample, mean gain over the final block}.
    // Gain samples are only taken where the reference is well away from a
    // zero crossing; the slope between two neighbouring valid samples is
    // normalised by their index distance so a step cannot hide inside a
    // skipped zero-crossing gap. The last-valid-sample bookkeeping is
    // persistent ACROSS calls, so the very first sample measured after a
    // toggle is compared against the last sample measured before it - the
    // exact boundary an unsmoothed toggle steps across.
    juce::int64 lastValidIndex = -1;
    float lastValidGain = 0.0f;
    juce::int64 trajectoryIndex = 0;

    auto measureGainTrajectory = [&] (int numBlocks)
    {
        float maxSlopePerSample = 0.0f;
        double finalBlockGainSum = 0.0;
        int finalBlockGainCount = 0;

        for (int block = 0; block < numBlocks; ++block)
        {
            processBlockPair (toggledBuffer, referenceBuffer);

            const auto* toggledLeft = toggledBuffer.getReadPointer (0);
            const auto* referenceLeft = referenceBuffer.getReadPointer (0);

            for (int i = 0; i < blockSize; ++i, ++trajectoryIndex)
            {
                if (std::abs (referenceLeft[i]) < 0.1f)
                    continue;

                const auto gain = toggledLeft[i] / referenceLeft[i];

                if (lastValidIndex >= 0)
                {
                    const auto distance = static_cast<float> (trajectoryIndex - lastValidIndex);
                    maxSlopePerSample = std::max (maxSlopePerSample, std::abs (gain - lastValidGain) / distance);
                }

                lastValidIndex = trajectoryIndex;
                lastValidGain = gain;

                if (block == numBlocks - 1)
                {
                    finalBlockGainSum += static_cast<double> (gain);
                    ++finalBlockGainCount;
                }
            }
        }

        REQUIRE (finalBlockGainCount > 0);
        return std::pair<float, float> { maxSlopePerSample, static_cast<float> (finalBlockGainSum / finalBlockGainCount) };
    };

    // Baseline block while still disabled: seeds the trajectory bookkeeping
    // with pre-toggle (unity-gain) samples so the engage step boundary itself
    // is part of the measured trajectory, and pins down that the effective
    // gain really is ~1.0 before the flip.
    const auto [baselineMaxSlope, baselineGain] = measureGainTrajectory (1);
    juce::ignoreUnused (baselineMaxSlope);
    CHECK (baselineGain > 0.99f);
    CHECK (baselineGain < 1.01f);

    // Engage mid-stream: the gain must ramp down to the ~0.35 safety floor
    // (correlation is settled near -1) without ever moving faster than 0.01
    // per sample - an unsmoothed toggle steps by ~0.65 across neighbouring
    // samples, orders of magnitude above this bound.
    toggled.setAutoMonoSafetyEnabled (true);
    const auto [engageMaxSlope, engagedGain] = measureGainTrajectory (10);

    CHECK (engageMaxSlope < 0.01f);
    CHECK (engagedGain < 0.5f); // the safety net must still genuinely engage...
    CHECK (engagedGain > 0.2f); // ...to (near) its documented 0.35 floor, not to full mute.

    // Disengage mid-stream: same smoothness requirement on the way back up,
    // and the gain must return to (near) unity.
    toggled.setAutoMonoSafetyEnabled (false);
    const auto [disengageMaxSlope, disengagedGain] = measureGainTrajectory (10);

    CHECK (disengageMaxSlope < 0.01f);
    CHECK (disengagedGain > 0.95f);
}

//==============================================================================
// v0.2.0 additions below (docs/design-brief.md) - dead-zone, user-controlled
// floor, multiband independence, and a ballistics regression guard.

TEST_CASE ("Auto Mono Safety dead-zone: correlation inside [-0.10, 1.0] leaves Side gain at bypass; only below -0.10 does gain measurably reduce", "[dsp][engine][automono][deadzone]")
{
    auto measureSteadyStateGainRatio = [] (float targetCorrelation) -> float
    {
        FirmamentEngine safetyOff;
        safetyOff.setWidthPercent (150.0f);
        safetyOff.setAutoMonoSafetyEnabled (false);
        safetyOff.setOutputDb (0.0f);
        safetyOff.prepare (makeTestSpec());

        FirmamentEngine safetyOn;
        safetyOn.setWidthPercent (150.0f);
        safetyOn.setAutoMonoSafetyEnabled (true);
        safetyOn.setOutputDb (0.0f);
        safetyOn.prepare (makeTestSpec());

        float offPeak = 0.0f;
        float onPeak = 0.0f;

        for (int block = 0; block < settleBlocks; ++block)
        {
            juce::AudioBuffer<float> offBuffer (2, blockSize);
            fillControlledCorrelation (offBuffer, static_cast<juce::int64> (block) * blockSize, targetCorrelation);
            juce::dsp::AudioBlock<float> offBlock (offBuffer);
            safetyOff.process (offBlock);

            juce::AudioBuffer<float> onBuffer (2, blockSize);
            fillControlledCorrelation (onBuffer, static_cast<juce::int64> (block) * blockSize, targetCorrelation);
            juce::dsp::AudioBlock<float> onBlock (onBuffer);
            safetyOn.process (onBlock);

            if (block == settleBlocks - 1)
            {
                offPeak = TestHelpers::peakAbsolute (offBuffer);
                onPeak = TestHelpers::peakAbsolute (onBuffer);
            }
        }

        return onPeak / offPeak;
    };

    // Inside the dead-zone (including a small negative deviation) - bit-
    // identical to full bypass, modulo the on/off crossfade ramp having
    // already settled after settleBlocks.
    CHECK (measureSteadyStateGainRatio (0.5f) == Catch::Approx (1.0f).margin (0.02f));
    CHECK (measureSteadyStateGainRatio (0.0f) == Catch::Approx (1.0f).margin (0.02f));
    CHECK (measureSteadyStateGainRatio (-0.05f) == Catch::Approx (1.0f).margin (0.02f));

    // Below the dead-zone - measurably reduced.
    CHECK (measureSteadyStateGainRatio (-0.5f) < 0.9f);
    CHECK (measureSteadyStateGainRatio (-0.9f) < 0.6f);
}

TEST_CASE ("Auto Mono Safety floor is user-controlled: sweeping autoMonoSafetyFloorDb at correlation == -1.0 matches the corresponding linear floor", "[dsp][engine][automono][floor]")
{
    // Default (-9.1 dB) must land close to v0.1.1's hardcoded 0.35 linear
    // floor - proving the parameter both works and preserves the old
    // default's behaviour bit-for-bit in intent.
    const float floorsDb[] = { -24.0f, -18.0f, -12.0f, -9.1f, -6.0f, -3.0f, 0.0f };

    for (const auto floorDb : floorsDb)
    {
        CAPTURE (floorDb);

        FirmamentEngine safetyOff;
        safetyOff.setWidthPercent (200.0f);
        safetyOff.setAutoMonoSafetyEnabled (false);
        safetyOff.setOutputDb (0.0f);
        safetyOff.prepare (makeTestSpec());

        FirmamentEngine safetyOn;
        safetyOn.setWidthPercent (200.0f);
        safetyOn.setAutoMonoSafetyEnabled (true);
        safetyOn.setAutoMonoSafetyFloorDb (floorDb);
        safetyOn.setOutputDb (0.0f);
        safetyOn.prepare (makeTestSpec());

        float offPeak = 0.0f;
        float onPeak = 0.0f;

        for (int block = 0; block < settleBlocks; ++block)
        {
            juce::AudioBuffer<float> offBuffer (2, blockSize);
            fillAntiPhase (offBuffer, static_cast<juce::int64> (block) * blockSize);
            juce::dsp::AudioBlock<float> offBlock (offBuffer);
            safetyOff.process (offBlock);

            juce::AudioBuffer<float> onBuffer (2, blockSize);
            fillAntiPhase (onBuffer, static_cast<juce::int64> (block) * blockSize);
            juce::dsp::AudioBlock<float> onBlock (onBuffer);
            safetyOn.process (onBlock);

            if (block == settleBlocks - 1)
            {
                offPeak = TestHelpers::peakAbsolute (offBuffer);
                onPeak = TestHelpers::peakAbsolute (onBuffer);
            }
        }

        const auto expectedGain = juce::Decibels::decibelsToGain (floorDb);
        CHECK ((onPeak / offPeak) == Catch::Approx (expectedGain).margin (0.05f));
    }
}

TEST_CASE ("Multiband Auto Mono Safety: low band (in-phase) stays near full gain while high band (out-of-phase) drops toward the floor; off pulls both bands down together", "[dsp][engine][automono][multiband]")
{
    constexpr float crossoverHz = 300.0f;
    constexpr float lowFreqHz = 80.0f;
    constexpr float highFreqHz = 2000.0f;

    // Low band: highly (but not perfectly) in-phase - carries a small but
    // measurable Side component whose gain can actually be observed (a
    // perfectly in-phase signal has Side == 0 identically, which would make
    // any gain applied to it unobservable). High band: fully out-of-phase.
    // The low band is deliberately given less energy than the high band: a
    // near-unity-correlation low band and an equal-energy fully-anti-phase
    // high band would partially cancel in a single *broadband* correlation
    // estimate (0.9 * E_low - E_high with E_low == E_high is only mildly
    // negative, landing back inside the dead-zone) - not a representative
    // "broadband gets dragged negative" scenario. Biasing energy toward the
    // high band keeps the broadband estimate clearly negative, which is what
    // the broadband-regression-guard assertions below need to be meaningful.
    constexpr float lowAmplitude = 0.2f;
    constexpr float highAmplitude = 0.6f;

    auto fillComposite = [] (juce::AudioBuffer<float>& buffer, juce::int64 startSample)
    {
        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < buffer.getNumSamples(); ++i)
        {
            const auto sampleIndex = startSample + i;
            const auto lowPhase = juce::MathConstants<double>::twoPi * lowFreqHz * static_cast<double> (sampleIndex) / testSampleRate;
            const auto highPhase = juce::MathConstants<double>::twoPi * highFreqHz * static_cast<double> (sampleIndex) / testSampleRate;

            const auto lowLeft = lowAmplitude * static_cast<float> (std::sin (lowPhase));
            const auto lowRight = lowAmplitude * 0.9f * static_cast<float> (std::sin (lowPhase));
            const auto highValue = highAmplitude * static_cast<float> (std::sin (highPhase));

            left[i] = lowLeft + highValue;
            right[i] = lowRight - highValue;
        }
    };

    auto makeConfiguredEngine = [] (bool safetyEnabled, bool multibandEnabled)
    {
        auto engine = std::make_unique<FirmamentEngine>();
        engine->setWidthPercent (100.0f);
        engine->setLowWidthPercent (100.0f);
        engine->setBassMonoFrequencyHz (crossoverHz);
        engine->setAutoMonoSafetyEnabled (safetyEnabled);
        engine->setAutoMonoSafetyMultibandEnabled (multibandEnabled);
        engine->setAutoMonoSafetyFloorDb (-20.0f); // firm floor for an unambiguous measurement
        engine->setOutputDb (0.0f);
        engine->prepare (makeTestSpec());
        return engine;
    };

    auto reference = makeConfiguredEngine (false, false); // safety fully off - the unattenuated baseline
    auto multibandOn = makeConfiguredEngine (true, true);
    auto broadbandOnly = makeConfiguredEngine (true, false); // safety on, Multiband off - regression guard

    juce::AudioBuffer<float> referenceBuffer (2, blockSize);
    juce::AudioBuffer<float> multibandBuffer (2, blockSize);
    juce::AudioBuffer<float> broadbandBuffer (2, blockSize);

    for (int block = 0; block < settleBlocks; ++block)
    {
        const auto startSample = static_cast<juce::int64> (block) * blockSize;

        fillComposite (referenceBuffer, startSample);
        juce::dsp::AudioBlock<float> refBlock (referenceBuffer);
        reference->process (refBlock);

        fillComposite (multibandBuffer, startSample);
        juce::dsp::AudioBlock<float> mbBlock (multibandBuffer);
        multibandOn->process (mbBlock);

        fillComposite (broadbandBuffer, startSample);
        juce::dsp::AudioBlock<float> bbBlock (broadbandBuffer);
        broadbandOnly->process (bbBlock);
    }

    // Band-splits a settled final block's (Left - Right) - exactly 2 * Side,
    // since decode() is L = Mid + Side, R = Mid - Side, so L - R == 2 * Side
    // regardless of Mid - with an external, freshly-prepared
    // LinkwitzRileyFilter at the same crossover frequency, measuring RMS
    // over the tail (letting the *external* splitter's own TPT startup
    // settle first) - the same "measure the settled tail" pattern
    // MultibandWidthTests.cpp already uses. Measuring L - R rather than L
    // alone is essential here: the low band is deliberately mostly in-phase
    // (Mid dominates Side there by design - see fillComposite's comment), so
    // Mid's untouched amplitude would otherwise swamp any change in Side's
    // own gain when measured from L directly.
    auto measureBandRms = [] (const juce::AudioBuffer<float>& buffer) -> std::pair<double, double>
    {
        juce::dsp::LinkwitzRileyFilter<float> splitter;
        juce::dsp::ProcessSpec spec;
        spec.sampleRate = testSampleRate;
        spec.maximumBlockSize = static_cast<juce::uint32> (blockSize);
        spec.numChannels = 1;
        splitter.prepare (spec);
        splitter.setCutoffFrequency (crossoverHz);

        const auto* left = buffer.getReadPointer (0);
        const auto* right = buffer.getReadPointer (1);

        std::vector<float> sideProxy (static_cast<size_t> (buffer.getNumSamples()));

        for (int i = 0; i < buffer.getNumSamples(); ++i)
            sideProxy[static_cast<size_t> (i)] = left[i] - right[i];

        const auto* input = sideProxy.data();

        constexpr int measureFrom = blockSize / 2;
        double lowSumSq = 0.0;
        double highSumSq = 0.0;
        int count = 0;

        for (int i = 0; i < blockSize; ++i)
        {
            float low = 0.0f;
            float high = 0.0f;
            splitter.processSample (0, input[i], low, high);

            if (i >= measureFrom)
            {
                lowSumSq += static_cast<double> (low) * static_cast<double> (low);
                highSumSq += static_cast<double> (high) * static_cast<double> (high);
                ++count;
            }
        }

        return { std::sqrt (lowSumSq / count), std::sqrt (highSumSq / count) };
    };

    const auto [referenceLowRms, referenceHighRms] = measureBandRms (referenceBuffer);
    const auto [multibandLowRms, multibandHighRms] = measureBandRms (multibandBuffer);
    const auto [broadbandLowRms, broadbandHighRms] = measureBandRms (broadbandBuffer);

    // Multiband on: the low band's Side content sits well inside the
    // dead-zone (correlation close to +1) - stays close to the unattenuated
    // reference...
    CHECK (multibandLowRms == Catch::Approx (referenceLowRms).epsilon (0.05));

    // ...while the fully out-of-phase high band is measurably pulled toward
    // the floor, well below the reference.
    CHECK (multibandHighRms < referenceHighRms * 0.7);

    // Regression guard: with Multiband OFF, the single broadband correlation
    // estimate (dragged strongly negative by the anti-phase high band) pulls
    // BOTH bands down together - the low band is no longer close to the
    // unattenuated reference either, proving Multiband actually changes
    // behaviour rather than being a no-op.
    CHECK (broadbandLowRms < referenceLowRms * 0.7);
    CHECK (broadbandHighRms < referenceHighRms * 0.7);
}

TEST_CASE ("Auto Mono Safety ballistics: the correlation estimate's settling time reflects the new 300 ms time constant, not the old 200 ms", "[dsp][engine][automono][ballistics]")
{
    constexpr int ballisticsBlockSize = 256;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = testSampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (ballisticsBlockSize);
    spec.numChannels = 2;

    FirmamentEngine engine;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, ballisticsBlockSize);
    juce::int64 sampleIndex = 0;

    // Settle at fully in-phase (+1) first - ~0.9 s, comfortably many time
    // constants under either the old (200 ms) or new (300 ms) ballistics.
    constexpr int settleBlocksLocal = 169; // 169 * 256 / 48000 ~= 0.901 s

    for (int i = 0; i < settleBlocksLocal; ++i)
    {
        TestHelpers::fillWithSine (buffer, testSampleRate, 500.0, 0.5f, sampleIndex);
        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
        sampleIndex += ballisticsBlockSize;
    }

    REQUIRE (engine.getCorrelationValue() > 0.95f);

    // Step to fully anti-phase and track how many samples it takes for the
    // correlation estimate to cross the one-time-constant point of its
    // exponential approach from +1 toward -1: corr(t) = -1 + 2*exp(-t/tau),
    // so corr(tau) = -1 + 2/e =~ -0.2642 - a direct, model-independent proxy
    // for the ballistics time constant.
    constexpr float oneTauCorrelation = -1.0f + 2.0f * 0.3678794412f;

    int blocksUntilThreshold = -1;

    for (int i = 0; i < 400; ++i) // generous upper bound: 400 * 256 / 48000 ~= 2.13 s
    {
        fillAntiPhase (buffer, sampleIndex);
        juce::dsp::AudioBlock<float> block (buffer);
        engine.process (block);
        sampleIndex += ballisticsBlockSize;

        if (engine.getCorrelationValue() <= oneTauCorrelation)
        {
            blocksUntilThreshold = i;
            break;
        }
    }

    REQUIRE (blocksUntilThreshold >= 0);

    const auto elapsedSeconds = static_cast<double> ((blocksUntilThreshold + 1) * ballisticsBlockSize) / testSampleRate;

    // Must land close to the new 300 ms time constant (block-quantised, so a
    // generous window)...
    CHECK (elapsedSeconds > 0.24);
    CHECK (elapsedSeconds < 0.42);

    // ...and clearly not the old 200 ms value.
    CHECK (elapsedSeconds > 0.2 * 1.25);
}
