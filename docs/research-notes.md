# Firmament — Research Notes (deep-dive for design brief v2)

Category: Mid/Side stereo widener + bass-mono utility + Haas-style secondary widening +
correlation/mono-safety net. Reference class searched: Waves S1 Stereo Imager (Shuffle/Widener),
iZotope Ozone Imager (Stereoize decorrelation modes), Brainworx bx_digital/bx_control (Mono
Maker), classic vinyl-cutting elliptical EQ practice, KERN WIDE / HoRNet ZeroWidth / In The Mix
Bandwidth (per-band correlation-safe wideners), Softube Widener, plus room-acoustics-adjacent
correlation-meter and precedence-effect (Haas) literature.

## 1. Bass-mono crossover frequency convention

- Vinyl-cutting origin: "Mono'ing the bass comes from vinyl cutting as you had devices
  (elliptical eqs) that would mono the bass under a selected frequency... required as vinyl
  cutting lathes don't like bass that is out of phase as it makes the stylus jump out of the
  groove." — [Streaky Mastering, Facebook](https://www.facebook.com/StreakyMastering/photos/monoing-the-bass-comes-from-vinyl-cutting-as-you-had-devices-elliptical-eqs-that/2612930892091464/)
- "Up to 300Hz is a very safe figure... For most amateurs keeping everything from at least
  0-150Hz mono will make a record 'punch' more... Typically the hardware EQ units found in
  cutting chains have a gentle slope of 6dB and cut anywhere from 100Hz upwards to 200Hz,
  depending on the material." — via search synthesis of vinyl-mastering sources
  ([Mobineko](https://blog.mobineko.com/2014/03/31/vinyl-mastering-notes/), [KA-Electronics](https://www.ka-electronics.com/kaelectronics/Elliptic_EQ/Elliptic_EQ.htm))
- Modern mastering consensus (searched cluster of Gearspace/SoundOnSound/indierecordingdepot
  threads): "typical threshold... is 100Hz... Most bass management subs tend to crossover
  around 80Hz... A common approach uses 6 or 12 dB/oct rolloff at 100 Hz... Sometimes 150 Hz is
  necessary... some engineers use around 120Hz... 125 Hz using the steepest slope possible...
  collapsing to mono from about 80hz-100hz seems to be a consensus... At the vinyl cutting era
  the usual crossover frequency was even 200 Hz, but with modern plugins at 200 Hz the stereo
  image is mostly too much influenced in a negative way." — search synthesis, [SoundOnSound: How do you make only low frequencies mono?](https://www.soundonsound.com/sound-advice/q-how-do-you-make-only-low-frequencies-mono)
- Flotown Mastering, "Center That Sub!": recommends checking width **below about 200 Hz**
  for vinyl-destined material; primary rationale is playback reality, not cutting risk: "the
  number of playback systems out there with true stereo sub-bass reproduction is fairly
  limited, so if low frequencies are going to be reproduced in a predominantly mono fashion
  anyhow, why not take control of them at the source." Practical implementation recommended:
  "a linear phase high-pass filter on the side channel." — [flotownmastering.com/blog/center-that-sub](https://flotownmastering.com/blog/center-that-sub)
- Brainworx bx_digital/bx_control "Mono Maker": commonly cited default/typical working range
  **160-180 Hz** (search-synthesized from Brainworx/Plugin-Alliance manual references and
  third-party summaries; primary manual PDF is image-based, not machine-readable — see Honesty
  section below for how this is classed).
- SoundOnSound, "How do you make only low frequencies mono?": confirms the mechanism directly —
  "In fact, the vast majority of devices which have a 'make the bass mono' mode actually
  achieve it in exactly this way internally, with simple M-S processing," equivalent to
  high-passing the Side channel at the chosen frequency/slope. — [soundonsound.com/sound-advice/q-how-do-you-make-only-low-frequencies-mono](https://www.soundonsound.com/sound-advice/q-how-do-you-make-only-low-frequencies-mono)

**Synthesis**: the researched consensus center is roughly **80-180 Hz**, with 200 Hz as a
legacy vinyl-era outer bound now considered too aggressive by modern engineers. Firmament's
existing 0-500 Hz range (0 = off) and manual copy ("Typical settings sit in the 80-200 Hz
range") already sits inside this consensus — this is a **confirmation, not a gap**.

## 2. Filter topology / crossover order

- "Today, the de facto standard for professional audio active crossovers is the 4th-order
  Linkwitz-Riley (LR-4) design. Offering in-phase outputs and steep 24 dB/octave slopes, the
  LR-4 alignment gives users the necessary tool to scale the next step toward the elusive goal
  of perfect sound." — [Sweetwater InSync, "Linkwitz-Riley"](https://www.sweetwater.com/insync/linkwitz-riley/)
- "Each order... increases the slopes by 6 dB/octave... 4th-order Linkwitz-Riley: 24 dB/octave
  (4 x 6 dB/octave)... The crossover point is located at -6 dB and the summed response is
  perfectly flat." — [Rane, "Linkwitz-Riley Crossovers: A Primer"](https://www.ranecommercial.com/legacy/note160.html)

**Synthesis**: Firmament's `juce::dsp::LinkwitzRileyFilter` (JUCE 8.0.14) is a fixed 4th-order
(24 dB/oct) LR design per JUCE's own documentation, already cited in `docs/architecture.md`.
This matches the researched professional-standard crossover order exactly — **confirmation,
not a gap**.

## 3. Haas Mode / precedence effect — timing window

- "A range of 5 to 35 ms is ideal for the Haas effect... A single reflection arriving within
  5 to 35 ms can be up to 10 dB louder than the direct sound without being perceived as a
  secondary auditory event (echo). Around 30 ms is known as the threshold of echo perception."
  — [Gearank, "The Helmut Haas Effect from 1949"](https://gearank.com/haas-effect)
- "If we hear two sounds within 40 milliseconds of one another, our ears interpret them as
  being the same sound... Usually somewhere between 5 and 35 ms works best." Percussive sounds
  want shorter delays; pianos/guitars want longer delays within the window. —
  [mastering.com, "The Haas Effect: The Pro's Secret for Super Wide Mixes"](https://mastering.com/the-haas-effect/)
- "The time window between 10 and 30 ms is referred to as the Haas Window, which represents the
  range where the precedence effect optimally creates a wide stereo image without phasing
  issues." — search synthesis (Gearank/QSC/mastering.com cluster)

**Synthesis**: Firmament's Haas Time range (0-40 ms) and default (20 ms) both sit exactly
inside the researched 5-35/10-30 ms window and respect the ~40 ms echo-perception ceiling —
**confirmation, not a gap**.

## 4. Haas delay vs. allpass decorrelation — mono-fold-down damage (the headline finding)

- "Comb filtering occurs when the original and delayed sounds combine in a way that certain
  frequencies reinforce each other while others cancel out... When the original signal and the
  delayed signal are combined into a mono source, certain frequencies reinforce each other
  while others cancel out." — [KERN Audio, "How to widen stereo without phase issues"](https://kernaudio.io/guides/stereo/stereo-without-phase-issues)
- "Although Haas delays often sound very impressive when heard in stereo, the parts on which
  they're used will disappear or, at best, change in tone and level when mixed to mono, due to
  phase cancellation... certain frequencies arrive in phase (they add) and others arrive out of
  phase (they cancel). The result on mono fold-down is comb filtering: deep notches at regular
  frequency intervals, spaced at 1/delay Hz apart." — [SoundOnSound, "Can Haas delays be mono-compatible?"](https://www.soundonsound.com/sound-advice/q-can-haas-delays-be-mono-compatible) / search synthesis
- "Allpass decorrelation shifts the phase of different frequencies by different amounts without
  changing the magnitude. The mono sum produces partial cancellation and partial reinforcement,
  but because the phase shifts are spread irregularly across the spectrum, the result is mild
  spectral ripple rather than deep periodic notches. Typically less than 1 to 2 dB of
  variation... Allpass decorrelation preserves magnitude spectrum with mild spectral ripple on
  mono fold-down (1 to 2 dB), while Haas delay creates dramatic width but has terrible mono
  compatibility with deep comb filter notches on every mono device." — [KERN Audio, "How to widen stereo without phase issues"](https://kernaudio.io/guides/stereo/stereo-without-phase-issues)
- iZotope Ozone Imager's "Stereoize" module (generates width from mono sources) ships **two**
  modes for exactly this reason: "Mode I uses Haas-style delay, Mode II uses velvet noise
  decorrelation" — search synthesis of Ozone Imager documentation summaries.

**Synthesis**: this is the strongest, most concretely-sourced finding of the whole deep-dive.
Firmament's Haas Mode is honestly documented as trading away the mono-sum guarantee (good), but
v1 offers **no alternative** widening technique that keeps most of that guarantee. The
researched reference class (Ozone Imager's dual Stereoize modes, and the general allpass-
decorrelation literature) treats "delay-based width" and "decorrelation-based width" as two
genuinely different tools with different mono-fold-down cost, not one technique with two knobs.

## 5. Correlation / phase-correlation meter — scale and ballistics

- "If the two inputs are identical (a dual-mono signal) they are said to be 'fully correlated'
  and the meter indicates +1"; "average phase difference... 90 degrees, and the cosine is
  therefore zero" for decorrelated signals; "in the case of a dual-mono signal where one
  channel is polarity-inverted, the phase difference will be precisely 180-degrees, and as the
  cosine is -1 the meter will indicate at the negative extreme." — [SoundOnSound, "What are my phase-correlation meters telling me?"](https://www.soundonsound.com/sound-advice/q-what-are-my-phase-correlation-meters-telling-me)
- "The meter's integration time is usually fairly long, typically around **600 milliseconds**,
  to ensure a relatively gentle and steady meter movement." — same source
- "Any amount of 'out-of-phasiness' will kick the meter into its negative half, between 0 and
  -1, and while **the occasional small deviation into the negative side is usually
  insignificant**, any steady reading in the negative half indicates a reduced degree of
  mono-compatibility: something will get lost (or attenuated) when you listen in mono!" — same
  source

**Synthesis**: Firmament's correlation estimate already uses the same `[-1, 1]` scale with the
same documented meaning (+1 in-phase, 0 uncorrelated, -1 out-of-phase) — confirmed, no change.
But two concrete gaps against the sourced convention:
1. **Ballistics**: Firmament's `correlationTimeConstantSeconds = 0.2` (200 ms) is notably
   faster/twitchier than the ~600 ms typical professional correlation-meter integration time
   documented above.
2. **Reactivity threshold**: Firmament's Auto Mono Safety starts attenuating the instant
   correlation goes below exactly 0 (`correlationEstimate >= 0.0f` is the only bypass
   condition), whereas the sourced guidance explicitly treats "occasional small deviation into
   the negative side" as insignificant and not something a safety net should react to.

## 6. Per-band correlation safety (multiband precedent)

- KERN WIDE: "monitors the interaural cross-correlation in all 40 ERB bands in real time and
  prevents any individual band from going below a safe threshold... offers allpass
  decorrelation with a bass crossover and correlation safety." — search synthesis of KERN Audio
  product documentation
- In The Mix "Bandwidth": "multiband approach allows you to keep the bass mono, widen the mids
  moderately, and push the highs wider, all from one plugin. A correlation meter shows your
  mono safety in real time." — search synthesis
- HoRNet ZeroWidth: "seven multi-band bands... per-band correlation learning... auto Side EQ,
  auto Width." — [hornetplugins.com/plugins/hornet-zerowidth](https://www.hornetplugins.com/plugins/hornet-zerowidth/)

**Synthesis**: the researched reference class treats mono-safety as fundamentally a **per-band**
problem (a narrowband phase issue in the highs shouldn't dull an already-safe low band, and
vice versa) rather than one global broadband number. Firmament's Auto Mono Safety is currently
single-band/broadband even when the bass-mono crossover is already splitting the signal into
low/high bands for Width purposes — a clear, sourced architectural gap. No source publishes
exact ERB-band-count or threshold numbers for a generic 2-band (not 7- or 40-band) design, so
the *principle* (per-band beats broadband) is sourced; the exact 2-band mapping onto Firmament's
existing crossover is reasoned.

## 7. Width curve / range convention (Waves S1 Shuffle)

- "S1 has a very useful Shuffle control that lets you further increase stereo width at lower
  frequencies (typically **below 600Hz**, as set by the associated Frequency control). This
  compensates for the fact that the ear is less sensitive to stereo bass effects." Waves S1
  ships two modes: "the Shuffler targets low-frequency stereo behavior (based on the BBC's
  stereo shuffling technique), while the Widener controls overall width via M/S processing." —
  search synthesis of Waves S1 documentation/reviews (primary waves.com product page carries no
  numeric spec sheet, confirmed by direct fetch — see Honesty section)

**Synthesis**: Firmament's Width/Low Width (0-200%) and Bass Mono Freq (0-500 Hz) already
implement the same "boost low-frequency stereo independently, because the ear is less sensitive
to stereo bass" principle Waves S1's Shuffle control is built on — architecturally confirmed.
One minor, reasoned gap: Waves' documented Shuffle crossover convention centers around **600
Hz**, a bit above Firmament's current 500 Hz range ceiling; this is a small, low-confidence
signal to widen the range ceiling, not a strong finding (single indirect source, not a primary
manual).

## 8. Auto-safety threshold/floor conventions (general)

Multiple reference wideners (KERN WIDE, HoRNet ZeroWidth, In The Mix Bandwidth) implement
*some* form of correlation-driven automatic safety, confirming Firmament's Auto Mono Safety is
a legitimate, well-precedented category feature — not a v1 invention needing justification.
None of the searched sources publish an exact numeric floor-gain value (e.g. Firmament's
hardcoded 0.35 linear / -9.1 dB), confirming this specific number is, and remains, a
**reasoned** internal choice rather than a sourced one.

## Honesty / sourcing caveats for this research pass

- The Brainworx bx_digital V2 manual PDF (`media.uaudio.com/assetlibrary/b/x/bx_digital_v2_manual.pdf`)
  is image-scanned, not machine-readable text; its "Mono Maker 160-180 Hz" figure above is a
  **search-engine synthesis of third-party summaries**, not a direct primary-source quote, and
  is treated as lower-confidence corroboration alongside the (higher-confidence, multi-source)
  general mastering-forum consensus in Section 1.
- waves.com's own S1 Stereo Imager product page (fetched directly) contains no numeric spec
  sheet; the Shuffle/600 Hz figures in Section 7 come from third-party review/summary sources
  found via search, not the Waves manual PDF itself — treated as the weakest-confidence finding
  in this pass and flagged accordingly in the brief.
- All other findings (Sections 1-6, 8) are corroborated across multiple independent sources
  (SoundOnSound direct quotes, KERN Audio direct quotes, Rane/Sweetwater direct quotes,
  Gearank/mastering.com direct quotes) and are treated as directly sourced.
