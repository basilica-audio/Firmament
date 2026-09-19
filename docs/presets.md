# Factory presets

Thirteen factory presets ship with Firmament, embedded via BinaryData from
`presets/factory/*.json` (see `basilica-audio/nave`'s
`docs/preset-system-notes.md` for the shared build wiring this repo copies).
Most are sourced starting points from `docs/design-brief.md`'s "Factory
Presets" section - see that document's own Honesty section for what these
numbers are and aren't calibrated against (research/forum/manual-derived, not
measured against any commercial widener's actual audio output). **Default** is
this plugin's own certified passthrough state.

| Preset | Category | Intent |
|---|---|---|
| **Default** | Init | The certified passthrough state (every parameter at its off/default position - Width 100%, Bass Mono off, Auto Mono Safety/Multiband/Decorrelate/Haas all off), exposed as an explicit preset so there's always a one-click way back to "no coloration." Also this plugin's out-of-the-box default (see the M2 default-resolution order in `basilica-audio/nave`'s `docs/preset-system-notes.md`). |
| **Open Strings** | Bus | The primary "widen the orchestral/choir bus, keep the low end centered" default - Width 140%, Bass Mono Freq 110 Hz (the researched 80-180 Hz consensus center), Auto Mono Safety on. |
| **Choir Bloom** | Bus | A wider, more exposed setting (Width 170%) with a little low-end air (Low Width 15%, not full mono) and a firmer -12 dB safety floor to match. |
| **Doubled Rhythm Glue** | Bus | The manual's own documented "110-140%" doubled-guitar-bus use case (Width 125%) - manual Auto Mono Safety off, for predictable, already-checked material. |
| **Master Bus Bass Mono** | Master | Width 100% (pass-through above the crossover) with Bass Mono Freq 90 Hz and Auto Mono Safety Multiband on - protects the wide highs without dulling the already-centered, safe low end (v0.2.0's headline multiband behaviour). |
| **Automated Width Safety Net** | Bus | For busses with automated/unpredictable Width where nobody is constantly monitoring - Auto Mono Safety on at its default -9.1 dB floor (the v0.1.1-reproducing value). |
| **Mono-Safe Air** | FX | Width from near-mono source material (a mono-tracked lead, a narrow pad) via the v0.2.0 Decorrelate technique at a subtle 35% amount - the lower-cost, gentler alternative to Haas Mode for this use case. |
| **Wide Pad, Full Precedence** | FX | The strong, well-known precedence-effect widening technique (Haas Mode on, Haas Time 22 ms, centrally in the researched 10-30 ms "Haas Window") for material where mono translation is a secondary concern. |
| **Extreme Width** | FX | The manual's own documented "200% is a special-effect setting, not a default" guidance, packaged with a firm -15 dB safety net rather than left unprotected. |
| **Subtle Openness** | Bus | A barely-there width lift (115%) for sources that just need a hint of stereo interest without drawing attention to the processing. |
| **Mastering Linear Phase Bass Mono** | Master | Width 100% (untouched - Width plays no role here) with Bass Mono Freq 120 Hz and Bass Mono Mode set to Linear Phase - the v0.3.0 zero-phase-rotation crossover reserved for mastering/render passes rather than live tracking, since it reports 2048 samples of latency at 48 kHz. Auto Mono Safety on at its default -9.1 dB floor, single-band rather than Multiband. For final bass-mono duties where phase purity through the crossover matters more than a live, latency-free toggle. |
| **Three Band Imager** | Widening | A narrow-to-wide curve across all three v0.3.0 bands - Low Width 40% below the 110 Hz Bass Mono Freq, Width 110% in the mid band, High Width 140% above the 2500 Hz High Split - voiced through the Phase Matched Bass Mono Mode, Multiband Auto Mono Safety and the faster Dynamic safety response. The showcase for the plugin's new three-band architecture. |
| **Velvet Width** | Widening | A moderate Width 120% lift across the full spectrum (no Bass Mono split engaged) layered with 60% Velvet Dense Decorrelate - the v0.3.0 mono-sum-safe decorrelation mode - plus Width Compensation on so the extra width doesn't also add level, and the faster Dynamic safety response. The showcase for Velvet decorrelation as a fully mono-safe, general-purpose alternative to Classic Decorrelate. |

None of the presets touch Decorrelate and Haas Mode at the same time (they
are mutually exclusive by design - see `docs/design-brief.md`); "Mono-Safe
Air", "Wide Pad, Full Precedence" and "Velvet Width" each showcase one of the
two alternative-widening techniques on its own - Mono-Safe Air and Velvet
Width via Classic and Velvet Dense Decorrelate respectively, Wide Pad, Full
Precedence via Haas Mode.
