<p align="center"><img src="docs/assets/icon.png" alt="Firmament icon" width="160"/></p>

# Firmament

*Open the heavens — a stereo widener and imager for lush orchestral layers.*

[![CI](https://github.com/basilica-audio/firmament/actions/workflows/ci.yml/badge.svg)](https://github.com/basilica-audio/firmament/actions/workflows/ci.yml)
[![License: AGPL v3](https://img.shields.io/badge/License-AGPL%20v3-blue.svg)](https://www.gnu.org/licenses/agpl-3.0)

> **Work in progress.** Firmament is pre-1.0 and under active development. There are no built binaries or releases yet — building from source is currently the only way to run it. Expect breaking changes until v1.0.0 ships (see [Roadmap](#roadmap)).

<!-- ==BEGIN BODY== (plugin engineer: replace this block with What it is / Features / Signal flow / Roadmap) -->
## What it is

Firmament is a Mid/Side stereo widener and imager built on JUCE 8, aimed at giving lush orchestral layers (strings, choirs, synth pads) a wider, more open stereo image without smearing the low end: it scales the Side channel of a Mid/Side encode to control apparent width, with an optional multiband crossover that keeps sub-bass content centered (or independently wide) regardless of how wide the rest of the spectrum is pushed. See [`docs/manual.md`](docs/manual.md) for the full user manual (what each control does musically, tips, where Firmament sits in a mix chain).

## Features

- **Width** - 0-200% Mid/Side width scale on the (single- or high-band) Side channel; 0% collapses to mono, 100% is an unmodified pass-through of the input's stereo image, up to 200% doubles the Side channel for a maximally wide image
- **Bass Mono Freq / Low Width** - optional crossover (0-500 Hz, 0 = off) that splits the Side channel into independently-scaled low and high bands: Low Width (0-200%, default 0%) governs the low band, Width governs the high band. At the Low Width default, this is a classic "bass mono" utility that keeps sub-bass centered while Width still affects everything above the crossover
- **Auto Mono Safety** - optional automatic Side attenuation, driven by a running input-correlation estimate, that reins in the stereo image whenever the source is heavily out-of-phase - a safety net on top of Width/Low Width that never touches Mid, so it can never break the mono-compatibility guarantee below
- **Haas Mode** - optional alternative widening technique (0-40 ms Right-channel delay after M/S decode) that can add a sense of width even to genuinely mono-compatible material, at the cost of the exact mono-sum guarantee the rest of the plugin provides - off by default, documented clearly as a deliberate trade-off
- **Zero latency** - no oversampling or FIR stage anywhere in the chain; the bass-mono/multiband crossover is a zero-latency Linkwitz-Riley (TPT) structure, and Haas Mode's delay is an intentional relative channel offset rather than a reportable processing artifact, so `getLatencySamples()` is always 0
- **Mono-safe by construction** (Width/Low Width/Auto Mono Safety path) - because only the Side channel is ever touched by those three controls, the mono downmix of the output is provably identical to the mono downmix of the input at any setting (see [`docs/architecture.md`](docs/architecture.md)); Haas Mode is the sole, clearly-documented exception
- **Correlation/phase estimate** - a running input-correlation value is computed and exposed (`FirmamentAudioProcessor::getCorrelationMeterValue()`) for a future GUI meter; DSP-complete, visual meter widget is a later milestone
- **Output** - output trim, -24 dB to +24 dB
- Full state save/recall via `AudioProcessorValueTreeState`

## Signal flow

```
Input L/R --> Encode M/S --> Side --> [Bass Mono off] Width (0-200%) -------\
                                   \-> [Bass Mono on]  Low Width (low band)  |
                                                        Width (high band) --/
                                   --> [optional] Auto Mono Safety (out-of-phase attenuation)
                                Mid (untouched) ------------------------+
                                                                        v
                                                          Decode M/S --> [optional] Haas Mode
                                                                         (delay Right) --> Output trim --> Output L/R
```

See [`docs/architecture.md`](docs/architecture.md) for the full breakdown (Mermaid diagram, exact math, the Mid/Side mono-compatibility invariant, the Linkwitz-Riley crossover's real magnitude/phase behaviour, Haas Mode's trade-off, and mono-input-bus handling), or [`docs/manual.md`](docs/manual.md) for the plain-language user manual.

## Parameters

| Parameter | Range | Default | Unit |
|---|---|---|---|
| Width | 0-200 | 100 | % |
| Bass Mono Freq | 0-500 | 0 (off) | Hz |
| Low Width | 0-200 | 0 | % |
| Auto Mono Safety | off/on | off | - |
| Haas Mode | off/on | off | - |
| Haas Time | 0-40 | 20 | ms |
| Output | -24 to +24 | 0 | dB |

Full musical description of each parameter: [`docs/manual.md`](docs/manual.md).

## Roadmap

| Milestone | Description | Status |
|---|---|---|
| M0 | Bootstrap - project skeleton, CI, docs | Done |
| M1 | DSP completion - multiband width, Auto Mono Safety, correlation meter (DSP), Haas Mode, broadened test coverage | Done |
| M2 | Presets & state recall - factory presets, full non-parameter state | Planned |
| M3 | Custom GUI & accessibility - vector-drawn LookAndFeel, correlation meter widget, keyboard/screen-reader support | Planned |
| M4 | Release engineering - signing, notarization, installers, v1.0.0 | Planned |
<!-- ==END BODY== -->

## Installation

No pre-built binaries are published yet (see the work-in-progress notice above). Once releases begin, installation will follow the standard plugin locations:

**macOS**

| Format | Path |
|---|---|
| AU (Component) | `~/Library/Audio/Plug-Ins/Components/` |
| VST3 | `~/Library/Audio/Plug-Ins/VST3/` |

If Logic Pro doesn't pick up the plugin after installing, force a rescan by resetting the AU cache:

```sh
killall -9 AudioComponentRegistrar
auval -a
```

**Windows**

| Format | Path |
|---|---|
| VST3 | `C:\Program Files\Common Files\VST3\` |

## Building from source

Requires JUCE 8.0.14, C++20, and CMake ≥ 3.24. See [`docs/building.md`](docs/building.md) for full prerequisites and step-by-step build/test commands for macOS and Windows.

```sh
cmake -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

## License

Firmament is licensed under the [GNU Affero General Public License v3.0](LICENSE) (AGPLv3).

This project uses [JUCE](https://juce.com) 8, whose open-source tier is licensed under AGPLv3 (as of JUCE 8; JUCE 7 and earlier used GPLv3), which is why this project is AGPLv3 rather than GPLv3. See [`docs/adr/0002-agplv3-licensing.md`](docs/adr/0002-agplv3-licensing.md) for the full reasoning.

VST is a registered trademark of Steinberg Media Technologies GmbH.

Firmament is an independent open-source project and is not affiliated with, endorsed by, or sponsored by any plugin manufacturer.
