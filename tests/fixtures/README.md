# Frozen v0.2.0 reference fixtures

Consumed by the v0.3.0 state-migration null tests in `tests/StateTests.cpp`
and the sentinel-null test in `tests/MultibandWidthTests.cpp` (binding brief
`.scaffold/research/2026-07-25-sota/brief-firmament.md`, section 6.1).

Both files were generated ONCE from the actual v0.2.0 sources (`origin/main`
commit `0bc7b4e`, macOS arm64, Debug) via a throwaway generator test in a
detached worktree, and are FROZEN - never regenerate them from a v0.3.0+
binary, or the cross-version null in 6.1(b) becomes circular and worthless.

- `v020-state.bin` - the exact `getStateInformation()` bytes the v0.2.0
  binary produced with width = 140 %, bassMonoFreq = 120 Hz, autoMonoSafety
  on, decorrelateEnabled on (Classic), everything else at defaults.
- `v020-reference-render.f32` - the v0.2.0 binary's render of the
  deterministic pink-noise stimulus (TestHelpers::DeterministicPinkNoise,
  seeds 123456789/987654321, amplitude 0.35, 469 blocks x 512 samples
  @48 kHz) with that state applied. Raw little-endian float32: all 240128
  left samples, then all 240128 right samples.

The stimulus generator is bit-exact across IEEE-754 platforms by
construction (integer LCG, values quantised to multiples of 2^-15, power-of-
two scaling - no libm involved), so any residual against the reference
measures the processing, not the stimulus. Cross-platform tolerance
rationale lives in the 6.1(b) test's comments.
