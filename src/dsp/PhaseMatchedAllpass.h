#pragma once

#include <juce_dsp/juce_dsp.h>

// The companion 2nd-order allpass for Firmament's Linkwitz-Riley (LR4)
// crossovers (v0.3.0 brief, section 3.2b):
//
//   AP2(s) = (s^2 - sqrt(2)*w0*s + w0^2) / (s^2 + sqrt(2)*w0*s + w0^2),
//   w0 = 2*pi*fc, Q = 1/sqrt(2)
//
// which satisfies angle(AP2) == angle(HP4_LR) == angle(LP4_LR) at every
// frequency, so passing the *other* path (Mid, or the low band at the second
// crossover) through it makes both paths phase-track exactly and the
// recombined signal is seam-free (research-stereo-imaging.md section 2.2).
//
// Implementation: the binding constraint is that the AP2 must be derived
// from the *same prewarped w0* as the LR4 sections and recomputed in the
// same once-per-block update from the same smoothed fc snapshot - any
// prewarp/discretisation mismatch breaks the phase identity (asserted to
// within 1 degree by tests/BassMonoPhaseTests.cpp, with a negative control).
// Rather than hand-rolling a TPT AP2 and hoping its prewarp matches JUCE's,
// this wraps juce::dsp::LinkwitzRileyFilter (JUCE 8.0.14) itself in its
// documented `allpass` mode: JUCE's own class documentation states the LR4
// low+high sum "is equivalent to an all-pass filter" and the allpass type
// computes exactly that sum from the *identical* TPT state equations and
// tan()-prewarped coefficients as the lowpass/highpass types - so the phase
// match holds bit-for-bit by construction, not by numerical coincidence, at
// every sample rate and cutoff. (This satisfies the brief's "hand-rolled TPT
// AP2" intent - same prewarp, same recompute cadence - with strictly
// stronger guarantees than a parallel implementation could offer.)
//
// Always-process discipline: like every other conditionally-used filter in
// FirmamentEngine, instances of this class are fed their input stream every
// sample regardless of whether the output is currently blended in, so
// engaging Phase Matched mode mid-stream never resumes from stale state.
class PhaseMatchedAllpass
{
public:
    // Message thread only (may allocate internal state).
    void prepare (const juce::dsp::ProcessSpec& spec)
    {
        filter.prepare (spec);
        filter.setType (juce::dsp::LinkwitzRileyFilterType::allpass);
    }

    // Audio-thread safe; same clamped-cutoff contract as the LR4 crossovers
    // (the caller passes the identical once-per-block smoothed fc snapshot).
    void setCutoffFrequency (float frequencyHz) noexcept
    {
        filter.setCutoffFrequency (frequencyHz);
    }

    void reset() { filter.reset(); }

    float processSample (int channel, float input) noexcept
    {
        return filter.processSample (channel, input);
    }

private:
    juce::dsp::LinkwitzRileyFilter<float> filter;
};
