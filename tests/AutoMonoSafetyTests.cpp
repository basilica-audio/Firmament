#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>

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
