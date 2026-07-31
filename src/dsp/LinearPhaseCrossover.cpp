#include "LinearPhaseCrossover.h"

#include <cmath>

namespace
{
    // Zeroth-order modified Bessel function of the first kind, by power
    // series - the standard Kaiser-window helper. Converges quickly for the
    // argument range used here (0 <= x <= 9).
    double besselI0 (double x) noexcept
    {
        double sum = 1.0;
        double term = 1.0;

        for (int k = 1; k < 64; ++k)
        {
            const auto factor = x / (2.0 * static_cast<double> (k));
            term *= factor * factor;
            sum += term;

            if (term < 1.0e-16 * sum)
                break;
        }

        return sum;
    }

    constexpr double kaiserBeta = 9.0;
    constexpr juce::uint32 coalesceIntervalMs = 50;
}

void LinearPhaseCrossover::prepare (const juce::dsp::ProcessSpec& monoSpec, float initialCutoffHz)
{
    // See the class-level THREADING comment: prepare() is reached from
    // FirmamentAudioProcessor::prepareToPlay(), which the host may call from
    // any non-audio thread - not necessarily JUCE's message thread - while
    // serviceMessageThreadUpdates() below can concurrently fire from the
    // real message thread (the 50 ms juce::Timer). This lock makes the two
    // mutually exclusive regardless of which OS threads they land on.
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    jassert (monoSpec.numChannels == 1);

    sampleRate = monoSpec.sampleRate;
    maximumBlockSize = static_cast<int> (monoSpec.maximumBlockSize);

    // N = 4096 * fs / 48000, rounded to even so the N/2 group delay is an
    // exact integer at every supported rate (brief 3.2c).
    const auto n = 2 * static_cast<int> (std::lround (4096.0 * sampleRate / 48000.0 / 2.0));
    kernelLength = n + 1;

    convolution.prepare (monoSpec);

    // The N/2 delay never changes between prepare() calls; a power-of-two
    // circular buffer keeps the audio-thread indexing branch-free.
    delaySamples = static_cast<size_t> (n / 2);
    const auto delayBufferSize = static_cast<size_t> (juce::nextPowerOfTwo (n / 2 + 1));
    delayMask = delayBufferSize - 1;
    sideDelayBuffer.assign (delayBufferSize, 0.0f);
    midDelayBuffer.assign (delayBufferSize, 0.0f);
    delayWriteIndex = 0;

    convolutionScratch.assign (static_cast<size_t> (juce::jmax (1, maximumBlockSize)), 0.0f);

    // A conservative bound on "the background loader has installed the IR
    // and its output crossfade has completed", measured in processed
    // samples: several maximum-size blocks or 100 ms, whichever is larger.
    settleSamples = static_cast<juce::uint64> (juce::jmax (4 * maximumBlockSize, static_cast<int> (sampleRate * 0.1)));

    samplesProcessed.store (0, std::memory_order_relaxed);
    publishedEpoch.store (0, std::memory_order_relaxed);
    requestedEpoch.store (0, std::memory_order_relaxed);
    samplesAtRequest.store (0, std::memory_order_relaxed);
    lastLoadedCutoffHz = -1.0f;
    lastLoadMillis = 0;

    // The convolution engine itself must not add latency on top of the
    // kernel's own N/2 group delay (the uniform-partitioned default engine
    // reports 0); fold it in anyway so the reported total stays honest if
    // the engine configuration ever changes.
    latencySamples = static_cast<int> (delaySamples) + convolution.getLatency();

    targetCutoffHz.store (initialCutoffHz, std::memory_order_relaxed);
    kernelDirty.store (true, std::memory_order_release);
    serviceMessageThreadUpdates (true);
}

int LinearPhaseCrossover::getLatencySamples() const noexcept
{
    // Same mutex as prepare()/serviceMessageThreadUpdates() - see the
    // class-level THREADING comment. This reads latencySamples, a plain
    // (non-atomic) member written by prepare(); getLatencySamples() is only
    // ever called from message-thread contexts (FirmamentAudioProcessor::
    // prepareToPlay()/handleMessageThreadServicing()), never from the audio
    // thread, so taking the lock here adds no audio-thread cost.
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);
    return latencySamples;
}

void LinearPhaseCrossover::reset() noexcept
{
    std::fill (sideDelayBuffer.begin(), sideDelayBuffer.end(), 0.0f);
    std::fill (midDelayBuffer.begin(), midDelayBuffer.end(), 0.0f);
    delayWriteIndex = 0;
    convolution.reset();
}

void LinearPhaseCrossover::setTargetCutoffFrequency (float frequencyHz) noexcept
{
    const auto previous = targetCutoffHz.load (std::memory_order_relaxed);

    // A relative threshold keeps block-rate smoother jitter from spamming
    // recompute requests while still tracking any audible fc move.
    if (std::abs (frequencyHz - previous) > 0.01f * juce::jmax (1.0f, previous))
    {
        targetCutoffHz.store (frequencyHz, std::memory_order_relaxed);
        kernelDirty.store (true, std::memory_order_release);
    }
}

void LinearPhaseCrossover::serviceMessageThreadUpdates (bool force)
{
    // See the class-level THREADING comment / prepare()'s lock comment
    // above - same mutex, same reason.
    const std::lock_guard<std::recursive_mutex> lock (messageThreadMutex);

    if (! kernelDirty.load (std::memory_order_acquire))
        return;

    const auto now = juce::Time::getMillisecondCounter();

    if (! force && lastLoadMillis != 0 && (now - lastLoadMillis) < coalesceIntervalMs)
        return;

    const auto cutoff = targetCutoffHz.load (std::memory_order_relaxed);
    kernelDirty.store (false, std::memory_order_relaxed);

    if (std::abs (cutoff - lastLoadedCutoffHz) < 1.0e-3f)
        return; // already loaded for this cutoff

    requestKernelLoad (cutoff);
    lastLoadMillis = now == 0 ? 1 : now;
}

void LinearPhaseCrossover::requestKernelLoad (float cutoffHz)
{
    const auto kernel = designKernel (cutoffHz);

    juce::AudioBuffer<float> irBuffer (1, static_cast<int> (kernel.size()));
    irBuffer.copyFrom (0, 0, kernel.data(), static_cast<int> (kernel.size()));

    // Convolution copies the buffer and swaps the transformed IR in through
    // its internal background loader (with an output crossfade) - the one
    // and only handoff mechanism for this stage (see class comment).
    convolution.loadImpulseResponse (std::move (irBuffer), sampleRate,
                                     juce::dsp::Convolution::Stereo::no,
                                     juce::dsp::Convolution::Trim::no,
                                     juce::dsp::Convolution::Normalise::no);

    lastLoadedCutoffHz = cutoffHz;
    samplesAtRequest.store (samplesProcessed.load (std::memory_order_relaxed), std::memory_order_relaxed);
    requestedEpoch.fetch_add (1, std::memory_order_release);
}

std::vector<float> LinearPhaseCrossover::designKernel (float cutoffHz) const
{
    const auto n = kernelLength - 1;
    const auto half = static_cast<double> (n) / 2.0;
    const auto normalisedCutoff = juce::jlimit (1.0e-4, 0.45, static_cast<double> (cutoffHz) / sampleRate);

    std::vector<double> kernel (static_cast<size_t> (kernelLength));
    const auto i0Beta = besselI0 (kaiserBeta);
    double sum = 0.0;

    for (int i = 0; i <= n; ++i)
    {
        const auto t = static_cast<double> (i) - half;

        // Normalised sinc lowpass at the requested cutoff...
        const auto x = 2.0 * juce::MathConstants<double>::pi * normalisedCutoff * t;
        const auto sinc = std::abs (x) < 1.0e-12 ? 1.0 : std::sin (x) / x;

        // ...shaped by a Kaiser window, beta = 9 (~90 dB stopband).
        const auto windowArg = 1.0 - juce::square (2.0 * static_cast<double> (i) / static_cast<double> (n) - 1.0);
        const auto window = besselI0 (kaiserBeta * std::sqrt (juce::jmax (0.0, windowArg))) / i0Beta;

        kernel[static_cast<size_t> (i)] = 2.0 * normalisedCutoff * sinc * window;
        sum += kernel[static_cast<size_t> (i)];
    }

    // Normalise to exactly unity DC gain, so the low band never changes the
    // level of fully-passed content.
    std::vector<float> result (kernel.size());
    const auto normalisation = sum != 0.0 ? 1.0 / sum : 1.0;

    for (size_t i = 0; i < kernel.size(); ++i)
        result[i] = static_cast<float> (kernel[i] * normalisation);

    return result;
}

void LinearPhaseCrossover::processBlock (const float* sideIn,
                                         float* sideLowOut,
                                         float* sideHighOut,
                                         const float* midIn,
                                         float* midDelayedOut,
                                         int numSamples) noexcept
{
    jassert (numSamples <= maximumBlockSize);

    // Convolve the Side stream with the current kernel -> low band.
    std::copy (sideIn, sideIn + numSamples, convolutionScratch.data());

    {
        float* channelPointer = convolutionScratch.data();
        juce::dsp::AudioBlock<float> block (&channelPointer, 1, static_cast<size_t> (numSamples));
        juce::dsp::ProcessContextReplacing<float> context (block);
        convolution.process (context);
    }

    for (int i = 0; i < numSamples; ++i)
    {
        sideDelayBuffer[delayWriteIndex & delayMask] = sideIn[i];
        midDelayBuffer[delayWriteIndex & delayMask] = midIn[i];

        const auto readIndex = (delayWriteIndex - delaySamples) & delayMask;
        const auto delayedSide = sideDelayBuffer[readIndex];
        const auto low = convolutionScratch[static_cast<size_t> (i)];

        sideLowOut[i] = low;
        sideHighOut[i] = delayedSide - low; // perfect reconstruction by construction
        midDelayedOut[i] = midDelayBuffer[readIndex];

        ++delayWriteIndex;
    }

    // Epoch publication: once the most recent load request is verifiably
    // active (IR present in the engine and the conservative settle window
    // has elapsed), expose it to kernelEpoch() pollers.
    const auto processed = samplesProcessed.fetch_add (static_cast<juce::uint64> (numSamples), std::memory_order_relaxed)
                           + static_cast<juce::uint64> (numSamples);

    const auto requested = requestedEpoch.load (std::memory_order_acquire);

    if (publishedEpoch.load (std::memory_order_relaxed) < requested
        && convolution.getCurrentIRSize() > 0
        && processed >= samplesAtRequest.load (std::memory_order_relaxed) + settleSamples)
    {
        publishedEpoch.store (requested, std::memory_order_release);
    }
}
