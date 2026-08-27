#pragma once

#include <juce_dsp/juce_dsp.h>

#include "LinearPhaseCrossover.h"
#include "MidSideCodec.h"
#include "PhaseMatchedAllpass.h"
#include "VelvetDecorrelator.h"

#include <array>
#include <atomic>
#include <vector>

// The complete Firmament signal path, independent of juce::AudioProcessor so
// it can be exercised directly by unit tests without instantiating a full
// plugin (see tests/EngineTests.cpp). Owns all DSP state; every filter is
// allocated in prepare() and never reallocated on the audio thread.
//
// Signal flow (see docs/architecture.md for the full diagram):
//
//   L/R -> encode M/S -> [multiband] Width scale on Side (2 or 3 bands)
//       -> [optional] Auto Mono Safety gain on Side -> decode M/S -> L/R
//       -> [optional] equal-power width compensation -> [optional] Haas
//       Mode delay on Right XOR Decorrelate -> Output trim -> [optional]
//       Mono Audition
//
// ============================ v0.2.0 heritage ============================
//
// Multiband width: when the bass-mono crossover is engaged (BassMonoFreq >
// 0), the derived Side stream is split into a low and a high band by the
// same juce::dsp::LinkwitzRileyFilter used for the v0.1 "bass mono" feature,
// and each band is scaled by its own width control - LowWidth below the
// crossover, Width above it - before being summed back together. Scaling a
// band by a constant commutes exactly with the (linear) filtering that
// produced it, so at LowWidth's default of 0% - where the low band's
// contribution is exactly zero regardless of what it contains - this
// reproduces the v0.1 behaviour of forcing the low band to silence exactly.
// Per JUCE's own documentation, a Linkwitz-Riley crossover's low+high sum is
// a flat-*magnitude* allpass, not the original signal - this is the
// standard, expected characteristic of any Linkwitz-Riley-crossover-based
// multiband processor. With the crossover off (BassMonoFreq == 0), Width
// alone scales the entire Side signal as a single band, exactly as in v0.1.
//
// Auto Mono Safety: a running, leaky-integrated correlation estimate of the
// plugin's *input* L/R (computed every sample regardless of whether the
// safety feature is engaged) is used, when enabled, to attenuate the Side
// signal in proportion to how out-of-phase the input is. Because this only
// ever scales Side and never touches Mid, it preserves the exact mono-sum
// invariant (L + R == 2 * Mid at any setting). The on/off toggle is
// crossfaded (not an instant gate). v0.2.0 ballistics/dead-zone/floor:
// 300 ms leaky integrator, dead-zone [-0.10, 1.0] -> full gain, linear ramp
// to the user floor (-24..0 dB, default -9.1 dB) at correlation -1.
//
// Haas Mode: an alternative, non-M/S widening technique applied *after* M/S
// decode - the Right channel is delayed by HaasTimeMs relative to Left via a
// juce::dsp::DelayLine. Unlike Width-based widening, this does NOT preserve
// the exact mono-sum invariant - it trades that guarantee for the
// well-known precedence-effect widening. Off by default.
//
// Decorrelate ("Classic" mode, v0.2.0): a gentler alternative widening
// technique for near-mono material, also applied post-M/S-decode to the
// Right channel: a cascade of 4 fixed-frequency allpass IIR filters blended
// with the dry Right signal by decorrelateAmount. Decorrelate and Haas Mode
// are mutually exclusive: whenever both are enabled, Decorrelate takes
// effect and the Haas delay line is pinned to 0 samples. Both remain
// explicit, documented exceptions to the mono-sum invariant.
//
// ============================ v0.3.0 additions ===========================
// (binding brief: .scaffold/research/2026-07-25-sota/brief-firmament.md;
// math sources: research-stereo-imaging.md - see docs/research-notes.md
// Section 7 for the full citations)
//
// 1. Velvet-noise decorrelation modes: "Velvet Dense" (OVN30) and "Velvet
//    Sparse" (OVN15) use the published DAFx-18 optimal tap pairs in a
//    *symmetric complementary* topology, post-M/S-decode, projected onto
//    the mono-safe manifold: the Side component follows the brief's wet mix
//    exactly - S' = (1-d)*S + d*(VND_A(L) - VND_B(R))/2 - while Mid stays
//    bit-exactly dry, so every velvet setting is mono-sum invariant BY
//    CONSTRUCTION (the published pair *sum* A+B has a real ~17 dB
//    third-octave notch, so the raw per-channel wet mix could not meet the
//    binding <= 3 dB fold-down spec; see the derivation comment in
//    FirmamentEngine.cpp and docs/architecture.md). Both channels are
//    genuinely processed (symmetric widening cost, centre image stays put),
//    unlike the R-only Classic cascade (which remains available and
//    default, and remains a documented mono-sum exception). All three
//    decorrelate paths always process ("always process, conditionally
//    use"); the output is selected via smoothed 50 ms crossfade weights,
//    and the master enable is likewise a smoothed 0..1 gain rather than the
//    v0.2.0 instant per-block gate.
//
// 2. Bass-mono mode selector (active only while BassMonoFreq > 0):
//    - Classic: bit-exact v0.2.0 behaviour (LR4 split on Side, Mid dry).
//    - Phase Matched: Mid additionally passes the companion 2nd-order
//      allpass AP2(fc, Q = 1/sqrt(2)) whose phase tracks the LR4 sections
//      exactly (see PhaseMatchedAllpass.h), making the recombination
//      seam-free. Crossfaded against Classic over 50 ms.
//    - Linear Phase: FIR complementary split of Side (Kaiser-sinc lowpass,
//      S_high = delay - S_low, perfect reconstruction by construction) with
//      Mid delayed to match - see LinearPhaseCrossover.h. This is the
//      codebase's first nonzero-latency path: getLatencySamples() becomes
//      dynamic (N/2 in this mode, 0 otherwise) and mode changes to/from
//      Linear Phase are handled with a short mute-crossfade (documented -
//      hosts renegotiate PDC).
//
// 3. 3-band width: a second Side-path LR4 at HighSplitFreq (sentinel 0 =
//    off, mirroring BassMonoFreq) splits the content above the bass-mono
//    crossover into mid (Width) and high (High Width) bands. Flat-sum
//    discipline: the low band passes AP2(HighSplitFreq) before the band sum
//    so all bands carry identical phase at the second crossover (standard
//    3-way Linkwitz-Riley practice); in Phase Matched mode Mid gets
//    AP2(BassMonoFreq) * AP2(HighSplitFreq). The internal clamp
//    HighSplitFreq >= 2 * BassMonoFreq keeps the crossovers ordered.
//
// 4. Safety Response modes: "Smooth" is the bit-exact v0.2.0 static map on
//    the 300 ms estimate. "Dynamic" drives the same static map from a
//    second, fast (30 ms) correlation estimator and passes the resulting
//    gain through a dedicated asymmetric one-pole with compressor-style
//    ballistics. Ballistics convention (binding): attack 5 ms / release
//    250 ms are times to 90% settling toward the target, so the internal
//    time constants are tau = t90 / ln(10) (~2.17 ms / ~108.6 ms). The gain
//    smoother is a dedicated one-pole, NOT juce::SmoothedValue. Energy-gate
//    rule (binding for every correlation estimator in the plugin, guard
//    detectors and display meters alike): below an energy gate the
//    estimate decays toward 0 instead of holding its last ratio - a leaky
//    Pearson ratio is invariant under silence (numerator and denominator
//    decay at the same rate), so without the gate the guard would hold
//    rho = -1 after an anti-phase burst into silence and never release.
//
// 5. Equal-power width compensation (opt-in): with a = (1+w)/2,
//    b = (1-w)/2, post-decode makeup g = 1/sqrt(a^2+b^2) (exactly 1 at
//    w = 1), computed from the broadband width parameter only (documented
//    multiband limitation), smoothed 50 ms. Off by default (mastering
//    convention: width changes side level only).
//
// 6. Mono audition: post-everything (after the output trim),
//    L = R = (L+R)/2, crossfaded 50 ms. The one stage that is deliberately
//    NOT part of the mono-sum invariant tests - it *is* the fold-down.
//
// 7. Haas/toggle polish: Lagrange 3rd-order delay interpolation, per-sample
//    smoothed delay-time application (kills the automation pitch-zipper),
//    and crossfaded (not instant) enable gates for both Haas Mode and
//    Decorrelate, mirroring the safety toggle discipline.
//
// 8. Meter surface: per-band *input* correlation (low / above-bass-mono /
//    mid / high) and a broadband *output* (post-processing) correlation,
//    all exposed as block-rate getters for the processor's atomics.
class FirmamentEngine
{
public:
    enum class DecorrelateMode
    {
        classic = 0,
        velvetDense = 1,
        velvetSparse = 2
    };

    enum class BassMonoMode
    {
        classic = 0,
        phaseMatched = 1,
        linearPhase = 2
    };

    enum class SafetyMode
    {
        smooth = 0,
        dynamic = 1
    };

    FirmamentEngine();

    // Allocates all DSP state. Message thread only; must be called (and
    // completed) before the first process() call, and again whenever sample
    // rate/block size/channel count change. `spec.numChannels` is expected
    // to be 2 (stereo); process() is a safe no-op for any other channel
    // count.
    void prepare (const juce::dsp::ProcessSpec& spec);

    // Clears all filter/gain/delay-line state without deallocating. Safe to
    // call from the audio thread (e.g. on playback stop/loop).
    void reset();

    // Processes `block` in place. `block` must be a 2-channel (stereo)
    // block, channel 0 = left, channel 1 = right; a zero-sample or
    // non-stereo block is a safe no-op. Blocks larger than the maximum
    // declared to prepare() are processed in chunks of at most that size
    // (oversized-block guard). No allocation occurs here.
    void process (juce::dsp::AudioBlock<float>& block) noexcept;

    // Parameter setters, in real units. Safe to call every block from the
    // audio thread - no allocation/locks.
    void setWidthPercent (float newWidthPercent);
    void setLowWidthPercent (float newLowWidthPercent);
    void setBassMonoFrequencyHz (float newFrequencyHz);
    void setAutoMonoSafetyEnabled (bool shouldBeEnabled);
    void setHaasEnabled (bool shouldBeEnabled);
    void setHaasTimeMs (float newHaasTimeMs);
    void setOutputDb (float newOutputDb);

    // v0.2.0 additions.
    void setAutoMonoSafetyFloorDb (float newFloorDb);
    void setAutoMonoSafetyMultibandEnabled (bool shouldBeEnabled);
    void setDecorrelateEnabled (bool shouldBeEnabled);
    void setDecorrelateAmountPercent (float newAmountPercent);

    // v0.3.0 additions - see the class-level comment above.
    void setDecorrelateMode (int newMode);
    void setBassMonoMode (int newMode);
    void setHighSplitFrequencyHz (float newFrequencyHz);
    void setHighWidthPercent (float newHighWidthPercent);
    void setSafetyMode (int newMode);
    void setWidthCompensationEnabled (bool shouldBeEnabled);
    void setMonoAuditionEnabled (bool shouldBeEnabled);

    // The most recent block's running correlation estimate of the plugin's
    // *input* L/R signal, in [-1, 1]. Updated once per process() call, safe
    // to read from any thread. Subject to the v0.3.0 energy-gate rule: the
    // estimate decays toward 0 under (near-)silence instead of holding its
    // last ratio (never show +/-1 on silence).
    float getCorrelationValue() const noexcept { return lastCorrelation; }

    // v0.3.0 meter surface: per-band input correlation (bands defined by
    // the bass-mono and high-split crossovers - always computed, regardless
    // of which stages are engaged) and the broadband *output*
    // (post-processing) correlation. Same update cadence/gating as
    // getCorrelationValue().
    float getCorrelationLowValue() const noexcept { return lastCorrelationLow; } // below BassMonoFreq
    float getCorrelationHighValue() const noexcept { return lastCorrelationHigh; } // above BassMonoFreq (v0.2.0 "high")
    float getCorrelationMidBandValue() const noexcept { return lastCorrelationMidBand; } // BassMonoFreq..HighSplitFreq
    float getCorrelationHighBandValue() const noexcept { return lastCorrelationHighBand; } // above HighSplitFreq
    float getOutputCorrelationValue() const noexcept { return lastOutputCorrelation; }

    // v0.3.0: dynamic. 0 in Classic/Phase Matched bass-mono modes (every
    // minimum-phase stage is sample-synchronous IIR; Haas Mode's delay is an
    // intentional relative channel offset, never reported); N/2 samples
    // while Linear Phase bass-mono is commanded (mode == linearPhase AND
    // BassMonoFreq > 0). The processor forwards changes via
    // setLatencySamples on the message thread - meaning THIS getter is also
    // only ever called from message-thread contexts (prepareToPlay()/
    // handleMessageThreadServicing()), never from the audio thread, while
    // setBassMonoMode()/setBassMonoFrequencyHz() are audio-thread-legal
    // setters. Cross-thread hardening (tests/CrossThreadReprepareTests.cpp,
    // caught directly by ThreadSanitizer): rather than read lastBassMonoMode/
    // lastBassMonoHz here - plain, audio-thread-owned state with no
    // synchronisation against a concurrent message-thread reader - this
    // reads linearPhaseCommanded, an atomic snapshot the two setters publish
    // on every audio-thread call (see their definitions).
    int getLatencySamples() const noexcept;

    // Message-thread service hook: forwards to
    // LinearPhaseCrossover::serviceMessageThreadUpdates() (coalesced FIR
    // kernel recompute + Convolution::loadImpulseResponse handoff). Called
    // by the processor's timer; tests may call it directly (with
    // force = true) for determinism.
    void serviceLinearPhaseUpdates (bool force = false) { linearPhase.serviceMessageThreadUpdates (force); }

    // Deterministic ready signal for the Linear Phase kernel handoff - see
    // LinearPhaseCrossover::kernelEpoch().
    juce::uint64 getLinearPhaseKernelEpoch() const noexcept { return linearPhase.kernelEpoch(); }

    // TEST-ONLY negative control for the Phase Matched phase-identity test
    // (brief 6.2): forces the Mid companion allpass to unity (weight 0) even
    // while Phase Matched mode is selected, which must make the phase-match
    // assertion fail. Never called from production code.
    void setPhaseMatchBypassedForTests (bool shouldBypass);

private:
    void processChunk (juce::dsp::AudioBlock<float>& block) noexcept;
    void refreshCrossfadeTargets();
    float computeWidthCompensationTarget() const noexcept;

    static constexpr double smoothingTimeSeconds = 0.05;

    // Correlation meter ballistics: one-pole leaky-integrator time constants.
    // 300 ms (v0.2.0) drives the display meters and the "Smooth" safety
    // path; 30 ms (v0.3.0) is the fast detector feeding the "Dynamic"
    // safety path (research-stereo-imaging.md section 2.5).
    static constexpr double correlationTimeConstantSeconds = 0.3;
    static constexpr double fastCorrelationTimeConstantSeconds = 0.03;

    // v0.3.0 Dynamic safety ballistics (binding convention, brief 3.4): the
    // published attack/release times are times to 90% settling toward the
    // target, so the internal one-pole time constants are t90 / ln(10).
    static constexpr double safetyAttackSeconds = 0.005 / 2.302585092994046; // ~2.17 ms
    static constexpr double safetyReleaseSeconds = 0.25 / 2.302585092994046; // ~108.6 ms

    // v0.3.0 Linear Phase transition mute-crossfade (brief 4: "short
    // mute-crossfade acceptable" on any change to/from Linear Phase - the
    // latency change makes a click-free transition impossible anyway).
    static constexpr double linearPhaseFadeSeconds = 0.005;

    // Maximum Haas Mode delay the DelayLine is sized for; matches the
    // haasTimeMs parameter's documented maximum with a small margin.
    static constexpr float maxHaasTimeMs = 41.0f;

    // Classic Decorrelate mode's cascaded allpass network (v0.2.0,
    // unchanged): stage count/frequencies are implementation-reasoned.
    static constexpr int numDecorrelateStages = 4;
    static constexpr std::array<float, numDecorrelateStages> decorrelateStageFrequenciesHz { 300.0f, 900.0f, 2700.0f, 8100.0f };
    static constexpr float decorrelateStageQ = 0.7f;

    double sampleRate = 44100.0;
    int preparedMaxBlockSize = 0;

    // Rest-flush threshold (fleet audit class 2b, issue #33): the exact
    // value juce_dsp's own per-block snapToZero() pass used
    // (JUCE_SNAP_TO_ZERO, juce_FloatVectorOperations.h, JUCE 8.0.14) before
    // the fleet disabled JUCE_DSP_ENABLE_SNAP_TO_ZERO. With that pass gone,
    // the LR4/TPT crossover state was measured parking on a rounding/FTZ
    // fixed point instead of decaying to zero (a constant ~2.6e-37 resting
    // output on arm64, ~6.6e-36 on x86_64) - see process() for the
    // silence-gated flush that replaces the library pass.
    static constexpr float restFlushThreshold = 1.0e-8f;

    // The flush must never touch in-flight audio: an impulse can be
    // travelling through the Linear Phase FIR (N/2 samples) or the Haas
    // delay (up to 40 ms) while the *input* is already silent. One full
    // second of contiguous silent input is an order-of-magnitude upper
    // bound over every structural delay in the engine at any sample rate,
    // so only a genuinely drained engine can be flushed.
    juce::int64 restFlushDwellSamples = 48000;
    juce::int64 silentInputStreak = 0;
    bool restFlushed = false;

    // ---- crossovers / filters -------------------------------------------
    // The bass-mono crossover operates on the single derived Side stream
    // (prepared mono); also used for the low/high multiband width split.
    juce::dsp::LinkwitzRileyFilter<float> bassMonoCrossover;

    // v0.3.0: second Side-path crossover at HighSplitFreq (3-band width).
    juce::dsp::LinkwitzRileyFilter<float> sideHighSplitCrossover;

    juce::dsp::Gain<float> outputGain;

    // Input-side band splits for the per-band correlation detectors: raw
    // L/R at BassMonoFreq (v0.2.0) and the content above it at
    // HighSplitFreq (v0.3.0). Always processed ("always process,
    // conditionally use") so their state never goes stale.
    juce::dsp::LinkwitzRileyFilter<float> leftMultibandCrossover;
    juce::dsp::LinkwitzRileyFilter<float> rightMultibandCrossover;
    juce::dsp::LinkwitzRileyFilter<float> leftHighSplitInputCrossover;
    juce::dsp::LinkwitzRileyFilter<float> rightHighSplitInputCrossover;

    // v0.3.0 Phase Matched companions (see PhaseMatchedAllpass.h): Mid at
    // the bass-mono corner, Mid at the high-split corner, and the low band's
    // AP2(HighSplitFreq) for the 3-band flat-sum discipline. All always
    // processed; blended in via the smoothed weights below.
    PhaseMatchedAllpass midBassAllpass;
    PhaseMatchedAllpass midHighAllpass;
    PhaseMatchedAllpass lowBandHighAllpass;

    // v0.3.0 Linear Phase bass-mono tier (see LinearPhaseCrossover.h).
    LinearPhaseCrossover linearPhase;
    std::vector<float> lpSideIn, lpMidIn, lpSideLow, lpSideHigh, lpMidDelayed;
    bool linearPhaseActive = false;

    // Classic Decorrelate allpass cascade (Right channel only, v0.2.0).
    std::array<juce::dsp::IIR::Filter<float>, numDecorrelateStages> decorrelateAllpassStages;

    // v0.3.0 velvet-noise decorrelators: symmetric complementary pairs
    // (channel A -> Left, channel B -> Right), dense (OVN30) and sparse
    // (OVN15). Always processed so mode switches start from warm state.
    VelvetDecorrelator velvetDenseLeft, velvetDenseRight;
    VelvetDecorrelator velvetSparseLeft, velvetSparseRight;

    // Haas Mode delay line - Right channel only. v0.3.0: Lagrange 3rd-order
    // interpolation (fractional-delay HF response; integer-sample delays,
    // including the pinned 0, are still exact) and per-sample smoothed
    // setDelay() calls. Always pushed/popped; the effective delay is the
    // smoothed Haas time multiplied by a smoothed 0/1 "pin" weight that is 0
    // whenever Haas is off or Decorrelate is engaged (mutual exclusivity).
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::Lagrange3rd> haasDelayLine;

    // ---- smoothed parameters --------------------------------------------
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> lowWidthSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highWidthSmoothed; // v0.3.0
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> bassMonoFrequencySmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Multiplicative> highSplitFrequencySmoothed; // v0.3.0
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> haasTimeMsSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> autoMonoSafetyAmountSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> autoMonoSafetyFloorGainSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decorrelateAmountSmoothed;

    // v0.3.0 crossfade weights (all 50 ms, all "select at the exact 0/1
    // endpoints" so settled states are bit-exact - see processChunk()):
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decorrelateEnableSmoothed; // master enable (fixes the v0.2.0 instant gate)
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decorrelateClassicWeightSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decorrelateDenseWeightSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> decorrelateSparseWeightSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> haasEnableSmoothed; // v0.3.0 crossfaded enable
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> haasPinSmoothed; // 0 while off/Decorrelate engaged
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midBassApWeightSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> midHighApWeightSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> highSplitBlendSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> safetyModeBlendSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> widthCompGainSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> monoAuditionSmoothed;
    juce::SmoothedValue<float, juce::ValueSmoothingTypes::Linear> linearPhaseFadeSmoothed;

    // ---- correlation estimators -----------------------------------------
    // Running leaky-integrator sums (double for precision over long runs),
    // computed from the *input* L/R every sample. 300 ms display/Smooth
    // set: broadband + per band. 30 ms fast set (v0.3.0): same structure,
    // feeds the Dynamic safety path.
    double correlationSumLR = 0.0, correlationSumLL = 0.0, correlationSumRR = 0.0;
    double correlationSumLRLow = 0.0, correlationSumLLLow = 0.0, correlationSumRRLow = 0.0;
    double correlationSumLRHigh = 0.0, correlationSumLLHigh = 0.0, correlationSumRRHigh = 0.0;
    double correlationSumLRMid = 0.0, correlationSumLLMid = 0.0, correlationSumRRMid = 0.0;
    double correlationSumLRTop = 0.0, correlationSumLLTop = 0.0, correlationSumRRTop = 0.0;

    double fastSumLR = 0.0, fastSumLL = 0.0, fastSumRR = 0.0;
    double fastSumLRLow = 0.0, fastSumLLLow = 0.0, fastSumRRLow = 0.0;
    double fastSumLRHigh = 0.0, fastSumLLHigh = 0.0, fastSumRRHigh = 0.0;
    double fastSumLRMid = 0.0, fastSumLLMid = 0.0, fastSumRRMid = 0.0;
    double fastSumLRTop = 0.0, fastSumLLTop = 0.0, fastSumRRTop = 0.0;

    // Output (post-processing) broadband correlation, 300 ms (v0.3.0).
    double outputSumLR = 0.0, outputSumLL = 0.0, outputSumRR = 0.0;

    double correlationSmoothingCoeff = 0.0;
    double fastCorrelationSmoothingCoeff = 0.0;

    // Energy-gated correlation states (v0.3.0 binding rule): under the
    // energy gate the estimate decays toward 0 instead of holding its last
    // (silence-invariant) ratio. With signal present each equals the plain
    // clamped Pearson ratio bit-for-bit.
    float gatedCorrelation = 0.0f;
    float gatedCorrelationLow = 0.0f, gatedCorrelationHigh = 0.0f;
    float gatedCorrelationMid = 0.0f, gatedCorrelationTop = 0.0f;
    float gatedFastCorrelation = 0.0f;
    float gatedFastCorrelationLow = 0.0f, gatedFastCorrelationHigh = 0.0f;
    float gatedFastCorrelationMid = 0.0f, gatedFastCorrelationTop = 0.0f;
    float gatedOutputCorrelation = 0.0f;

    // Dynamic safety gain smoother states (dedicated asymmetric one-poles,
    // per band + broadband; NOT SmoothedValue - brief 3.4 binding).
    float dynamicGainBroadband = 1.0f;
    float dynamicGainLow = 1.0f, dynamicGainHigh = 1.0f;
    float dynamicGainMid = 1.0f, dynamicGainTop = 1.0f;
    double safetyAttackCoeff = 0.0, safetyReleaseCoeff = 0.0;

    float lastCorrelation = 0.0f;
    float lastCorrelationLow = 0.0f, lastCorrelationHigh = 0.0f;
    float lastCorrelationMidBand = 0.0f, lastCorrelationHighBand = 0.0f;
    float lastOutputCorrelation = 0.0f;

    // ---- last commanded parameter values --------------------------------
    // Re-applied to the smoothers on every prepare() so re-preparing never
    // resets a live parameter. 0 Hz is the frozen "off" sentinel for both
    // crossover frequencies and is never fed to a smoother/filter.
    float lastWidthPercent = 100.0f;
    float lastLowWidthPercent = 0.0f;
    float lastBassMonoHz = 0.0f;
    bool lastAutoMonoSafetyEnabled = false;
    bool lastHaasEnabled = false;
    float lastHaasTimeMs = 20.0f;

    float lastAutoMonoSafetyFloorDb = -9.1f;
    bool lastAutoMonoSafetyMultibandEnabled = false;
    bool lastDecorrelateEnabled = false;
    float lastDecorrelateAmountPercent = 50.0f;

    // v0.3.0 - defaults reproduce v0.2.0 behaviour exactly.
    int lastDecorrelateMode = static_cast<int> (DecorrelateMode::classic);
    int lastBassMonoMode = static_cast<int> (BassMonoMode::classic);

    // Atomic snapshot of "lastBassMonoMode == linearPhase && lastBassMonoHz
    // > 0", published by setBassMonoMode()/setBassMonoFrequencyHz() (audio
    // thread) and consumed only by getLatencySamples() (message thread) -
    // see that getter's comment. A plain relaxed store/load: this is a
    // single independent flag, not a synchronisation point for other
    // memory, so acquire/release ordering buys nothing here.
    std::atomic<bool> linearPhaseCommanded { false };
    float lastHighSplitHz = 0.0f;
    float lastHighWidthPercent = 100.0f;
    int lastSafetyMode = static_cast<int> (SafetyMode::smooth);
    bool lastWidthCompensationEnabled = false;
    bool lastMonoAuditionEnabled = false;

    bool phaseMatchBypassedForTests = false;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (FirmamentEngine)
};
