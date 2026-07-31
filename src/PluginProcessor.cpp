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
            // v0.3.0 additions (additive only - the 10 presets above are
            // frozen; see the binding brief's State migration section).
            { BinaryData::velvetWidth_json, BinaryData::velvetWidth_jsonSize },
            { BinaryData::masteringLinearPhaseBassMono_json, BinaryData::masteringLinearPhaseBassMono_jsonSize },
            { BinaryData::threeBandImager_json, BinaryData::threeBandImager_jsonSize },
        };
    }

    // v0.3.0 state schema version (see the binding brief's State migration
    // section): written to the APVTS root on save; absent in v0.1.x/v0.2.0
    // states, which therefore load as version 1. The neutral-default design
    // of every new parameter IS the migration - version numbers exist so any
    // future non-neutral change has a hook.
    constexpr const char* stateVersionProperty = "stateVersion";
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
    decorrelateMode = apvts.getRawParameterValue (ParamIDs::decorrelateMode);
    bassMonoMode = apvts.getRawParameterValue (ParamIDs::bassMonoMode);
    highSplitFreq = apvts.getRawParameterValue (ParamIDs::highSplitFreq);
    highWidth = apvts.getRawParameterValue (ParamIDs::highWidth);
    safetyMode = apvts.getRawParameterValue (ParamIDs::safetyMode);
    widthComp = apvts.getRawParameterValue (ParamIDs::widthComp);
    monoAudition = apvts.getRawParameterValue (ParamIDs::monoAudition);

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
    jassert (decorrelateMode != nullptr);
    jassert (bassMonoMode != nullptr);
    jassert (highSplitFreq != nullptr);
    jassert (highWidth != nullptr);
    jassert (safetyMode != nullptr);
    jassert (widthComp != nullptr);
    jassert (monoAudition != nullptr);

    // M2 default resolution: user "Default" preset > factory "Default"
    // preset > the ParameterLayout defaults apvts was just constructed
    // with above (see PresetManager::applyStartupDefault()'s docs).
    presetManager.applyStartupDefault();

    // v0.3.0: 50 ms message-thread servicing cadence for Linear Phase
    // kernel handoffs and dynamic latency reporting - see the class
    // comment in PluginProcessor.h for why this is a timer, not an
    // audio-thread-triggered AsyncUpdater.
    startTimer (50);
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
    engine.setDecorrelateMode (static_cast<int> (decorrelateMode->load (std::memory_order_relaxed)));
    engine.setBassMonoMode (static_cast<int> (bassMonoMode->load (std::memory_order_relaxed)));
    engine.setHighSplitFrequencyHz (highSplitFreq->load (std::memory_order_relaxed));
    engine.setHighWidthPercent (highWidth->load (std::memory_order_relaxed));
    engine.setSafetyMode (static_cast<int> (safetyMode->load (std::memory_order_relaxed)));
    engine.setWidthCompensationEnabled (widthComp->load (std::memory_order_relaxed) > 0.5f);
    engine.setMonoAuditionEnabled (monoAudition->load (std::memory_order_relaxed) > 0.5f);

    engine.prepare (spec);

    // v0.3.0: latency is now dynamic - 0 for every minimum-phase path, N/2
    // samples while the Linear Phase bass-mono mode is commanded (the
    // codebase's first nonzero-latency stage; see LinearPhaseCrossover.h).
    // prepareToPlay() is called by the host on whatever thread the host
    // chooses - the VST3/AU contract guarantees only that it is not the
    // audio thread, NOT that it is JUCE's own MessageManager thread (this
    // comment previously claimed otherwise; see
    // LinearPhaseCrossover.h's THREADING comment and
    // tests/CrossThreadReprepareTests.cpp for why that assumption was false
    // and what protects against it). This still reports directly because
    // engine.prepare() above has already synchronously produced the correct
    // latency for whichever thread called prepareToPlay(); mid-stream mode
    // changes made after prepareToPlay() are picked up by the 50 ms
    // servicing timer (handleMessageThreadServicing()).
    //
    // latencyReportMutex additionally serialises this call against
    // handleMessageThreadServicing()'s own setLatencySamples() call below:
    // juce::AudioProcessor::setLatencySamples() reads-then-writes its own
    // plain (non-atomic) latencySamples member, so two threads calling it
    // concurrently - this one and the real message thread's 50 ms timer -
    // race on JUCE's own base-class state even once LinearPhaseCrossover's
    // and FirmamentEngine's state are both correctly synchronised (caught
    // directly by ThreadSanitizer; see tests/CrossThreadReprepareTests.cpp).
    {
        const std::lock_guard<std::mutex> lock (latencyReportMutex);
        setLatencySamples (engine.getLatencySamples());
    }
}

void FirmamentAudioProcessor::handleMessageThreadServicing (bool forceKernelReload)
{
    engine.serviceLinearPhaseUpdates (forceKernelReload);

    const auto engineLatency = engine.getLatencySamples();

    // See prepareToPlay()'s latencyReportMutex comment - same mutex, same
    // reason.
    const std::lock_guard<std::mutex> lock (latencyReportMutex);

    if (engineLatency != getLatencySamples())
        setLatencySamples (engineLatency);
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
    engine.setDecorrelateMode (static_cast<int> (decorrelateMode->load (std::memory_order_relaxed)));
    engine.setBassMonoMode (static_cast<int> (bassMonoMode->load (std::memory_order_relaxed)));
    engine.setHighSplitFrequencyHz (highSplitFreq->load (std::memory_order_relaxed));
    engine.setHighWidthPercent (highWidth->load (std::memory_order_relaxed));
    engine.setSafetyMode (static_cast<int> (safetyMode->load (std::memory_order_relaxed)));
    engine.setWidthCompensationEnabled (widthComp->load (std::memory_order_relaxed) > 0.5f);
    engine.setMonoAuditionEnabled (monoAudition->load (std::memory_order_relaxed) > 0.5f);

    juce::dsp::AudioBlock<float> block (buffer);
    engine.process (block);

    // Refresh the correlation/phase meter values for any reader (see
    // getCorrelationMeterValue() and the v0.3.0 per-band/output meters) -
    // plain atomic stores, real-time-safe.
    correlationMeterValue.store (engine.getCorrelationValue(), std::memory_order_relaxed);
    correlationMeterLow.store (engine.getCorrelationLowValue(), std::memory_order_relaxed);
    correlationMeterHigh.store (engine.getCorrelationHighValue(), std::memory_order_relaxed);
    correlationMeterMidBand.store (engine.getCorrelationMidBandValue(), std::memory_order_relaxed);
    correlationMeterHighBand.store (engine.getCorrelationHighBandValue(), std::memory_order_relaxed);
    outputCorrelationMeter.store (engine.getOutputCorrelationValue(), std::memory_order_relaxed);
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
    auto state = apvts.copyState();

    // v0.3.0 state schema versioning (see the binding brief's State
    // migration section): every save is stamped with the current version.
    state.setProperty (stateVersionProperty, currentStateVersion, nullptr);

    const std::unique_ptr<juce::XmlElement> xml (state.createXml());
    copyXmlToBinary (*xml, destData);
}

void FirmamentAudioProcessor::setStateInformation (const void* data, int sizeInBytes)
{
    const std::unique_ptr<juce::XmlElement> xmlState (getXmlFromBinary (data, sizeInBytes));

    if (xmlState != nullptr && xmlState->hasTagName (apvts.state.getType()))
    {
        const auto state = juce::ValueTree::fromXml (*xmlState);

        // Absent attribute => a v0.1.x/v0.2.0 state => version 1. Version 1
        // states load as-is (APVTS's tolerant load leaves every new v0.3.0
        // parameter at its neutral ParameterLayout default): the
        // neutral-default design IS the migration, verified bit-exactly by
        // tests/StateTests.cpp. The version exists so any future
        // non-neutral schema change has a transformation hook here.
        loadedStateVersion = static_cast<int> (state.getProperty (stateVersionProperty, 1));

        apvts.replaceState (state);
    }
}

//==============================================================================
// This creates new instances of the plugin.
juce::AudioProcessor* JUCE_CALLTYPE createPluginFilter()
{
    return new FirmamentAudioProcessor();
}
