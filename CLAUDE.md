# Firmament — stereo widener & imager (mix)

Per-repo working memory for Claude Code sessions on this plugin. Part of the **Basilica Audio** plugin suite — sacred-architecture DSP for heavy music (`github.com/basilica-audio`).

## What this is
Firmament is the "stereo widener & imager (mix)" member of the suite. AU / VST3 / Standalone, JUCE 8.

## Status (v0.1.0 — M1 DSP completion & test coverage complete)
Core DSP + M1 additions (multiband width, Auto Mono Safety, correlation meter DSP, Haas Mode) working, **~49 Catch2 tests green** (sample-rate sweeps 44.1-192 kHz, bus-config coverage, long-run NaN/Inf stability, extreme automation). CI (macOS + Windows, pluginval strictness 10 + auval) green. GUI is a functional v0.1-style slider/toggle editor (custom LookAndFeel + a correlation/phase meter widget are roadmap M3 - GitHub issue "Custom GUI / LookAndFeel"). No signing yet (roadmap M4). Open work is tracked in this repo's GitHub **milestones/issues**.

## DSP
Firmament encodes L/R to Mid/Side (M=(L+R)/2, S=(L-R)/2, factored into a stateless MidSideCodec), then only ever scales the *Side* channel — never Mid — through the following stages, before decoding back to L/R and applying an output trim via juce::dsp::Gain:

- **Width / multiband width**: single-band Width (0-200%) scales all of Side when the crossover is off; with BassMonoFreq > 0, Side is split via a single juce::dsp::LinkwitzRileyFilter (JUCE 8.0.14, prepared mono, dual-output processSample) into a low and high band, each scaled independently by Low Width / Width. Low Width's default (0%) exactly reproduces the original "force Side to zero below BassMonoFreq" behaviour. **Important, JUCE-documented fact discovered/verified during M1**: a Linkwitz-Riley crossover's low+high sum is a flat-magnitude allpass, not an identity/null operation — see docs/architecture.md's "Bass-mono crossover" section for the exact citation and what it does/doesn't imply for this plugin.
- **Auto Mono Safety** (off by default): a 200 ms leaky-integrated input-correlation estimate drives an additional Side attenuation when the input goes strongly out-of-phase. Only ever scales Side, so it can't break the mono-sum invariant.
- **Correlation/phase meter**: the same running correlation estimate above is exposed via FirmamentAudioProcessor::getCorrelationMeterValue() — DSP-complete, not yet wired to a GUI widget (M3 scope).
- **Haas Mode** (off by default): a post-M/S-decode Right-channel delay (0-40 ms) via juce::dsp::DelayLine — a deliberately non-mono-sum-preserving alternative widening technique, clearly documented as the one exception to the invariant below.

Because Mid is never touched by Width/multiband width/Auto Mono Safety, L+R (the mono downmix) is provably identical to the input's mono downmix at any setting of those three — verified end-to-end in tests. Haas Mode is the sole, documented exception. The Linkwitz-Riley crossover is a zero-latency TPT structure and Haas Mode's delay is an intentional relative channel offset rather than a processing artifact (no oversampling/FIR anywhere), so getLatencySamples() is always exactly 0, reported via a static constexpr.

## Build & test
```sh
export CPM_SOURCE_CACHE="$HOME/.cache/CPM"      # shared JUCE 8.0.14 + Catch2 cache
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target Tests Firmament_Standalone --parallel 4
ctest --test-dir build --output-on-failure
```
Release/universal + pluginval + auval run in CI, not locally.

## Conventions & guardrails
- JUCE 8.0.14 via CPM · C++20 · AGPLv3 · Pamplejuce `SharedCode` pattern · manufacturer `Yvsv`, plugin code `Frmm`, `com.yvesvogl.firmament`.
- **Real-time safety:** no alloc/lock/file-IO/logging on the audio thread; allocate in `prepareToPlay`; `reset()` clears all state; `ScopedNoDenormals`; smoothed params; report latency via `setLatencySamples` where the chain adds any.
- **DryWetMixer gotcha (JUCE 8.0.14):** prime `setWetMixProportion(mix)` before `reset()` in `prepare()` (else it ramps from 100% wet). See sibling `overture`.
- **`main` is protected** — no direct commits; feature branch + PR, green CI required (Conventional Commits). New DSP needs tests (null/reference, NaN/Inf sweep, state round-trip, latency).

## Roadmap
GitHub milestones (M1 DSP & tests · M2 presets/state · M3 GUI & a11y · M4 release/signing/v1.0.0) + issues. Read with `gh issue list --repo basilica-audio/firmament`.

## Suite context
Style references: sibling `basilica-audio/overture` and `basilica-audio/crypta`. The suite: overture, tenebrae, nave, silentium, requiem, seraph, aureate, firmament, triptych, apotheosis, crypta.
