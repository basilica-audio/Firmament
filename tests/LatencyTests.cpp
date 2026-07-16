#include "PluginProcessor.h"
#include "dsp/FirmamentEngine.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("getLatencySamples() is always 0 - Firmament has no oversampling or FIR stage", "[latency]")
{
    FirmamentAudioProcessor processor;

    // Before prepareToPlay, JUCE's default AudioProcessor latency is 0.
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() == 0);

    // Cross-check the engine reports the same (trivial, but documents the
    // contract explicitly rather than relying on AudioProcessor's default).
    CHECK (FirmamentEngine::getLatencySamples() == 0);
}

TEST_CASE ("Latency stays 0 across repeated prepareToPlay calls and sample-rate changes", "[latency]")
{
    FirmamentAudioProcessor processor;

    processor.prepareToPlay (44100.0, 256);
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (44100.0, 256);
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (96000.0, 512);
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (192000.0, 32);
    CHECK (processor.getLatencySamples() == 0);
}

TEST_CASE ("Latency stays 0 regardless of bass-mono crossover setting", "[latency]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto* bassMonoParam = processor.apvts.getParameter ("bassMonoFreq");
    REQUIRE (bassMonoParam != nullptr);

    bassMonoParam->setValueNotifyingHost (bassMonoParam->convertTo0to1 (250.0f));
    processor.prepareToPlay (48000.0, 512);

    CHECK (processor.getLatencySamples() == 0);
}

TEST_CASE ("Latency stays 0 with Decorrelate engaged - the allpass cascade is a zero-latency IIR structure, not FIR/oversampled", "[latency][v0.2.0]")
{
    // docs/design-brief.md guarantee #10: Decorrelate's allpass stage must
    // be zero-latency, like the existing bass-mono crossover, so it is
    // never reported via getLatencySamples() regardless of amount/enabled
    // state or whether Multiband/Haas Mode are also engaged simultaneously.
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    auto setParam = [&] (const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    };

    setParam (ParamIDs::decorrelateEnabled, 1.0f);
    setParam (ParamIDs::decorrelateAmount, 80.0f);
    setParam (ParamIDs::autoMonoSafetyMultiband, 1.0f);
    setParam (ParamIDs::bassMonoFreq, 300.0f);
    setParam (ParamIDs::haasEnabled, 1.0f); // mutual exclusivity path - Decorrelate should still add no latency

    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() == 0);

    CHECK (FirmamentEngine::getLatencySamples() == 0);
}
