#include "PluginProcessor.h"
#include "PluginEditor.h"
#include "params/ParameterIds.h"
#include "params/ParameterLayout.h"

#include <BinaryData.h>

namespace
{
    // The small, Firmament-specific config surface PresetManager needs (see
    // src/presets/PresetManager.h's class docs) - everything else about the
    // preset system is fully generic and portable across the suite (see
    // basilica-audio/nave's docs/preset-system-notes.md, the pilot
    // implementation this was copied from).
    basilica::presets::PresetManagerConfig makePresetManagerConfig()
    {
        // JucePlugin_CFBundleIdentifier expands to a raw (unquoted) token
        // sequence, not a string literal - JUCE_STRINGIFY() is the
        // documented way to turn it into one. This is always
        // "com.yvesvogl.firmament" here (BUNDLE_ID in CMakeLists.txt),
        // matching the "plugin" field baked into every presets/factory/*.json
        // file.
        basilica::presets::PresetManagerConfig config;
        config.pluginId = JUCE_STRINGIFY (JucePlugin_CFBundleIdentifier);
        config.pluginName = JucePlugin_Name;
        config.manufacturerName = "Yves Vogl";
        config.pluginVersion = JucePlugin_VersionString;
        // userPresetsDirectoryOverrideForTests intentionally left
        // default-constructed (empty) - production instances always use the
        // real platform-standard preset location (see PresetManager.h).
        return config;
    }

    // BinaryData symbol names are derived from the presets/factory/*.json
    // file names passed to juce_add_binary_data() in CMakeLists.txt (dots
    // become underscores) - this list must stay in sync with that SOURCES
    // list. Order here only affects factory-preset iteration order before
    // getAllPresets() re-sorts alphabetically, so it isn't otherwise
    // significant.
    std::vector<basilica::presets::FactoryPresetAsset> makeFactoryPresetAssets()
    {
        return {
            { BinaryData::default_json, BinaryData::default_jsonSize },
            { BinaryData::openStrings_json, BinaryData::openStrings_jsonSize },
            { BinaryData::choirBloom_json, BinaryData::choirBloom_jsonSize },
            { BinaryData::doubledRhythmGlue_json, BinaryData::doubledRhythmGlue_jsonSize },
            { BinaryData::masterBusBassMono_json, BinaryData::masterBusBassMono_jsonSize },
            { BinaryData::automatedWidthSafetyNet_json, BinaryData::automatedWidthSafetyNet_jsonSize },
            { BinaryData::monoSafeAir_json, BinaryData::monoSafeAir_jsonSize },
            { BinaryData::widePadFullPrecedence_json, BinaryData::widePadFullPrecedence_jsonSize },
            { BinaryData::extremeWidth_json, BinaryData::extremeWidth_jsonSize },
            { BinaryData::subtleOpenness_json, BinaryData::subtleOpenness_jsonSize },
        };
    }
}

//==============================================================================
FirmamentAudioProcessor::FirmamentAudioProcessor()
    : AudioProcessor (BusesProperties()
                          .withInput ("Input", juce::AudioChannelSet::stereo(), true)
                          .withOutput ("Output", juce::AudioChannelSet::stereo(), true)),
      apvts (*this, nullptr, "PARAMETERS", createParameterLayout()),
      presetManager (apvts, makePresetManagerConfig(), makeFactoryPresetAssets())
{
    widthPercent = apvts.getRawParameterValue (ParamIDs::width);
    lowWidthPercent = apvts.getRawParameterValue (ParamIDs::lowWidth);
    bassMonoFreqHz = apvts.getRawParameterValue (ParamIDs::bassMonoFreq);
    autoMonoSafetyEnabled = apvts.getRawParameterValue (ParamIDs::autoMonoSafety);
    haasEnabled = apvts.getRawParameterValue (ParamIDs::haasEnabled);
    haasTimeMs = apvts.getRawParameterValue (ParamIDs::haasTimeMs);
    outputDb = apvts.getRawParameterValue (ParamIDs::output);
    autoMonoSafetyFloorDb = apvts.getRawParameterValue (ParamIDs::autoMonoSafetyFloorDb);
    autoMonoSafetyMultiband = apvts.getRawParameterValue (ParamIDs::autoMonoSafetyMultiband);
    decorrelateEnabled = apvts.getRawParameterValue (ParamIDs::decorrelateEnabled);
    decorrelateAmount = apvts.getRawParameterValue (ParamIDs::decorrelateAmount);

    jassert (widthPercent != nullptr);
    jassert (lowWidthPercent != nullptr);
    jassert (bassMonoFreqHz != nullptr);
    jassert (autoMonoSafetyEnabled != nullptr);
    jassert (haasEnabled != nullptr);
    jassert (haasTimeMs != nullptr);
    jassert (outputDb != nullptr);
    jassert (autoMonoSafetyFloorDb != nullptr);
    jassert (autoMonoSafetyMultiband != nullptr);
    jassert (decorrelateEnabled != nullptr);
    jassert (decorrelateAmount != nullptr);

    // M2 default resolution: user "Default" preset > factory "Default"
    // preset > the ParameterLayout defaults apvts was just constructed
    // with above (see PresetManager::applyStartupDefault()'s docs).
    presetManager.applyStartupDefault();
}

FirmamentAudioProcessor::~FirmamentAudioProcessor() = default;

//==============================================================================
juce::AudioProcessorValueTreeState::ParameterLayout FirmamentAudioProcessor::createParameterLayout()
{
    return frmm::createParameterLayout();
}

//==============================================================================
const juce::String FirmamentAudioProcessor::getName() const
{
    return JucePlugin_Name;
}

bool FirmamentAudioProcessor::acceptsMidi() const
{
    return false;
}

bool FirmamentAudioProcessor::producesMidi() const
{
    return false;
}

bool FirmamentAudioProcessor::isMidiEffect() const
{
    return false;
}

double FirmamentAudioProcessor::getTailLengthSeconds() const
{
    return 0.0;
}

int FirmamentAudioProcessor::getNumPrograms()
{
    return 1;
}

int FirmamentAudioProcessor::getCurrentProgram()
{
    return 0;
}

void FirmamentAudioProcessor::setCurrentProgram (int)
{
}

const juce::String FirmamentAudioProcessor::getProgramName (int)
{
    return {};
}

void FirmamentAudioProcessor::changeProgramName (int, const juce::String&)
{
}

//==============================================================================
void FirmamentAudioProcessor::prepareToPlay (double sampleRate, int samplesPerBlock)
{
    juce::dsp::ProcessSpec spec;
    spec.sampleRate = sampleRate;
    spec.maximumBlockSize = static_cast<juce::uint32> (samplesPerBlock);
    spec.numChannels = static_cast<juce::uint32> (getTotalNumOutputChannels());

    // Seed the engine's parameters from the current APVTS state before
    // prepare() primes the crossover coefficients, so the very first block
    // after prepareToPlay() already reflects the host/session's actual
    // parameter values rather than the engine's built-in defaults.
    engine.setWidthPercent (widthPercent->load (std::memory_order_relaxed));
    engine.setLowWidthPercent (lowWidthPercent->load (std::memory_order_relaxed));
    engine.setBassMonoFrequencyHz (bassMonoFreqHz->load (std::memory_order_relaxed));
    engine.setAutoMonoSafetyEnabled (autoMonoSafetyEnabled->load (std::memory_order_relaxed) > 0.5f);
    engine.setHaasEnabled (haasEnabled->load (std::memory_order_relaxed) > 0.5f);
    engine.setHaasTimeMs (haasTimeMs->load (std::memory_order_relaxed));
    engine.setOutputDb (outputDb->load (std::memory_order_relaxed));
    engine.setAutoMonoSafetyFloorDb (autoMonoSafetyFloorDb->load (std::memory_order_relaxed));
    engine.setAutoMonoSafetyMultibandEnabled (autoMonoSafetyMultiband->load (std::memory_order_relaxed) > 0.5f);
    engine.setDecorrelateEnabled (decorrelateEnabled->load (std::memory_order_relaxed) > 0.5f);
    engine.setDecorrelateAmountPercent (decorrelateAmount->load (std::memory_order_relaxed));

    engine.prepare (spec);

    // Firmament has no oversampling, convolution, or other latency-inducing
    // stage - the Linkwitz-Riley bass-mono crossover is a zero-latency TPT
    // structure - so this is always 0, but it is still reported explicitly
    // (rather than relying on AudioProcessor's default) so the intent is
    // documented and this stays correct if a latent stage is ever added.
    setLatencySamples (engine.getLatencySamples());
}

void FirmamentAudioProcessor::releaseResources()
{
}

void FirmamentAudioProcessor::reset()
{
    engine.reset();
}

bool FirmamentAudioProcessor::isBusesLayoutSupported (const BusesLayout& layouts) const
{
    const auto mono = juce::AudioChannelSet::mono();
    const auto stereo = juce::AudioChannelSet::stereo();

    const auto mainOut = layouts.getMainOutputChannelSet();
    const auto mainIn = layouts.getMainInputChannelSet();

    // Firmament is fundamentally a stereo processor - Mid/Side encoding
    // needs two channels to mean anything - so the output bus must be
    // stereo.
    if (mainOut != stereo)
        return false;

    // The input bus may be mono (some hosts route a mono source into a
    // stereo effect chain) or stereo; mono input is handled gracefully in
    // processBlock() by duplicating the single channel before M/S encode,
    // which degrades safely to an unwidened mono pass-through (Side == 0)
    // rather than crashing or producing a hard-panned artifact.
    if (mainIn != mono && mainIn != stereo)
        return false;

    return true;
}

void FirmamentAudioProcessor::processBlock (juce::AudioBuffer<float>& buffer, juce::MidiBuffer&)
{
    juce::ScopedNoDenormals noDenormals;

    const auto totalNumInputChannels = getTotalNumInputChannels();
    const auto totalNumOutputChannels = getTotalNumOutputChannels();

    // The output bus is always stereo (isBusesLayoutSupported), so if the
    // input bus is mono, duplicate the single input channel into the second
    // channel before M/S encoding - clearing it instead would make the
    // encoded Side channel equal to half the mono signal (a hard-panned
    // artifact), whereas duplicating makes Side == 0 exactly, i.e. a clean,
    // graceful mono pass-through regardless of the Width setting.
    for (auto channel = totalNumInputChannels; channel < totalNumOutputChannels; ++channel)
        buffer.copyFrom (channel, 0, buffer, 0, 0, buffer.getNumSamples());

    engine.setWidthPercent (widthPercent->load (std::memory_order_relaxed));
    engine.setLowWidthPercent (lowWidthPercent->load (std::memory_order_relaxed));
    engine.setBassMonoFrequencyHz (bassMonoFreqHz->load (std::memory_order_relaxed));
    engine.setAutoMonoSafetyEnabled (autoMonoSafetyEnabled->load (std::memory_order_relaxed) > 0.5f);
    engine.setHaasEnabled (haasEnabled->load (std::memory_order_relaxed) > 0.5f);
    engine.setHaasTimeMs (haasTimeMs->load (std::memory_order_relaxed));
    engine.setOutputDb (outputDb->load (std::memory_order_relaxed));
    engine.setAutoMonoSafetyFloorDb (autoMonoSafetyFloorDb->load (std::memory_order_relaxed));
    engine.setAutoMonoSafetyMultibandEnabled (autoMonoSafetyMultiband->load (std::memory_order_relaxed) > 0.5f);
    engine.setDecorrelateEnabled (decorrelateEnabled->load (std::memory_order_relaxed) > 0.5f);
    engine.setDecorrelateAmountPercent (decorrelateAmount->load (std::memory_order_relaxed));

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    // Refresh the correlation/phase meter value for any reader (see
    // getCorrelationMeterValue()) - a plain atomic store, real-time-safe.
    correlationMeterValue.store (engine.getCorrelationValue(), std::memory_order_relaxed);
}

//==============================================================================
bool FirmamentAudioProcessor::hasEditor() const
{
    return true;
}

juce::AudioProcessorEditor* FirmamentAudioProcessor::createEditor()
{
    return new FirmamentAudioProcessorEditor (*this);
}

//==============================================================================
void FirmamentAudioProcessor::getStateInformation (juce::MemoryBlock& destData)
{
    const auto state = apvts.copyState();
    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void FirmamentAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
        apvts.replaceState (juce::ValueTree::fromXml (*xmlState));
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FirmamentAudioProcessor();
}
