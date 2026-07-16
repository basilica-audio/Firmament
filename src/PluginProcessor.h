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
class FirmamentAudioProcessor final : public juce::AudioProcessor
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

private:
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

    std::atomic<float> correlationMeterValue { 0.0f };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentAudioProcessor)
};
