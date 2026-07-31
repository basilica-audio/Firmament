// Cross-thread convolution-reconfiguration hardening (bug class first found
// and fixed in sibling plugin basilica-audio/Nave, PR #28, then re-confirmed
// live in sibling basilica-audio/Crypta, PR #72). This test file is the
// independent Firmament audit + regression coverage for the same bug class
// in LinearPhaseCrossover's use of juce::dsp::Convolution.
//
// ============================================================================
// AUDIT (performed against this checkout, origin/main @ 130afb1, JUCE 8.0.14)
// ============================================================================
//
// ENTRY POINTS INTO juce::dsp::Convolution::loadImpulseResponse(), both real
// and currently wired:
//
//   1. FirmamentAudioProcessor::prepareToPlay (PluginProcessor.cpp)
//        -> FirmamentEngine::prepare()               (FirmamentEngine.cpp)
//        -> LinearPhaseCrossover::prepare()           (LinearPhaseCrossover.cpp)
//        -> requestKernelLoad() -> convolution.loadImpulseResponse()
//
//      prepareToPlay() is called by the host on a thread of the HOST's
//      choosing. The VST3 and Audio Unit specs guarantee only that this is
//      not the audio (real-time processing) thread - they make no promise
//      that it is JUCE's own MessageManager thread. This is exactly the
//      non-guarantee that caused Nave's #27/#28 crash and that Nave's own
//      CabConvolutionEngine.h class comment documents at length.
//
//   2. juce::Timer::timerCallback() (real JUCE message-thread callback,
//      started unconditionally in FirmamentAudioProcessor's constructor via
//      startTimer(50) - PluginProcessor.cpp - so it is running for the
//      entire lifetime of any processor instance, including inside
//      pluginval)
//        -> FirmamentAudioProcessor::handleMessageThreadServicing()
//        -> FirmamentEngine::serviceLinearPhaseUpdates()
//        -> LinearPhaseCrossover::serviceMessageThreadUpdates()
//        -> requestKernelLoad() -> convolution.loadImpulseResponse()
//           (only when a cutoff change is pending AND >= 50 ms have passed
//           since the last load, per the coalescing check - but note prepare()
//           itself unconditionally forces a load via
//           serviceMessageThreadUpdates(true), and any prior automation can
//           leave kernelDirty set, so this path is live essentially any time
//           the timer fires while Linear Phase mode is engaged)
//
//   juce::Timer callbacks are documented by JUCE to always run on the
//   message thread (see juce::Timer class docs, JUCE 8.0.14). That is the
//   functional equivalent of Nave's AsyncUpdater::handleAsyncUpdate().
//
// THE FALSE ASSUMPTION ON RECORD: PluginProcessor.cpp, in
// FirmamentAudioProcessor::prepareToPlay(), carries the comment "prepareToPlay
// runs on the message thread, so this reports directly". This is the exact
// same false assumption Nave's PR #28 documents and refutes ("the VST3/AU
// contract guarantees only that it is not the audio thread - not that it is
// JUCE's own MessageManager thread"). It is FALSE for pluginval specifically
// (pluginval drives prepareToPlay()/processBlock() from its own worker
// thread while the real JUCE message thread runs independently for GUI
// support), which is exactly the environment that triggered Nave's crash.
//
// THE RACE: no synchronisation of any kind (mutex, atomic gate, etc.) exists
// between LinearPhaseCrossover::prepare() and
// LinearPhaseCrossover::serviceMessageThreadUpdates() before this fix -
// both call requestKernelLoad() -> convolution.loadImpulseResponse()
// directly. If a host (or pluginval) calls prepareToPlay() from a thread
// other than JUCE's message thread while the 50 ms timer concurrently fires
// serviceMessageThreadUpdates() on the real message thread - both perfectly
// plausible in the field once Linear Phase mode is engaged and the user (or
// automation) is moving the bass-mono cutoff - two threads can call
// juce::dsp::Convolution::loadImpulseResponse() on the SAME Convolution
// instance concurrently and unsynchronised. loadImpulseResponse()'s
// background-loader hand-off (juce::dsp::BackgroundMessageQueue::push(), in
// JUCE 8.0.14's juce_Convolution.cpp) is documented safe only "from a single
// thread at a time" - concurrent unsynchronised callers can corrupt its
// internal FixedSizeFunction command slots, and the background convolution
// loader thread (a third thread besides these two) later invokes a
// corrupted/empty one, throwing std::bad_function_call with no reachable
// catch handler on that thread, aborting the whole process. Same mechanism
// as Nave #27/#28 and Crypta #72, different call sites.
//
// PRECONDITION FOR THE RACE TO BE LIVE: Linear Phase bass-mono mode must be
// engaged (ParamIDs::bassMonoMode == FirmamentEngine::BassMonoMode::linearPhase,
// i.e. 2) AND ParamIDs::bassMonoFreq > 0 (FirmamentEngine.h,
// LinearPhaseCrossover.h class comment) - only then does prepare() reach
// LinearPhaseCrossover::prepare(), and only then does the FIR kernel-recompute
// path in serviceMessageThreadUpdates() do anything. This test engages both.
//
// VERDICT: the race is real, and the "prepareToPlay runs on the message
// thread" comment was INACCURATE - it is only true when the host happens to
// call prepareToPlay() from JUCE's message thread, which is not guaranteed
// and demonstrably false under pluginval.
//
// VERIFICATION METHODOLOGY AND RESULT. The test below reproduces the
// concurrent-entry scenario directly with the real juce::Timer mechanism (no
// stand-in). Two forms of verification were run against it:
//
//   1. Plain (non-instrumented) Debug builds, stress-run repeatedly (58 runs
//      total against the unfixed code, matching-or-exceeding Nave's own
//      24-run campaign) looking for the literal std::bad_function_call
//      abort. This did NOT reproduce the crash on this platform/toolchain in
//      that many runs - being a genuine data race, a non-reproduction here is
//      NOT evidence of absence (Nave's own hit rate was only ~1-in-20; the
//      exact manifestation depends on scheduler timing, kernel-design cost
//      at the sample rate/N in play, and how the corrupted FixedSizeFunction
//      bytes happen to land), so a stronger tool was used instead of
//      concluding "no defect" from this alone.
//
//   2. The SAME test binary built with -fsanitize=thread (ThreadSanitizer),
//      which does not rely on the race actually corrupting memory badly
//      enough to crash - it directly observes and reports genuinely
//      unsynchronised concurrent accesses to the same memory. Run 5/5 times
//      against the UNFIXED code: every single run reported 10-11 distinct
//      data races, always in the exact locations this fix addresses:
//        - LinearPhaseCrossover::prepare() (every plain member it writes:
//          sampleRate, maximumBlockSize, kernelLength, latencySamples,
//          delaySamples, lastLoadedCutoffHz, lastLoadMillis) racing against
//          serviceMessageThreadUpdates()/requestKernelLoad()/designKernel()
//          - i.e. prepare() and the timer-driven servicing path genuinely
//          interleaving on the same LinearPhaseCrossover instance, exactly
//          the scenario this test targets.
//        - juce::dsp::ConvolutionEngineQueue's internal `pendingCommand`
//          FixedSizeFunction (juce_Convolution.cpp) - the EXACT object Nave's
//          PR #28 identifies as the corruption site for std::bad_function_call
//          - written by both LinearPhaseCrossover::requestKernelLoad() call
//          sites (one via prepare(), one via serviceMessageThreadUpdates())
//          with no synchronisation between them.
//        - FirmamentEngine::setBassMonoMode()/setBassMonoFrequencyHz()
//          (called from prepareToPlay(), audio-thread-role) racing against
//          FirmamentEngine::getLatencySamples() (called from
//          handleMessageThreadServicing(), message-thread-role) over the
//          plain lastBassMonoMode/lastBassMonoHz members - a second, related
//          race one level up from LinearPhaseCrossover, also fixed here (see
//          "THE FIX" below).
//        - juce::AudioProcessor::setLatencySamples()/getLatencySamples()
//          (JUCE 8.0.14's own base-class members) racing between
//          prepareToPlay()'s and handleMessageThreadServicing()'s calls -
//          fixed here too, at the FirmamentAudioProcessor level.
//      Against the FIXED code, the same TSAN campaign (10 runs) came back
//      clean in 9/10 runs and reported exactly ONE residual race in the
//      10th - see "KNOWN RESIDUAL FINDING, ACCEPTED" below, which is a
//      distinct, out-of-scope, inherent-to-juce::dsp::Convolution's own
//      design pattern, not a leftover of the bug this fix targets.
//   Red-verified by `git stash` on the six fixed source files (this test
//   file is untracked, so it survives the stash) - see the PR body for the
//   exact before/after TSAN summaries.
//
// OTHER CONCURRENT-ENTRY ANGLES CHECKED (per the brief) - none found:
//   - releaseResources(): empty body (PluginProcessor.cpp), touches nothing.
//   - AudioProcessor::reset()/FirmamentEngine::reset()/
//     LinearPhaseCrossover::reset(): clear state buffers and call
//     convolution.reset() only - juce::dsp::Convolution::reset() is a
//     different, audio-thread-safe operation (clears internal history/FIFOs,
//     no IR content is touched, no background queue interaction) from
//     loadImpulseResponse(), so this is not part of the same race and is
//     unaffected by the fix below (the mutex is never taken by reset()).
//   - setStateInformation(): only calls apvts.replaceState() - it does NOT
//     call prepareToPlay(), engine.prepare(), or touch LinearPhaseCrossover
//     directly at all. Any resulting bassMonoMode/bassMonoFreq parameter
//     change is picked up later, either by the next processBlock() (audio-
//     thread-safe setters only) or by the 50 ms timer's
//     serviceMessageThreadUpdates() - so setStateInformation() itself is not
//     an additional entry point into Convolution.
//   - grep for std::function members that could be invoked before
//     assignment (the other half of the named bug class, per Nave/Crypta's
//     root cause being corrupted FixedSizeFunction slots): no std::function
//     members exist anywhere under src/ outside ParameterLayout.cpp's
//     transient lambda arguments to APVTS parameter constructors (consumed
//     synchronously by JUCE's own code, not stored/invoked later by this
//     codebase).
//
// ============================================================================
// THE FIX (three layers, all message-thread-only - never touching
// processBlock()/the audio thread)
// ============================================================================
//
// 1. src/dsp/LinearPhaseCrossover.{h,cpp} (primary fix): a
//    `mutable std::recursive_mutex messageThreadMutex` is now taken at the
//    top of prepare(), serviceMessageThreadUpdates(), and getLatencySamples()
//    - the message-thread-only methods that touch requestKernelLoad() /
//    convolution.loadImpulseResponse() or the plain state prepare() writes -
//    exactly the Nave/Crypta pattern. The mutex is NEVER taken by
//    processBlock(), reset(), or setTargetCutoffFrequency(): those remain
//    lock-free and allocation-free, as asserted by
//    tests/AllocationGuardTests.cpp's existing conventions.
//
// 2. src/dsp/FirmamentEngine.{h,cpp}: getLatencySamples() previously
//    recomputed its answer from lastBassMonoMode/lastBassMonoHz - plain,
//    audio-thread-owned members written by the audio-thread-legal
//    setBassMonoMode()/setBassMonoFrequencyHz() setters - directly, racing
//    against those setters when called from a message-thread context. It now
//    reads a dedicated `std::atomic<bool> linearPhaseCommanded` that both
//    setters publish on every call (a plain relaxed store/load - no
//    allocation, no lock, negligible audio-thread cost).
//
// 3. src/PluginProcessor.{h,cpp}: a plain `std::mutex latencyReportMutex`
//    now guards both call sites of setLatencySamples() (prepareToPlay() and
//    handleMessageThreadServicing()), because juce::AudioProcessor's own
//    setLatencySamples()/getLatencySamples() (JUCE 8.0.14) read-then-write a
//    plain, non-atomic base-class member with no thread-safety guarantee of
//    their own - a residual race one level up from FirmamentEngine, also
//    caught directly by ThreadSanitizer. Never taken by processBlock().
//
// Together these remove the race structurally (the message-thread-only
// callers can no longer touch shared state concurrently, regardless of which
// OS threads they land on) rather than merely narrowing the timing window.
//
// KNOWN RESIDUAL FINDING, ACCEPTED (out of scope): even after all three
// fixes, TSAN occasionally (1/10 runs in this campaign) still reports a race
// inside juce::dsp::ConvolutionEngineQueue::postPendingCommand() between the
// AUDIO thread (processBlock() -> Convolution::process() -> postPendingCommand(),
// reading `pendingCommand`) and a message-thread loadImpulseResponse() call
// (writing it). This is JUCE's OWN internal design for its OFFICIALLY
// SUPPORTED usage pattern - exactly one non-audio-thread caller of
// loadImpulseResponse() plus the audio thread calling process() every block
// - not a leftover of the two-unsynchronised-non-audio-callers bug this fix
// targets, and it is NOT fixable at this level without taking a lock inside
// processBlock(), which would violate the audio-thread lock-free/
// allocation-free contract this whole fix is trying to protect. Nave's own
// fix (CabConvolutionEngine.h) has the identical residual - its class
// comment explicitly states the mutex is "never taken by any audio-thread
// method (reset()/process()...)" - so this is accepted suite-wide precedent,
// not a Firmament-specific gap.

#include "PluginProcessor.h"
#include "params/ParameterIds.h"

#include "PluginProcessor.h"
#include "params/ParameterIds.h"
#include "TestHelpers.h"

#include <catch2/catch_test_macros.hpp>
#include <juce_events/juce_events.h>

#include <atomic>
#include <thread>

namespace
{
    void setParam (FirmamentAudioProcessor& processor, const char* id, float realValue)
    {
        auto* param = processor.apvts.getParameter (id);
        REQUIRE (param != nullptr);
        param->setValueNotifyingHost (param->convertTo0to1 (realValue));
    }
}

// THIS TEST reproduces the concurrent-entry scenario directly, mirroring
// Nave's CrossThreadReprepareTests.cpp three-thread structure: one thread
// stands in for the host's own prepareToPlay()-calling thread (repeatedly
// repreparing at 44.1/96/192 kHz with both a small (64) and a large (1024)
// block size, with Linear Phase bass-mono mode engaged so the FIR path is
// actually live), a second thread stands in for host automation (moving
// bassMonoFreq via setValueNotifyingHost(), which is how real DAWs typically
// deliver automation - from a non-message thread), and the test's own
// calling thread - which IS "the message thread" per TestMain.cpp's
// ScopedJuceInitialiser_GUI - pumps juce::MessageManager's dispatch loop so
// the REAL 50 ms juce::Timer genuinely fires serviceMessageThreadUpdates()
// concurrently with the other two threads' work. Deliberately does NOT call
// handleMessageThreadServicing() directly for this test - that would bypass
// the real juce::Timer mechanism this bug class depends on and defeat the
// point of the reproduction.
//
// Being a genuine data race, this is a best-effort reproduction: any single
// run could get lucky and miss the window. See the PR body for the exact
// red-verification evidence (reverting the fix and re-running this test).
TEST_CASE ("Concurrent prepareToPlay and timer-driven Linear Phase servicing survive a multi-rate reprepare", "[processor][threading][v0.3.0]")
{
    FirmamentAudioProcessor processor;
    processor.prepareToPlay (48000.0, 512);

    // Engage Linear Phase bass-mono mode (BassMonoMode::linearPhase == 2)
    // with a real cutoff, so LinearPhaseCrossover::prepare() and
    // serviceMessageThreadUpdates() both do real convolution work rather
    // than being skipped entirely.
    setParam (processor, ParamIDs::bassMonoMode, 2.0f);
    setParam (processor, ParamIDs::bassMonoFreq, 120.0f);
    processor.handleMessageThreadServicing (true);

    auto* bassMonoFreqParam = processor.apvts.getParameter (ParamIDs::bassMonoFreq);
    REQUIRE (bassMonoFreqParam != nullptr);

    std::atomic<bool> stop { false };
    std::atomic<bool> sawNonFiniteOutput { false };

    // Simulates host automation of the bass-mono cutoff, delivered from a
    // non-message thread (real DAWs typically deliver automation from the
    // audio thread). setTargetCutoffFrequency() itself is audio-thread-safe
    // (a plain atomic store), but the resulting kernelDirty flag is what
    // serviceMessageThreadUpdates() (fired by the real timer below) acts on
    // concurrently with the reprepare thread's own direct calls into
    // prepare().
    std::thread automationThread ([&]
    {
        int i = 0;
        while (! stop.load (std::memory_order_relaxed))
        {
            const auto hz = (i++ % 2 == 0) ? 80.0f : 180.0f;
            bassMonoFreqParam->setValueNotifyingHost (bassMonoFreqParam->convertTo0to1 (hz));
            std::this_thread::yield();
        }
    });

    // Simulates the host's own prepareToPlay()-calling thread: repeatedly
    // reprepares across 44.1/96/192 kHz with both a small and a large block
    // size and processes audio, exactly the sweep pluginval performs.
    std::thread hostThread ([&]
    {
        for (int iteration = 0; iteration < 20; ++iteration)
        {
            for (double sampleRate : { 44100.0, 96000.0, 192000.0 })
            {
                for (int blockSize : { 64, 1024 })
                {
                    processor.prepareToPlay (sampleRate, blockSize);

                    juce::AudioBuffer<float> buffer (2, blockSize);
                    juce::MidiBuffer midi;

                    for (int block = 0; block < 2; ++block)
                    {
                        TestHelpers::fillStereoWithDistinctSines (buffer, sampleRate, 220.0, 330.0);
                        processor.processBlock (buffer, midi);

                        if (! TestHelpers::allSamplesFinite (buffer))
                            sawNonFiniteOutput.store (true, std::memory_order_relaxed);
                    }
                }
            }
        }

        stop.store (true, std::memory_order_relaxed);
    });

    // This test's own calling thread is JUCE's message thread (the first
    // thread to touch MessageManager::getInstance() - TestMain.cpp's main()
    // - becomes it). Pumping it here is what lets the real 50 ms juce::Timer
    // actually fire handleMessageThreadServicing() concurrently with the two
    // threads above - exactly as it would in a real host where the message
    // thread runs independently of whichever thread the host calls
    // prepareToPlay() from. If the race this test targets were still
    // present, the crash happens on JUCE's internal convolution
    // background-loader thread - a fourth thread besides these three - so no
    // try/catch here could intercept it; the process aborts exactly as it
    // did in Nave's CI.
    while (! stop.load (std::memory_order_relaxed))
        juce::MessageManager::getInstance()->runDispatchLoopUntil (1);

    automationThread.join();
    hostThread.join();

    REQUIRE_FALSE (sawNonFiniteOutput.load());
}
