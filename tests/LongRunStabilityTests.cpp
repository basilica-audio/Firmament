#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

#include <random>

// Broadened test coverage (M1): a long-run stability sweep with continuous,
// randomised parameter automation across every parameter (including the M1
// additions - Low Width, Auto Mono Safety, Haas Mode/Time), checking that no
// NaN/Inf ever appears over an extended run. Deliberately sized to stay well
// under a minute in a Debug build on CI (a few hundred thousand samples of
// cheap per-sample DSP with no allocation).
namespace
{
    void setParam (FirmamentAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }
}

TEST_CASE ("Long-run stability: continuous randomised automation of every parameter produces no NaN/Inf over ~1000 blocks", "[robustness][longrun]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 256);

    std::mt19937 rng (99);
    std::uniform_real_distribution<float> unit (0.0f, 1.0f);
    std::bernoulli_distribution coinFlip (0.5);

    juce::MidiBuffer midi;

    constexpr int numBlocks = 1000;
    constexpr int blockSize = 256;

    for (int block = 0; block < numBlocks; ++block)
    {
        setParam (processor, ParamIDs::width, unit (rng) * 200.0f);
        setParam (processor, ParamIDs::lowWidth, unit (rng) * 200.0f);
        setParam (processor, ParamIDs::bassMonoFreq, unit (rng) * 600.0f);
        setParam (processor, ParamIDs::autoMonoSafety, coinFlip (rng) ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::haasEnabled, coinFlip (rng) ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::haasTimeMs, unit (rng) * 40.0f);
        setParam (processor, ParamIDs::output, -24.0f + unit (rng) * 48.0f);
        setParam (processor, ParamIDs::autoMonoSafetyFloorDb, -24.0f + unit (rng) * 24.0f);
        setParam (processor, ParamIDs::autoMonoSafetyMultiband, coinFlip (rng) ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::decorrelateEnabled, coinFlip (rng) ? 1.0f : 0.0f);
        setParam (processor, ParamIDs::decorrelateAmount, unit (rng) * 100.0f);

        juce::AudioBuffer<float> buffer (2, blockSize);
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0,
                                                   80.0 + unit (rng) * 8000.0,
                                                   80.0 + unit (rng) * 8000.0,
                                                   0.7f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Long-run stability: sustained worst-case settings (max width/safety/Haas) hold up over ~500 blocks", "[robustness][longrun]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (96000.0, 512);

    setParam (processor, ParamIDs::width, 200.0f);
    setParam (processor, ParamIDs::lowWidth, 200.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 600.0f);
    setParam (processor, ParamIDs::autoMonoSafety, 1.0f);
    setParam (processor, ParamIDs::haasEnabled, 1.0f);
    setParam (processor, ParamIDs::haasTimeMs, 40.0f);
    setParam (processor, ParamIDs::output, 24.0f);
    setParam (processor, ParamIDs::autoMonoSafetyFloorDb, -24.0f);
    setParam (processor, ParamIDs::autoMonoSafetyMultiband, 1.0f);
    setParam (processor, ParamIDs::decorrelateEnabled, 1.0f);
    setParam (processor, ParamIDs::decorrelateAmount, 100.0f);

    juce::MidiBuffer midi;

    constexpr int numBlocks = 500;
    constexpr int blockSize = 512;

    for (int block = 0; block < numBlocks; ++block)
    {
        juce::AudioBuffer<float> buffer (2, blockSize);
        // Alternate between in-phase and out-of-phase content block to
        // block, to keep Auto Mono Safety's correlation estimate actively
        // transitioning rather than settling into a single steady state.
        const auto rightFreq = (block % 2 == 0) ? 60.0 : 61.0 + 30.0 * ((block / 2) % 5);
        TestHelpers::fillStereoWithDistinctSines (buffer, 96000.0, 60.0, rightFreq, 1.0f);

        CHECK_NOTHROW (processor.processBlock (buffer, midi));

        CHECK (TestHelpers::allSamplesFinite (buffer));
        CHECK (TestHelpers::peakAbsolute (buffer) < 200.0f); // sane bound, not just "finite"
    }
}

// =========================================================================
// Fleet audit class 2b (issue #33): the decaying-tail denormal guard.
//
// The fleet ships with JUCE_DSP_ENABLE_SNAP_TO_ZERO=0 and relies wholly on
// the juce::ScopedNoDenormals held across processBlock() for its denormal
// discipline. This test is what proves that reliance: feed a loud burst so
// every recursive state (LR4 crossovers, phase-matched allpasses, the
// Classic decorrelator's IIR cascade, both correlation integrators, the
// Dynamic safety ballistics) is charged, then hold digital silence and
// require that
//   (a) no output sample ever classifies as subnormal - FP_ZERO/FP_NORMAL
//       only. With FTZ/DAZ correctly engaged this is guaranteed by
//       construction; if anyone ever drops the ScopedNoDenormals (or adds a
//       processing path outside its scope), decaying one-pole/biquad tails
//       land in the subnormal range and this trips on every platform;
//   (b) the tail actually reaches exact zero and rests there - a state
//       parked on a small-but-normal rounding fixed point (the failure
//       Miserere#46 measured at ~1e-34 on x86, sustained indefinitely)
//       would fail this even though FTZ is on;
//   (c) silent blocks cost no more CPU than busy blocks - the audible
//       symptom of denormal grinding on Intel is a plugin that gets
//       *slower* exactly when the track goes quiet (Requiem's 6.12
//       pattern). Both loops below have identical bodies, so the ratio
//       isolates DSP cost. The 10x bound is derived from the failure mode,
//       not from noise: Intel's subnormal penalty is a 10-100x per-op
//       microcode assist, while scheduler jitter on a mean over ~840
//       blocks stays within a few tens of percent. A pass needs no timing
//       precision; only a genuine denormal stall can lose an order of
//       magnitude on the mean.
TEST_CASE ("Long-run stability: after a loud burst, a silent tail decays to exact-zero rest with no denormal residue",
           "[robustness][longrun][denormal]")
{
    constexpr double sampleRate = 48000.0;
    constexpr int blockSize = 512;

    FirmamentAudioProcessor processor;

    // Every recursive path engaged at once: 3-band width (two LR4
    // crossovers on Side), Phase Matched bass-mono (AP2 cascade on Mid),
    // Classic decorrelation (4-stage IIR allpass cascade on Right), Dynamic
    // safety (30 ms correlation estimator + asymmetric ballistics one-pole)
    // and equal-power width compensation. Haas stays off (mutually
    // exclusive with Decorrelate anyway); Linear Phase is skipped because
    // its FIR/delay states are structurally self-flushing.
    setParam (processor, ParamIDs::width, 180.0f);
    setParam (processor, ParamIDs::lowWidth, 150.0f);
    setParam (processor, ParamIDs::highWidth, 150.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 120.0f);
    setParam (processor, ParamIDs::bassMonoMode, 1.0f); // Phase Matched
    setParam (processor, ParamIDs::highSplitFreq, 2000.0f);
    setParam (processor, ParamIDs::autoMonoSafety, 1.0f);
    setParam (processor, ParamIDs::autoMonoSafetyMultiband, 1.0f);
    setParam (processor, ParamIDs::safetyMode, 1.0f); // Dynamic
    setParam (processor, ParamIDs::autoMonoSafetyFloorDb, -24.0f);
    setParam (processor, ParamIDs::decorrelateEnabled, 1.0f);
    setParam (processor, ParamIDs::decorrelateMode, 0.0f); // Classic (IIR cascade)
    setParam (processor, ParamIDs::decorrelateAmount, 100.0f);
    setParam (processor, ParamIDs::widthComp, 1.0f);

    processor.prepareToPlay (sampleRate, blockSize);

    juce::AudioBuffer<float> buffer (2, blockSize);
    juce::MidiBuffer midi;

    // Two seconds of hot, genuinely stereo programme (out-of-phase LF
    // content so the safety estimators and both crossovers all do real
    // work), timing the second second with the same loop body as the
    // silent loop below.
    constexpr auto blocksPerSecond = static_cast<int> (sampleRate) / blockSize;

    for (int block = 0; block < blocksPerSecond; ++block)
    {
        TestHelpers::fillStereoWithDistinctSines (buffer, sampleRate, 90.0, 200.0, 0.8f,
                                                  static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (buffer, midi);
    }

    const auto busyStart = juce::Time::getHighResolutionTicks();

    for (int block = 0; block < blocksPerSecond; ++block)
    {
        TestHelpers::fillStereoWithDistinctSines (buffer, sampleRate, 90.0, 200.0, 0.8f,
                                                  static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (buffer, midi);
        REQUIRE (TestHelpers::allSamplesFinite (buffer));
    }

    const auto busyTicks = juce::Time::getHighResolutionTicks() - busyStart;

    // Ten seconds of digital silence. The first two seconds legitimately
    // carry the crossover/allpass ring-out, the safety release and the
    // engine's one-second rest-flush dwell (see FirmamentEngine.h); after
    // that, every surviving non-zero sample is a parked state.
    constexpr int silentBlocks = 10 * blocksPerSecond;

    float worstTail = 0.0f;
    int subnormalSamples = 0;

    const auto silentStart = juce::Time::getHighResolutionTicks();

    for (int block = 0; block < silentBlocks; ++block)
    {
        buffer.clear();
        processor.processBlock (buffer, midi);
        REQUIRE (TestHelpers::allSamplesFinite (buffer));

        if (block < 2 * blocksPerSecond)
            continue;

        worstTail = juce::jmax (worstTail, TestHelpers::peakAbsolute (buffer));

        for (int channel = 0; channel < buffer.getNumChannels(); ++channel)
        {
            const auto* data = buffer.getReadPointer (channel);

            for (int sample = 0; sample < buffer.getNumSamples(); ++sample)
            {
                const auto classification = std::fpclassify (data[sample]);

                if (classification != FP_ZERO && classification != FP_NORMAL)
                    ++subnormalSamples;
            }
        }
    }

    const auto silentTicks = juce::Time::getHighResolutionTicks() - silentStart;

    INFO ("worst tail after 2 s of silence = " << worstTail
          << ", subnormal samples = " << subnormalSamples);

    // (a) FTZ discipline covers the whole output path.
    CHECK (subnormalSamples == 0);

    // (b) True rest. Exact zero, not merely small: measured on both arm64
    // (native) and x86_64 (Rosetta 2, SSE mul/add rounding like the
    // Windows leg), every path above rests at exactly zero within one
    // second of silence - the crossovers/allpasses decay into the FTZ
    // flush and the correlation/ballistics states follow. Any relaxation
    // here would re-open the Miserere#46 class of parked-state bugs.
    CHECK (worstTail == 0.0f);

    // (c) Silence must not cost more than programme (see the derivation in
    // the header comment; both measured loops share one body).
    const auto busyPerBlock = static_cast<double> (busyTicks) / blocksPerSecond;
    const auto silentPerBlock = static_cast<double> (silentTicks) / silentBlocks;

    INFO ("silent block cost " << silentPerBlock << " ticks vs busy " << busyPerBlock);
    CHECK (silentPerBlock <= busyPerBlock * 10.0);

    // And the engine wakes up cleanly (a few blocks so the smoothed
    // crossfades and safety ballistics settle back in).
    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillStereoWithDistinctSines (buffer, sampleRate, 90.0, 200.0, 0.8f,
                                                  static_cast<juce::int64> (block) * blockSize);
        processor.processBlock (buffer, midi);
        REQUIRE (TestHelpers::allSamplesFinite (buffer));
    }

    CHECK (TestHelpers::peakAbsolute (buffer) > 1.0e-3f);
}
