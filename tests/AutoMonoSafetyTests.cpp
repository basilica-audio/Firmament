#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <utility>

// Auto Mono Safety (M1): when enabled, a running correlation estimate of the
// plugin's input is used to attenuate the Side channel when the input is
// heavily out-of-phase. It only ever scales Side, so - like Width/Low Width
// - it can never break the L + R == 2 * Mid mono-sum invariant, regardless
// of how aggressively it reacts.
namespace
{
    constexpr double testSampleRate = 48000.0;
    constexpr int blockSize = 2048;

    // Enough blocks for the 200 ms leaky-integrator correlation estimate to
    // settle close to its steady-state value (a handful of time constants).
    constexpr int settleBlocks = 30;

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
