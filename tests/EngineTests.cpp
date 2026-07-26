#include "dsp/FirmamentEngine.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <cmath>
#include <vector>

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

TEST_CASE ("Width 100% + BassMono off nulls against the input (unity M/S round-trip)", "[dsp][engine][null]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (100.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    REQUIRE (engine.getLatencySamples() == 0);

    juce::AudioBuffer<float> reference (2, testBlockSize);
    // Genuinely distinct L/R content, not a mono source - a real M/S round-
    // trip test has to prove the whole encode/scale/decode chain is
    // transparent, not just that it happens to be transparent for a signal
    // that was already mono (Side == 0) to begin with.
    TestHelpers::fillStereoWithDistinctSines (reference, testSampleRate, 1000.0, 1300.0, 0.5f);

    juce::AudioBuffer<float> processed;
    processed.makeCopyOf (reference);

    juce::dsp::AudioBlock<float> block (processed);
    engine.process (block);

    // < -90 dBFS residual, in linear amplitude.
    constexpr float tolerance = 3.1623e-5f; // 10^(-90/20)

    for (int channel = 0; channel < reference.getNumChannels(); ++channel)
    {
        const auto* refData = reference.getReadPointer (channel);
        const auto* outData = processed.getReadPointer (channel);

        float maxResidual = 0.0f;

        for (int i = 0; i < testBlockSize; ++i)
            maxResidual = std::max (maxResidual, std::abs (outData[i] - refData[i]));

        CHECK (maxResidual < tolerance);
    }
}

TEST_CASE ("Width 0% collapses to mono (L == R)", "[dsp][engine]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (0.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 1000.0, 1300.0, 0.6f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    const auto* left = buffer.getReadPointer (0);
    const auto* right = buffer.getReadPointer (1);

    float maxDifference = 0.0f;

    for (int i = 0; i < testBlockSize; ++i)
        maxDifference = std::max (maxDifference, std::abs (left[i] - right[i]));

    CHECK (maxDifference < 1.0e-6f);
}

TEST_CASE ("Mono downmix (L + R) is unaffected by Width or bass-mono, at any setting", "[dsp][engine]")
{
    // By construction, decode() always returns L = Mid + Side, R = Mid -
    // Side, so L + R == 2 * Mid regardless of what happens to Side - this is
    // the defining mono-compatibility property of M/S widening: increasing
    // apparent stereo width never changes what a mono listener (or a mono
    // fold-down) hears. This test exercises that invariant end-to-end
    // through the engine, across a spread of Width and bass-mono settings.
    struct Setting
    {
        float widthPercent;
        float bassMonoHz;
    };

    const Setting settings[] = {
        { 0.0f, 0.0f },
        { 100.0f, 0.0f },
        { 150.0f, 0.0f },
        { 200.0f, 0.0f },
        { 200.0f, 150.0f },
    };

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

    for (const auto& setting : settings)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (setting.widthPercent);
        engine.setBassMonoFrequencyHz (setting.bassMonoHz);
        engine.setOutputDb (0.0f);

        const auto spec = makeTestSpec();
        engine.prepare (spec);

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
}

TEST_CASE ("Bass-mono forces the Side channel to (near) zero below the crossover frequency", "[dsp][engine][bassmono]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (200.0f); // maximally wide, to make any residual Side content obvious
    engine.setBassMonoFrequencyHz (300.0f);
    engine.setOutputDb (0.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    // A low-frequency test tone (well below the 300 Hz crossover) with
    // genuine stereo content: without bass-mono this would stay wide, with
    // it engaged the low end must collapse to mono (L == R).
    juce::AudioBuffer<float> buffer (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 80.0, 90.0, 0.5f);

    juce::dsp::AudioBlock<float> block (buffer);
    // One warm-up block to let the crossover's TPT state settle out of its
    // zero-state turn-on transient before measuring.
    engine.process (block);
    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 80.0, 90.0, 0.5f);
    engine.process (block);

    const auto* left = buffer.getReadPointer (0);
    const auto* right = buffer.getReadPointer (1);

    // Measure over the settled tail of the block, in dB relative to the
    // 0.5-amplitude input, well clear of the crossover's own passband edge.
    constexpr int measureFrom = testBlockSize / 2;

    float maxDifference = 0.0f;

    for (int i = measureFrom; i < testBlockSize; ++i)
        maxDifference = std::max (maxDifference, std::abs (left[i] - right[i]));

    // -30 dB relative to the 0.5 amplitude input is a generous bound that
    // still clearly distinguishes "forced mono" from "left wide" (which
    // would show a difference on the order of the input amplitude itself).
    CHECK (maxDifference < 0.5f * 0.0316f);
}

TEST_CASE ("Engine reset() clears crossover/gain state without crashing", "[dsp][engine]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (150.0f);
    engine.setBassMonoFrequencyHz (200.0f);
    engine.setOutputDb (6.0f);

    const auto spec = makeTestSpec();
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, testBlockSize);
    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 1000.0, 1300.0, 0.9f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    CHECK_NOTHROW (engine.reset());
    CHECK (TestHelpers::allSamplesFinite (buffer));

    TestHelpers::fillStereoWithDistinctSines (buffer, testSampleRate, 1000.0, 1300.0, 0.9f);
    CHECK_NOTHROW (engine.process (block));
    CHECK (TestHelpers::allSamplesFinite (buffer));
}

// ===========================================================================
// v0.3.0 additions (binding brief, sections 3.6/6.10).

TEST_CASE ("Mono audition: post-everything fold-down makes L == R, crossfaded in and out", "[dsp][engine][audition][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (160.0f);
    engine.setBassMonoFrequencyHz (0.0f);
    engine.setMonoAuditionEnabled (true);
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 512);

    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 700.0, 1100.0, 0.4f);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);
    }

    // Engaged (and settled via prepare): the output is exactly the mono
    // fold-down - L == R.
    float maxChannelDifference = 0.0f;

    for (int i = 0; i < 512; ++i)
        maxChannelDifference = std::max (maxChannelDifference, std::abs (buffer.getSample (0, i) - buffer.getSample (1, i)));

    CHECK (maxChannelDifference <= 0.0f);

    // Disengaging mid-stream crossfades back to stereo without a step.
    engine.setMonoAuditionEnabled (false);

    float maxStep = 0.0f;
    float previousLeft = 0.0f;
    bool first = true;

    for (int block = 0; block < 8; ++block)
    {
        // Phase-continuous across blocks - the step detector below must
        // only see the crossfade, never a stimulus discontinuity.
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 700.0, 1100.0, 0.4f,
                                                  static_cast<juce::int64> (block) * 512);
        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        for (int i = 0; i < 512; ++i)
        {
            const auto sample = buffer.getSample (0, i);

            if (! first)
                maxStep = std::max (maxStep, std::abs (sample - previousLeft));

            previousLeft = sample;
            first = false;
        }
    }

    CHECK (maxStep < 0.2f);

    // Fully disengaged again: channels differ (the 160% width stereo image
    // is back).
    float finalDifference = 0.0f;

    for (int i = 0; i < 512; ++i)
        finalDifference = std::max (finalDifference, std::abs (buffer.getSample (0, i) - buffer.getSample (1, i)));

    CHECK (finalDifference > 0.1f);
}

TEST_CASE ("Mono-sum invariant holds with the 3-band width engaged (High Width scales Side only)", "[dsp][engine][multiband][highsplit][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setWidthPercent (150.0f);
    engine.setLowWidthPercent (70.0f);
    engine.setHighWidthPercent (190.0f);
    engine.setBassMonoFrequencyHz (120.0f);
    engine.setHighSplitFrequencyHz (3000.0f);
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::DeterministicPinkNoise pinkLeft (2468u), pinkRight (1357u);

    for (int block = 0; block < 24; ++block)
    {
        TestHelpers::fillStereoWithDeterministicPinkNoise (buffer, pinkLeft, pinkRight, 0.35f);

        std::vector<float> monoIn (512);

        for (int i = 0; i < 512; ++i)
            monoIn[static_cast<size_t> (i)] = buffer.getSample (0, i) + buffer.getSample (1, i);

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        for (int i = 0; i < 512; ++i)
        {
            const auto monoOut = buffer.getSample (0, i) + buffer.getSample (1, i);
            CHECK (std::abs (monoOut - monoIn[static_cast<size_t> (i)]) < 1.0e-5f);
        }
    }
}

TEST_CASE ("Automation robustness: hard mode toggles every ~100 ms plus width/high-split sweeps stay click-free and finite", "[dsp][engine][automation][v0.3.0]")
{
    FirmamentEngine engine;
    engine.setBassMonoFrequencyHz (120.0f);
    engine.setLowWidthPercent (60.0f);
    engine.setAutoMonoSafetyEnabled (true);
    engine.setDecorrelateEnabled (true);
    engine.setOutputDb (0.0f);

    juce::dsp::ProcessSpec spec;
    spec.sampleRate = 48000.0;
    spec.maximumBlockSize = 512;
    spec.numChannels = 2;
    engine.prepare (spec);

    juce::AudioBuffer<float> buffer (2, 512);
    TestHelpers::DeterministicPinkNoise pinkLeft (86420u), pinkRight (97531u);

    float maxOutputStep = 0.0f;
    float maxInputStep = 0.0f;
    float previousLeft = 0.0f, previousRight = 0.0f, previousInLeft = 0.0f, previousInRight = 0.0f;
    bool first = true;

    constexpr int numBlocks = 300; // ~3.2 s
    int lpSettleCountdown = 0;

    for (int block = 0; block < numBlocks; ++block)
    {
        // Every ~100 ms (9-10 blocks): cycle every mode selector.
        if (block % 9 == 0)
        {
            engine.setDecorrelateMode ((block / 9) % 3);
            engine.setSafetyMode ((block / 9) % 2);
            engine.setBassMonoMode ((block / 9) % 3);
        }

        // Continuous sweeps: width 0 -> 200%, highSplit 0 -> 8000 Hz (the
        // sentinel boundary is crossed repeatedly).
        const auto sweepPhase = static_cast<float> (block) / static_cast<float> (numBlocks);
        engine.setWidthPercent (200.0f * sweepPhase);
        engine.setHighSplitFrequencyHz (8000.0f * juce::jmax (0.0f, sweepPhase * 2.0f - 1.0f));

        auto* left = buffer.getWritePointer (0);
        auto* right = buffer.getWritePointer (1);

        for (int i = 0; i < 512; ++i)
        {
            left[i] = 0.35f * pinkLeft.nextSample();
            right[i] = 0.35f * pinkRight.nextSample();

            if (! first || i > 0)
            {
                maxInputStep = std::max (maxInputStep, std::abs (left[i] - previousInLeft));
                maxInputStep = std::max (maxInputStep, std::abs (right[i] - previousInRight));
            }

            previousInLeft = left[i];
            previousInRight = right[i];
        }

        juce::dsp::AudioBlock<float> audioBlock (buffer);
        engine.process (audioBlock);

        REQUIRE (TestHelpers::allSamplesFinite (buffer));

        // The Linear Phase mute-crossfade makes output near the transition
        // legitimately discontinuous *in content* but never in amplitude
        // slope; still, exclude one block right after each mode command so
        // the click detector measures steady-state behaviour plus the
        // crossfades, exactly as a listener would perceive them.
        juce::ignoreUnused (lpSettleCountdown);

        for (int i = 0; i < 512; ++i)
        {
            const auto outLeft = buffer.getSample (0, i);
            const auto outRight = buffer.getSample (1, i);

            if (! first)
            {
                maxOutputStep = std::max (maxOutputStep, std::abs (outLeft - previousLeft));
                maxOutputStep = std::max (maxOutputStep, std::abs (outRight - previousRight));
            }

            previousLeft = outLeft;
            previousRight = outRight;
            first = false;
        }
    }

    // Click detector (brief 6.10): no output sample step beyond 6 dB (2x)
    // of the program's own worst-case step - mode switches are crossfaded,
    // sweeps are smoothed, and the Linear Phase transitions are mute-faded,
    // so nothing may exceed the program's own scale by more than the width
    // range's 2x gain.
    CAPTURE (maxOutputStep, maxInputStep);
    CHECK (maxOutputStep <= 2.0f * maxInputStep);
}

TEST_CASE ("CPU sanity: a Linear Phase render costs at most 5x a Classic render (guards pathological convolver configuration)", "[dsp][engine][cpu][v0.3.0]")
{
    constexpr int numBlocks = 256; // ~2.7 s at 512 samples

    const auto renderSeconds = [] (int bassMonoMode)
    {
        FirmamentEngine engine;
        engine.setWidthPercent (120.0f);
        engine.setLowWidthPercent (50.0f);
        engine.setBassMonoFrequencyHz (120.0f);
        engine.setBassMonoMode (bassMonoMode);
        engine.setOutputDb (0.0f);

        juce::dsp::ProcessSpec spec;
        spec.sampleRate = 48000.0;
        spec.maximumBlockSize = 512;
        spec.numChannels = 2;
        engine.prepare (spec);

        TestHelpers::DeterministicPinkNoise pinkLeft (111u), pinkRight (222u);
        juce::AudioBuffer<float> buffer (2, 512);

        // Warmup (also lets the Linear Phase kernel install).
        for (int block = 0; block < 32; ++block)
        {
            TestHelpers::fillStereoWithDeterministicPinkNoise (buffer, pinkLeft, pinkRight, 0.35f);
            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);
        }

        const auto start = juce::Time::getHighResolutionTicks();

        for (int block = 0; block < numBlocks; ++block)
        {
            TestHelpers::fillStereoWithDeterministicPinkNoise (buffer, pinkLeft, pinkRight, 0.35f);
            juce::dsp::AudioBlock<float> audioBlock (buffer);
            engine.process (audioBlock);
        }

        return juce::Time::highResolutionTicksToSeconds (juce::Time::getHighResolutionTicks() - start);
    };

    // Best-of-3 per mode to shake off scheduler noise.
    double classicSeconds = 1.0e9, linearPhaseSeconds = 1.0e9;

    for (int attempt = 0; attempt < 3; ++attempt)
    {
        classicSeconds = std::min (classicSeconds, renderSeconds (static_cast<int> (FirmamentEngine::BassMonoMode::classic)));
        linearPhaseSeconds = std::min (linearPhaseSeconds, renderSeconds (static_cast<int> (FirmamentEngine::BassMonoMode::linearPhase)));
    }

    CAPTURE (classicSeconds, linearPhaseSeconds);
    CHECK (linearPhaseSeconds <= 5.0 * classicSeconds);
}
