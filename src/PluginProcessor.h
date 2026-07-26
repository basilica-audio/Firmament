#pragma once

#include <juce_audio_processors/juce_audio_processors.h>
#include <juce_dsp/juce_dsp.h>

#include "dsp/FirmamentEngine.h"
#include "presets/PresetManager.h"

// Firmament: a stereo widener/imager built around Mid/Side encode/decode.
// Signal flow lives in FirmamentEngine (src/dsp) so it stays unit-testable
// independent of this AudioProcessor; this class is just APVTS + host
// plumbing around it. Firmament fundamentally needs a stereo signal to
// operate on (Mid/Side encoding requires both L and R) - see
// isBusesLayoutSupported() and processBlock() for how a mono input bus is
// handled gracefully rather than rejected outright.
//
// v0.3.0 message-thread servicing: the Linear Phase bass-mono mode makes
// two things message-thread work that previously did not exist in this
// codebase - (a) the FIR kernel recompute + Convolution::loadImpulseResponse
// handoff on cutoff changes (coalesced to at most one recompute per 50 ms;
// see LinearPhaseCrossover.h) and (b) forwarding the engine's now-dynamic
// latency via setLatencySamples. Both are driven by a lightweight 50 ms
// juce::Timer (handleMessageThreadServicing()) rather than by
// AsyncUpdater::triggerAsyncUpdate() from the audio thread: triggerAsyncUpdate
// posts a message (a heap allocation) when none is pending, which would
// violate the zero-allocation audio-thread contract asserted by
// tests/AllocationGuardTests.cpp - and parameter changes can arrive on the
// audio thread under host automation. The timer achieves the same
// "message-thread hop, coalesced" semantics allocation-free on the audio
// side; tests call handleMessageThreadServicing() directly (or pump the
// message loop, which fires the timer) for determinism.
class FirmamentAudioProcessor final : public juce::AudioProcessor,
                                      private juce::Timer
{
public:
    FirmamentAudioProcessor();
    ~FirmamentAudioProcessor() override;

    //==============================================================================
    void prepareToPlay (double sampleRate, int samplesPerBlock) override;
    void releaseResources() override;
    void reset() override;

    bool isBusesLayoutSupported (const BusesLayout& layouts) const override;

    void processBlock (juce::AudioBuffer<float>&, juce::MidiBuffer&) override;

    //==============================================================================
    juce::AudioProcessorEditor* createEditor() override;
    bool hasEditor() const override;

    //==============================================================================
    const juce::String getName() const override;

    bool acceptsMidi() const override;
    bool producesMidi() const override;
    bool isMidiEffect() const override;
    double getTailLengthSeconds() const override;

    //==============================================================================
    int getNumPrograms() override;
    int getCurrentProgram() override;
    void setCurrentProgram (int index) override;
    const juce::String getProgramName (int index) override;
    void changeProgramName (int index, const juce::String& newName) override;

    //==============================================================================
    void getStateInformation (juce::MemoryBlock& destData) override;
    void setStateInformation (const void* data, int sizeInBytes) override;

    static juce::AudioProcessorValueTreeState::ParameterLayout createParameterLayout();

    juce::AudioProcessorValueTreeState apvts;

    // M2 preset system (.scaffold/specs/preset-system-m2.md,
    // src/presets/PresetManager.h). Constructed after apvts (its
    // constructor registers APVTS parameter listeners) and public so
    // FirmamentAudioProcessorEditor's PresetBar can talk to it directly -
    // the same "processor owns it, editor references it" pattern apvts
    // itself already uses.
    basilica::presets::PresetManager presetManager;

    // The most recent block's correlation/phase estimate of the plugin's
    // input (see FirmamentEngine::getCorrelationValue()), refreshed from the
    // engine at the end of every processBlock() call. Safe to read from any
    // thread. Not yet consumed by the GUI - the v0.1 editor is a placeholder
    // (see PluginEditor.*); wiring an actual meter widget to this value is
    // M3 (GUI & accessibility) scope, not M1.
    float getCorrelationMeterValue() const noexcept { return correlationMeterValue.load (std::memory_order_relaxed); }

    // v0.3.0 meter surface (brief, feature 8): per-band input correlation
    // (bands defined by the bass-mono and high-split crossovers) and the
    // broadband *output* (post-processing) correlation, all refreshed once
    // per processBlock() and safe to read from any thread. Consumed by the
    // M3 GUI later; asserted by tests/CorrelationMeterTests.cpp now.
    float getCorrelationMeterLowValue() const noexcept { return correlationMeterLow.load (std::memory_order_relaxed); }
    float getCorrelationMeterHighValue() const noexcept { return correlationMeterHigh.load (std::memory_order_relaxed); }
    float getCorrelationMeterMidBandValue() const noexcept { return correlationMeterMidBand.load (std::memory_order_relaxed); }
    float getCorrelationMeterHighBandValue() const noexcept { return correlationMeterHighBand.load (std::memory_order_relaxed); }
    float getOutputCorrelationMeterValue() const noexcept { return outputCorrelationMeter.load (std::memory_order_relaxed); }

    // v0.3.0 message-thread service entry point (see the class comment):
    // forwards Linear Phase kernel recomputes to the engine and reports
    // latency changes via setLatencySamples. Normally driven by the internal
    // 50 ms timer; public so tests can invoke the exact same hop
    // deterministically. Message thread only.
    void handleMessageThreadServicing (bool forceKernelReload = false);

    // Deterministic Linear Phase kernel ready-signal, for tests (see
    // LinearPhaseCrossover::kernelEpoch()).
    juce::uint64 getLinearPhaseKernelEpoch() const noexcept { return engine.getLinearPhaseKernelEpoch(); }

    // The state-schema version found in the most recently loaded state (1
    // for v0.1.x/v0.2.0 states without a stateVersion attribute, 2 for
    // v0.3.0+). Fresh instances report the current version.
    int getLoadedStateVersion() const noexcept { return loadedStateVersion; }

    static constexpr int currentStateVersion = 2;

private:
    void timerCallback() override { handleMessageThreadServicing(); }

    FirmamentEngine engine;

    // Raw atomic pointers into the APVTS-managed parameter values, resolved
    // once at construction time so processBlock() never has to search for
    // them (no allocation/locks on the audio thread).
    std::atomic<float>* widthPercent = nullptr;
    std::atomic<float>* lowWidthPercent = nullptr;
    std::atomic<float>* bassMonoFreqHz = nullptr;
    std::atomic<float>* autoMonoSafetyEnabled = nullptr;
    std::atomic<float>* haasEnabled = nullptr;
    std::atomic<float>* haasTimeMs = nullptr;
    std::atomic<float>* outputDb = nullptr;

    // v0.2.0 additions - see ParameterIds.h.
    std::atomic<float>* autoMonoSafetyFloorDb = nullptr;
    std::atomic<float>* autoMonoSafetyMultiband = nullptr;
    std::atomic<float>* decorrelateEnabled = nullptr;
    std::atomic<float>* decorrelateAmount = nullptr;

    // v0.3.0 additions - see ParameterIds.h.
    std::atomic<float>* decorrelateMode = nullptr;
    std::atomic<float>* bassMonoMode = nullptr;
    std::atomic<float>* highSplitFreq = nullptr;
    std::atomic<float>* highWidth = nullptr;
    std::atomic<float>* safetyMode = nullptr;
    std::atomic<float>* widthComp = nullptr;
    std::atomic<float>* monoAudition = nullptr;

    std::atomic<float> correlationMeterValue { 0.0f };
    std::atomic<float> correlationMeterLow { 0.0f };
    std::atomic<float> correlationMeterHigh { 0.0f };
    std::atomic<float> correlationMeterMidBand { 0.0f };
    std::atomic<float> correlationMeterHighBand { 0.0f };
    std::atomic<float> outputCorrelationMeter { 0.0f };

    int loadedStateVersion = currentStateVersion;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentAudioProcessor)
};
