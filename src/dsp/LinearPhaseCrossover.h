#pragma once

#include <juce_dsp/juce_dsp.h>

#include <atomic>
#include <mutex>
#include <vector>

// Linear-phase complementary bass-mono crossover (v0.3.0 brief, section
// 3.2c): the "reference tier" alternative to the minimum-phase LR4 split of
// the Side channel.
//
//   S_low  = FIR_LP(S)                (Kaiser-windowed sinc lowpass)
//   S_high = delay(S, N/2) - S_low    (perfect reconstruction by
//                                      construction - the magnitude sum is
//                                      exactly flat)
//   Mid    = delay(M, N/2)            (keeps Mid time-aligned with Side)
//
// Kernel: Kaiser beta = 9, length N + 1 with N = 4096 at 48 kHz
// (research-stereo-imaging.md section 3: "FIR length ~ 4096 @ 48 kHz for
// fc = 120 Hz"), scaled N = 4096 * fs / 48000 rounded to even, so the N/2
// group delay is always an exact integer. This is Firmament's first (and
// only) nonzero-latency stage; getLatencySamples() reports N/2 and the
// processor forwards it via setLatencySamples on the message thread.
//
// Kernel handoff (binding, ONE mechanism): the kernel is recomputed on the
// message thread - in prepare(), and on cutoff changes via
// serviceMessageThreadUpdates(), coalesced to at most one recompute per
// 50 ms - and installed with juce::dsp::Convolution::loadImpulseResponse(),
// which copies the buffer and swaps it in through Convolution's internal
// background loader with its built-in output crossfade. There is NO external
// double buffer and NO atomic index flip - Convolution owns its IR storage
// and only accepts new IRs via loadImpulseResponse. loadImpulseResponse
// allocates on the calling thread, so it is never called from the audio
// thread; the audio thread only publishes the desired cutoff through an
// atomic (setTargetCutoffFrequency()).
//
// juce::dsp::FIR::Filter is explicitly REJECTED for this stage (brief 3.2c):
// its coefficients live in a ReferenceCountedObjectPtr, and reassigning that
// pointer from the message thread while the audio thread dereferences it is
// a data race, so it cannot take dynamically recomputed kernels safely.
//
// THREADING (binding, cross-thread reconfiguration hardening - ported from
// the suite's Nave/Crypta bug class fix, sibling basilica-audio/nave PR #28,
// basilica-audio/crypta PR #72). prepare() and serviceMessageThreadUpdates()
// are both documented "message thread only", but that describes the
// REQUIRED caller, not an ENFORCED one: prepare() is reached from
// FirmamentAudioProcessor::prepareToPlay(), which the host calls on
// whatever thread the host chooses - the VST3/AU contract guarantees only
// that it is not the audio thread, NOT that it is JUCE's own MessageManager
// thread. serviceMessageThreadUpdates() is reached from a real juce::Timer
// callback, which always runs on the actual message thread. Those can be
// two different OS threads, so a host whose prepareToPlay()-calling thread
// differs from JUCE's message thread (true of pluginval, which drives its
// test sequence from its own thread while the JUCE message thread runs
// independently) can end up with both methods running concurrently -
// confirmed by direct reproduction under ThreadSanitizer (see
// tests/CrossThreadReprepareTests.cpp): both reach requestKernelLoad() ->
// juce::dsp::Convolution::loadImpulseResponse(), whose background hand-off
// is documented safe only "from a single thread at a time", and unsynchronised
// concurrent callers can corrupt its internal command slots (the same
// mechanism as Nave's std::bad_function_call crash, #27/#28).
//
// messageThreadMutex (a std::recursive_mutex - recursive because prepare()
// calls serviceMessageThreadUpdates() internally) serialises prepare(),
// serviceMessageThreadUpdates(), and getLatencySamples() - the third because
// it reads plain (non-atomic) state written by prepare() and is itself only
// ever called from message-thread contexts (FirmamentAudioProcessor::
// prepareToPlay()/handleMessageThreadServicing()), never from the audio
// thread. This removes the race structurally rather than narrowing its
// timing window. The mutex is NEVER taken by processBlock(), reset(), or
// setTargetCutoffFrequency() - those stay lock-free and allocation-free, as
// asserted by tests/AllocationGuardTests.cpp.
//
// Determinism for tests: kernelEpoch() is a monotonic counter that
// increments once a newly requested kernel load is active in the process
// path (installed by Convolution's background loader and past its output
// crossfade, bounded conservatively in processed samples). The Catch2
// harness runs no dispatch loop, so tests must pump the message loop (or
// call serviceMessageThreadUpdates() directly), keep processing audio, and
// poll kernelEpoch() with a bounded timeout before opening a measurement
// window - see TestHelpers.h and tests/BassMonoPhaseTests.cpp.
class LinearPhaseCrossover
{
public:
    LinearPhaseCrossover() = default;

    // Message thread only: allocates everything, computes the kernel for
    // `initialCutoffHz` synchronously and requests its installation.
    // `monoSpec` must carry numChannels == 1 and the host's maximum block
    // size.
    void prepare (const juce::dsp::ProcessSpec& monoSpec, float initialCutoffHz);

    // Clears delay-line/convolution state without deallocating. Audio-thread
    // safe (used when the engine switches this stage in mid-stream).
    void reset() noexcept;

    // Splits `sideIn` into sideLowOut/sideHighOut and produces the matching
    // N/2-delayed Mid stream. All pointers must hold `numSamples` samples;
    // `numSamples` must not exceed the prepared maximum block size. Audio
    // thread; no allocation.
    void processBlock (const float* sideIn,
                       float* sideLowOut,
                       float* sideHighOut,
                       const float* midIn,
                       float* midDelayedOut,
                       int numSamples) noexcept;

    // Audio-thread safe: publishes the cutoff the message thread should
    // rebuild the kernel for (the engine passes its once-per-block smoothed,
    // Nyquist-clamped fc snapshot). A plain atomic store + dirty flag; the
    // actual recompute happens in serviceMessageThreadUpdates().
    void setTargetCutoffFrequency (float frequencyHz) noexcept;

    // Message thread only: if the target cutoff changed, recomputes the
    // kernel and hands it to Convolution::loadImpulseResponse(), coalesced
    // to at most one recompute per 50 ms (`force` bypasses the coalescing
    // interval - used by prepare() and deterministic tests).
    void serviceMessageThreadUpdates (bool force = false);

    // N/2 for the prepared sample rate (plus the convolution engine's own
    // latency, which is 0 for the uniform-partitioned default). Constant
    // between prepare() calls. Message thread only (see the class-level
    // THREADING comment) - takes messageThreadMutex, since it reads state
    // written by prepare() and is never called from the audio thread.
    int getLatencySamples() const noexcept;

    // See class comment. Starts at 0 after prepare(); reaches 1 once the
    // initial kernel is verifiably active, and increments again for every
    // subsequent installed recompute.
    juce::uint64 kernelEpoch() const noexcept { return publishedEpoch.load (std::memory_order_acquire); }

    // The kernel length currently designed for (N + 1 taps). Test surface.
    int getKernelLength() const noexcept { return kernelLength; }

private:
    void requestKernelLoad (float cutoffHz);
    std::vector<float> designKernel (float cutoffHz) const;

    // See the class-level THREADING comment: serialises prepare(),
    // serviceMessageThreadUpdates(), and getLatencySamples() - the three
    // methods that are only ever called from message-thread contexts but
    // are not guaranteed to be called from the SAME message-thread-context
    // thread. Recursive because prepare() calls serviceMessageThreadUpdates()
    // internally. Never taken by processBlock()/reset()/
    // setTargetCutoffFrequency() - those remain lock-free and
    // allocation-free.
    mutable std::recursive_mutex messageThreadMutex;

    juce::dsp::Convolution convolution;

    double sampleRate = 48000.0;
    int maximumBlockSize = 0;
    int kernelLength = 0; // N + 1
    int latencySamples = 0; // N / 2 (+ convolution engine latency)
    size_t delaySamples = 0; // exactly N / 2

    // Integer N/2 delay lines for the Side and Mid streams (simple
    // power-of-two circular buffers - the delay never changes between
    // prepare() calls, so no interpolation is involved).
    std::vector<float> sideDelayBuffer, midDelayBuffer;
    size_t delayMask = 0;
    size_t delayWriteIndex = 0;

    std::vector<float> convolutionScratch;

    // Message-thread kernel-handoff state (see class comment).
    std::atomic<float> targetCutoffHz { 120.0f };
    std::atomic<bool> kernelDirty { false };
    juce::uint32 lastLoadMillis = 0;
    float lastLoadedCutoffHz = -1.0f;

    // Epoch plumbing: the message thread bumps requestedEpoch when it hands
    // a kernel to loadImpulseResponse; the audio thread publishes it once
    // the install is verifiably active (IR present and a conservative
    // settle window of processed samples has elapsed, covering the
    // background loader plus Convolution's internal crossfade).
    std::atomic<juce::uint64> requestedEpoch { 0 };
    std::atomic<juce::uint64> samplesAtRequest { 0 };
    std::atomic<juce::uint64> publishedEpoch { 0 };
    std::atomic<juce::uint64> samplesProcessed { 0 };
    juce::uint64 settleSamples = 0;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (LinearPhaseCrossover)
};
