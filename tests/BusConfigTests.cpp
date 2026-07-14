#include "PluginProcessor.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>

// Broadened test coverage (M1): direct coverage of isBusesLayoutSupported()
// itself (previously only exercised indirectly via the mono-input-bus
// processing test in RobustnessTests.cpp), plus end-to-end processing
// checks for every bus configuration Firmament is expected to accept.
namespace
{
    juce::AudioProcessor::BusesLayout makeLayout (const juce::AudioChannelSet& in, const juce::AudioChannelSet& out)
    {
        juce::AudioProcessor::BusesLayout layout;
        layout.inputBuses.add (in);
        layout.outputBuses.add (out);
        return layout;
    }
}

TEST_CASE ("isBusesLayoutSupported: accepted configurations", "[processor][buses]")
{
    FirmamentAudioProcessor processor;

    SECTION ("stereo in / stereo out (the default, primary configuration)")
    {
        CHECK (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo())));
    }

    SECTION ("mono in / stereo out (mono source routed into the stereo effect)")
    {
        CHECK (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo())));
    }
}

TEST_CASE ("isBusesLayoutSupported: rejected configurations", "[processor][buses]")
{
    FirmamentAudioProcessor processor;

    SECTION ("mono out is rejected - M/S encoding fundamentally needs a stereo output")
    {
        CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::mono())));
    }

    SECTION ("mono in / mono out is rejected")
    {
        CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::mono())));
    }

    SECTION ("5.1 surround out is rejected")
    {
        CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::create5point1())));
    }

    SECTION ("5.1 surround in with stereo out is rejected (input must be mono or stereo)")
    {
        CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::create5point1(), juce::AudioChannelSet::stereo())));
    }

    SECTION ("disabled/empty output bus is rejected")
    {
        CHECK_FALSE (processor.checkBusesLayoutSupported (makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::disabled())));
    }
}

TEST_CASE ("Stereo in / stereo out: full M1 signal path processes cleanly with every new stage engaged", "[processor][buses]")
{
    FirmamentAudioProcessor processor;

    juce::AudioProcessor::BusesLayout stereoInStereoOut = makeLayout (juce::AudioChannelSet::stereo(), juce::AudioChannelSet::stereo());
    REQUIRE (processor.setBusesLayout (stereoInStereoOut));

    processor.prepareToPlay (48000.0, 512);

    auto setParam = [&] (const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    };

    setParam ("width", 160.0f);
    setParam ("lowWidth", 40.0f);
    setParam ("bassMonoFreq", 180.0f);
    setParam ("autoMonoSafety", 1.0f);
    setParam ("haasEnabled", 1.0f);
    setParam ("haasTimeMs", 12.0f);
    setParam ("output", 3.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        TestHelpers::fillStereoWithDistinctSines (buffer, 48000.0, 400.0, 470.0, 0.6f);
        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}

TEST_CASE ("Mono in / stereo out: full M1 signal path processes cleanly with every new stage engaged", "[processor][buses][mono]")
{
    FirmamentAudioProcessor processor;

    juce::AudioProcessor::BusesLayout monoInStereoOut = makeLayout (juce::AudioChannelSet::mono(), juce::AudioChannelSet::stereo());
    REQUIRE (processor.checkBusesLayoutSupported (monoInStereoOut));
    REQUIRE (processor.setBusesLayout (monoInStereoOut));

    processor.prepareToPlay (48000.0, 512);

    auto setParam = [&] (const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    };

    setParam ("width", 200.0f);
    setParam ("lowWidth", 200.0f);
    setParam ("bassMonoFreq", 100.0f);
    setParam ("autoMonoSafety", 1.0f);
    setParam ("haasEnabled", 1.0f);
    setParam ("haasTimeMs", 30.0f);

    juce::AudioBuffer<float> buffer (2, 512);
    juce::MidiBuffer midi;

    for (int block = 0; block < 8; ++block)
    {
        buffer.clear();
        TestHelpers::fillWithSine (buffer, 48000.0, 800.0, 0.7f, static_cast<juce::int64> (block) * 512);
        buffer.clear (1, 0, 512); // simulate an unpopulated second channel, as a mono-in host would present

        CHECK_NOTHROW (processor.processBlock (buffer, midi));
        CHECK (TestHelpers::allSamplesFinite (buffer));
    }
}
