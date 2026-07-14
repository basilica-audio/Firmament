<p align="center"><img src="assets/icon.png" alt="Firmament icon" width="120"/></p>

# Firmament — User Manual

*Open the heavens — a stereo widener and imager for lush symphonic layers.*

## What Firmament is

Firmament is a Mid/Side stereo widener and imager. It is one of the twelve plugins in the **Metal up your ass** symphonic-metal suite, and its job in that suite (and in any mix) is to take content that already has stereo information - strings, choirs, synth pads, doubled/layered guitars, ambience returns - and control how wide it feels, without ever compromising how the mix folds down to mono (club PA, phone speaker, a broadcast mono-check, a bass player checking their part in mono).

Firmament does not create stereo width out of a genuinely mono signal by itself (Width and Low Width can only scale what stereo difference is already present in the input) - the one exception is **Haas Mode**, which *can* create a sense of width from mono-compatible material by delaying one channel, at the cost of the exact mono-sum guarantee the rest of the plugin provides (see below).

## Where it sits in a symphonic-metal chain

Firmament is a **width/imaging** tool, which places it toward the back end of a channel or bus chain, after tone-shaping and dynamics have already been decided:

1. **Corrective/tonal EQ, compression, saturation** (e.g. `overture`, `tenebrae`, other suite members) - shape the sound first.
2. **Firmament** - decide how wide it should feel, once the tone is set.
3. **Reverb/delay sends, final bus processing** - width decisions made before verb affect how the reverb tail itself is perceived; some engineers prefer to widen after the reverb return instead, which is a valid alternative depending on the source.

Typical placements in a symphonic-metal production:

- **String/choir/pad busses** - the primary use case. Push Width up to open the orchestral/choral layers without smearing the guitars/bass/kick that share the mix.
- **Doubled rhythm guitar bus** - a gentler touch (Width 110-140%) can glue two already-panned doubles into a single, wider-feeling wall without the extreme "hyped" artifacts of some stereo wideners, because Firmament never adds anything that wasn't already in the stereo image (again, aside from Haas Mode).
- **Master bus (used sparingly)** - Bass Mono is particularly valuable here: keep the kick/bass/low guitar energy centered (mono-compatible, and physically tighter on a PA) while the cymbals/strings/reverb tails above the crossover stay as wide as the mix already has them.
- **Mono-source instruments routed through a stereo bus** - Firmament accepts a mono input bus gracefully (see "Mono input" below); in that case Width has no effect at all unless Haas Mode is engaged, since there is no inter-channel difference to scale.

## Signal flow (plain-language version)

See [`docs/architecture.md`](architecture.md) for the full technical breakdown (Mermaid diagram, exact math, the Linkwitz-Riley crossover's real magnitude/phase behaviour, real-time-safety details). In short:

1. The input is split into **Mid** (mono content, `(L+R)/2`) and **Side** (stereo difference content, `(L-R)/2`).
2. If **Bass Mono** is off, **Width** scales the whole Side signal by a single amount.
3. If **Bass Mono** is engaged, Side is split into a low band (below the Bass Mono frequency) and a high band (above it); **Low Width** scales the low band and **Width** scales the high band independently.
4. If **Auto Mono Safety** is on, Side is further attenuated automatically whenever the input is heavily out-of-phase (a safety net against mono cancellation), on top of whatever Width/Low Width already did.
5. Mid and the processed Side are recombined back into Left/Right. Mid is *never* touched by any of the above - this is what guarantees the mono-fold-down safety described below.
6. If **Haas Mode** is on, the Right channel is delayed slightly relative to Left - a different, non-Mid/Side widening trick, applied last, before the final trim.
7. **Output** trims the overall level.

## Parameter reference

| Parameter | Range | Default | Unit | What it does |
|---|---|---|---|---|
| **Width** | 0-200 | 100 | % | Scales the Side (stereo-difference) signal. 100% is the input's original stereo image, unmodified. 0% collapses everything to mono (Side silenced). 200% doubles the Side channel's amplitude for a maximally wide, sometimes "hyped"/artificial-feeling image - useful in small doses, easy to overdo on a whole mix. When Bass Mono is engaged, Width governs only the band *above* the Bass Mono frequency. |
| **Bass Mono Freq** | 0-500 | 0 (off) | Hz | The crossover frequency below which Low Width (rather than Width) governs the Side signal. At 0 Hz (off), the whole spectrum is a single band governed by Width alone. Typical settings sit in the 80-200 Hz range - low enough to leave kick/bass/low-guitar fundamentals centered, high enough to still catch the "boxy" low-mid stereo smear some wide reverbs/pads produce. |
| **Low Width** | 0-200 | 0 | % | Independent width scale for the band *below* Bass Mono Freq - only audible while Bass Mono Freq is above 0 Hz. At the default of 0%, the low band is forced to mono, exactly like a classic "bass mono" utility - the standard mastering-bus move to keep sub-bass energy centered and translate well on smaller systems. Raise it above 0% if you specifically want some width to survive down low (rare, but occasionally useful for very wide pad/drone material where even the low end should breathe a little). |
| **Auto Mono Safety** | off/on | off | - | When on, automatically reins in the Side signal whenever the input is heavily out-of-phase (correlation trending toward -1), independent of and on top of Width/Low Width. A safety net for automated Width or aggressive settings on unpredictable source material (e.g. a widened synth pad that occasionally goes strongly out-of-phase); it never affects Mid, so it cannot break Firmament's mono-fold-down guarantee - it only ever reins in *how* wide Side gets. Leave it off if you are dialling in Width by ear and want full manual control; turn it on as a safety net on busses you can't constantly monitor (e.g. an automated width send). |
| **Haas Mode** | off/on | off | - | Enables an alternative widening technique: delays the Right channel by Haas Time relative to Left, after the Mid/Side stage. This is the *only* control in Firmament that can widen genuinely mono-compatible material (it works even at Width = 0%) - but unlike Width/Low Width, it does **not** preserve an exact mono-sum match with the input (summing two time-offset channels is inherently different from summing the original pair). Use it deliberately, and always check a mono fold-down before committing if translation matters for the target (e.g. broadcast, club systems). |
| **Haas Time** | 0-40 | 20 | ms | The Left/Right delay Haas Mode applies, only audible while Haas Mode is on. Short times (5-15 ms) read as subtle widening; the 15-35 ms "precedence effect" zone reads as a strong, immersive width; times approaching 40 ms start to read as a discrete slap/echo rather than width - back off if you hear a distinct repeat rather than a wider image. |
| **Output** | -24 to +24 | 0 | dB | Final output trim, applied after everything else (including Haas Mode). Firmament has no built-in limiter or ceiling - Width/Low Width above 100% and Output above 0 dB can both add gain, so use this to compensate level changes introduced by extreme Width settings, not as a general-purpose gain/makeup stage. |

## Tips

- **Start with Width, not Bass Mono.** Most material only needs the single global Width control; reach for Bass Mono Freq/Low Width specifically when you hear the low end losing focus or translating poorly in mono as you push Width up.
- **A/B in mono, always.** Solo the bus, flip your DAW's mono/downmix monitoring on, and confirm nothing phases out oddly - Firmament's own Width/Low Width/Auto Mono Safety path is provably mono-safe by construction (see `docs/architecture.md`), but Haas Mode deliberately is not, and even a mono-safe width setting can still expose problems that were already latent in the source material's own stereo image.
- **200% Width is a special-effect setting, not a default.** On a whole mix or a loud bus it reads as artificial/phasey quickly; it's much more useful reached for briefly (automated for a chorus lift, a breakdown, a single pad layer) than left engaged throughout.
- **Auto Mono Safety is a safety net, not a substitute for listening.** It reacts to the correlation of the plugin's *input*, with meter-style (200 ms) ballistics - fast enough to catch a sustained phase problem, not fast enough (nor intended) to police fast transients sample-by-sample.
- **Haas Mode and Bass Mono/Low Width combine fine**, but think about what each is doing: Bass Mono/Low Width shapes width *within* the Mid/Side model (provably mono-safe), Haas Mode is a separate, later, non-Mid/Side effect. If you need guaranteed mono compatibility (broadcast delivery, a club system with an unpredictable mono/summed sub), leave Haas Mode off and rely on Width/Low Width/Auto Mono Safety alone.
- **On a mono-input track/bus**, Width/Low Width/Auto Mono Safety have nothing to act on (there is no Side signal) - the plugin passes the source through cleanly. Haas Mode is the one control that still does something in that situation, since it operates after the (now-identical) L/R pair has been decoded.

## Roadmap / what's not here yet

Firmament's GUI is still the plain, functional v0.1-style slider/toggle editor - every parameter above is fully controllable from the plugin's own window (and from any host's generic editor/automation lanes), but there is no custom-drawn interface yet, and the correlation/phase estimate that drives Auto Mono Safety is not yet displayed as a visible meter (the DSP value is fully computed and tested - see `docs/architecture.md` - only the visual widget is outstanding). Both are tracked for a later milestone (custom GUI + metering). Preset management (factory presets, browsing) is likewise a later milestone; for now, save/recall works via your host's own plugin-state save mechanism.
