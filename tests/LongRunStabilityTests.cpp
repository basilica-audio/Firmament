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
