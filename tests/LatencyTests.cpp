#include "PluginProcessor.h"
#include "dsp/FirmamentEngine.h"
#include "params/ParameterIds.h"

#include <catch2/catch_test_macros.hpp>

TEST_CASE ("getLatencySamples() is 0 in every minimum-phase configuration (v0.3.0: latency is dynamic, but the default path stays latency-free)", "[latency]")
{
    FirmamentAudioProcessor processor;

    // Before prepareToPlay, JUCE's default AudioProcessor latency is 0.
    CHECK (processor.getLatencySamples() == 0);

    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() == 0);

    // Cross-check the engine reports the same (documents the contract
    // explicitly rather than relying on AudioProcessor's default). v0.3.0:
    // getLatencySamples() became an instance method (it is nonzero only
    // while the Linear Phase bass-mono mode is commanded - see the Linear
    // Phase cases below), but the default remains exactly 0.
    FirmamentEngine engine;
    CHECK (engine.getLatencySamples() == 0);
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
}

// ===========================================================================
// v0.3.0 Linear Phase latency tests (binding brief, section 6.4) - the
// first-ever nonzero-latency assertions in this repo. Protocol note (brief
// 6.3/6.4): the harness runs no dispatch loop, so the message-thread hop
// (kernel handoff + setLatencySamples) must be invoked explicitly via
// FirmamentAudioProcessor::handleMessageThreadServicing(), the kernel epoch
// polled with a bounded timeout, and a settling preroll discarded before
// measuring.

#include "TestHelpers.h"

#include <cmath>
#include <vector>

namespace
{
    void setChoiceParam (FirmamentAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }

    // N = 4096 * fs / 48000 rounded to even; latency = N / 2.
    int expectedLinearPhaseLatency (double sampleRate)
    {
        const auto n = 2 * static_cast<int> (std::lround (4096.0 * sampleRate / 48000.0 / 2.0));
        return n / 2;
    }

    // Runs the determinism protocol on a processor already switched to
    // Linear Phase: pump + service + process silence until the kernel epoch
    // advances, then a generous preroll.
    void settleProcessorLinearPhase (FirmamentAudioProcessor& processor, int blockSize)
    {
        juce::AudioBuffer<float> silence (2, blockSize);
        juce::MidiBuffer midi;

        const auto processSilence = [&]
        {
            silence.clear();
            processor.processBlock (silence, midi);
        };

        REQUIRE (TestHelpers::waitForKernelEpoch ([&] { return processor.getLinearPhaseKernelEpoch(); },
                                                  1,
                                                  [&] { processor.handleMessageThreadServicing (true); },
                                                  processSilence));

        for (int i = 0; i < 64; ++i)
            processSilence();
    }
}

TEST_CASE ("v0.3.0 latency: Classic and Phase Matched bass-mono modes report exactly 0 samples", "[latency][v0.3.0]")
{
    FirmamentAudioProcessor processor;
    setChoiceParam (processor, ParamIDs::bassMonoFreq, 120.0f);

    setChoiceParam (processor, ParamIDs::bassMonoMode, 0.0f); // Classic
    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() == 0);

    setChoiceParam (processor, ParamIDs::bassMonoMode, 1.0f); // Phase Matched
    processor.prepareToPlay (48000.0, 512);
    CHECK (processor.getLatencySamples() == 0);
}

TEST_CASE ("v0.3.0 latency: Linear Phase reports exactly N/2 at 44.1/48/96/192 kHz, matching the measured impulse peak offset within +/-1 sample", "[latency][linearphase][v0.3.0]")
{
    constexpr double rates[] = { 44100.0, 48000.0, 96000.0, 192000.0 };
    constexpr int blockSize = 512;

    for (const auto rate : rates)
    {
        FirmamentAudioProcessor processor;
        setChoiceParam (processor, ParamIDs::bassMonoFreq, 120.0f);
        setChoiceParam (processor, ParamIDs::bassMonoMode, 2.0f); // Linear Phase

        processor.prepareToPlay (rate, blockSize);

        const auto expected = expectedLinearPhaseLatency (rate);
        CAPTURE (rate, expected);
        CHECK (processor.getLatencySamples() == expected);

        settleProcessorLinearPhase (processor, blockSize);

        // Measure: mono impulse (Side == 0, so the Mid path's N/2 delay is
        // the whole story) - the output peak must sit at the reported
        // latency within +/-1 sample.
        juce::AudioBuffer<float> buffer (2, blockSize);
        juce::MidiBuffer midi;

        std::vector<float> output;
        bool impulseSent = false;

        while (static_cast<int> (output.size()) < expected + 4 * blockSize)
        {
            buffer.clear();

            if (! impulseSent)
            {
                buffer.setSample (0, 0, 1.0f);
                buffer.setSample (1, 0, 1.0f);
                impulseSent = true;
            }

            processor.processBlock (buffer, midi);
            output.insert (output.end(), buffer.getReadPointer (0), buffer.getReadPointer (0) + blockSize);
        }

        int peakIndex = 0;
        float peakValue = 0.0f;

        for (size_t i = 0; i < output.size(); ++i)
        {
            if (std::abs (output[i]) > peakValue)
            {
                peakValue = std::abs (output[i]);
                peakIndex = static_cast<int> (i);
            }
        }

        CAPTURE (rate, peakIndex, peakValue);
        CHECK (std::abs (peakIndex - processor.getLatencySamples()) <= 1);
        CHECK (peakValue > 0.9f);
    }
}

TEST_CASE ("v0.3.0 latency: switching bass-mono mode mid-stream re-reports latency via the message-thread servicing hop", "[latency][linearphase][v0.3.0]")
{
    constexpr int blockSize = 512;

    FirmamentAudioProcessor processor;
    setChoiceParam (processor, ParamIDs::bassMonoFreq, 120.0f);
    processor.prepareToPlay (48000.0, blockSize);
    REQUIRE (processor.getLatencySamples() == 0);

    // Switch to Linear Phase mid-stream: the audio thread never calls
    // setLatencySamples itself (allocation-free contract); the change is
    // picked up on the message-thread servicing hop (the 50 ms timer in
    // production, invoked directly here for determinism).
    setChoiceParam (processor, ParamIDs::bassMonoMode, 2.0f);

    juce::AudioBuffer<float> silence (2, blockSize);
    juce::MidiBuffer midi;
    silence.clear();
    processor.processBlock (silence, midi); // engine picks up the new mode
    CHECK (processor.getLatencySamples() == 0); // not yet reported

    processor.handleMessageThreadServicing();
    CHECK (processor.getLatencySamples() == expectedLinearPhaseLatency (48000.0));

    // ...and back: latency returns to 0 the same way.
    setChoiceParam (processor, ParamIDs::bassMonoMode, 0.0f);
    silence.clear();
    processor.processBlock (silence, midi);
    processor.handleMessageThreadServicing();
    CHECK (processor.getLatencySamples() == 0);
}
