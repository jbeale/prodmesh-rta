# Program mode — design plan and roadmap (v0.7.0 → v0.9.0)

Status: **v0.7.0 shipped** (tagged, signed and notarised). v0.8.0 not started.

Roadmap at a glance:

- **v0.7.0** — Acoustic/Program modes, BS.1770 loudness, true peak. *Done.*
- **v0.8.0** — silence/dropout detection, stereo tools, LRA and QC logging.
- **v0.9.0** — per-device "will this survive the listener's playback" status.
- *Separate package* — `prodmesh-rta-capture`, the VST3/AU insert plugin.

## Why

The app measures a *room* through a microphone: every metric is anchored to
20 µPa via the cal offset, and Leq / L10 / dose are hearing-exposure numbers.
On a livestream machine there is no acoustic domain — you are metering a
digital bus, there is nothing to calibrate against, and OSHA dose on a YouTube
feed is meaningless.

Digital audio already has an absolute reference: full scale. That is what
LUFS is built on (ITU-R BS.1770-4 / EBU R128). So the stream side wants a
different metric set on the same DSP core.

## The mode

One top-level mode, because the differences are coupled, not independent:

| | Acoustic | Program |
|---|---|---|
| Reference | 20 µPa via cal offset | digital full scale (no cal) |
| Weighting | A / C / Z selectable | K (mandated) for loudness; A/C/Z still drives the RTA *display* |
| Metrics | Leq, L10/L50/L90, C-A, dose | M / S / Integrated LUFS, dBTP, PLR, (LRA) |
| Channels | pick one, or mix | stereo pair |
| Axis labels | dB SPL | dBFS |
| Tabs | RTA · Spectrogram · Split | **Loudness** · RTA · Spectrogram · Split · (**Stereo**) |

Naming: **Acoustic** / **Program**. "Program" is the broadcast term for the
mix bus and covers metering a console feed even when nothing is streaming.

Placement: `Settings → Input & Mode…`, next to the device picker — consistent
with the house rule that options live in dialogs, and it means nobody flips it
by brushing a toolbar mid-service. The active mode is shown in the window
title so it is never ambiguous.

Settings are **namespaced per mode** for the keys that genuinely differ
(selected metrics, breakout tiles, metric targets, alarms, input channel,
active tab). Acoustic keeps the existing un-prefixed keys so current users'
settings survive with no migration; Program-mode keys get a `program/` prefix.

The *device* is deliberately **not** mode-scoped: switching mode should never
silently change which input is open mid-service, and in practice the two modes
run on different machines anyway.

## Capture path (operational, not code)

Qt captures from *input* devices, and the programme bus is not one.

- **macOS**: BlackHole (2ch), standalone or inside an Aggregate Device so the
  operator can monitor and meter at once. Appears as a normal input; still
  trips the mic TCC prompt.
- **Windows**: Qt6 `QAudioSource` enumerates capture endpoints, not WASAPI
  loopback — VB-Audio Virtual Cable, or Stereo Mix where the interface has it.

Measure as late in the chain as possible (post master fader, post bus
processing) so the numbers match what gets encoded.

The intended long-term route is a **ProdMesh insert plugin (VST3/AU)** that
sends the bus straight to the app, removing the loopback-device step
altogether. Not built yet, and it lives in a separate package — do not
document it as available. See "Out of scope for this repo" at the end.

---

# v0.7.0 — foundation + loudness core

## 1. Multichannel ring (`audio.h`)

Capture is *already* multichannel — `start()` deliberately does not force
mono. The collapse to one channel happens at **write** time in `onReady()`.
The change is to store all channels interleaved and collapse at **read** time:

- `m_buf` becomes interleaved, `m_pos` counts frames.
- `latest(n, out, …)` de-interleaves the selected channel (or mixes).
- `latest(ch, n, out)` for a specific channel (0.8.0 stereo tools).
- Peak tracking stays on the selected channel (unchanged CLIP/LZpk meaning).

This keeps Acoustic mode on the *same* code path rather than a parallel one,
which is what protects the mode already in production.

## 2. K-weighting + true peak (`dsp.h`)

- `Biquad` — transposed direct form II, so both stages share one struct.
- `KWeightFilter` — BS.1770 stage 1 high shelf
  (f0 1681.974450955533 Hz, G 3.999843853973347 dB, Q 0.7071752369554196) and
  stage 2 RLB high-pass (f0 38.13547087602444 Hz, Q 0.5003270373238773),
  designed by bilinear transform at the actual sample rate rather than
  hardcoding the published 48 kHz table. Selftest compares the designed
  coefficients against that table.
- `TruePeakDetector` — 4× polyphase interpolator, windowed-sinc designed at
  runtime (cutoff at the original Nyquist, Blackman window). BS.1770 requires
  ≥4× oversampling; a hardcoded table transcription is a worse risk than a
  designed filter we can test.

## 3. `LoudnessEngine` (`metrics.h`)

Consumes gapless 100 ms sub-blocks of per-channel K-weighted mean square.

- **Momentary** = 400 ms sliding (4 sub-blocks), ungated.
- **Short-term** = 3 s sliding (30 sub-blocks), ungated.
- **Integrated** = gated over 400 ms blocks at 100 ms hop (75 % overlap):
  absolute gate −70 LUFS, then relative gate 10 LU below the mean of the
  blocks that passed the absolute gate. Implemented with a 0.1 LU histogram
  from −70 to +10 holding count + summed power per bin, so the threshold
  comparison is quantized but the final mean is exact over the selected set.
- Loudness of a block = `−0.691 + 10·log10(Σ Gᵢ·zᵢ)`, Gᵢ = 1.0 for L/R.
- **True peak** — session max and per-tick value, dBTP.
- **PLR** = dBTP − Integrated.

Scope call: loudness is computed over the **selected stereo pair** (or the
single channel on a mono device), which is exactly BS.1770 for stereo. Full
surround weighting (Ls/Rs at 1.41) is out of scope — not a use case here, and
it bounds the per-sample filter cost at two channels.

## 4. Mode plumbing (`main.cpp`)

- `MetricInfo` gains a `modes` bitmask (1 = acoustic, 2 = program) so the
  single registry drives both modes; everything that iterates it (Metrics
  dialog, Alarms dialog, CSV header/rows, API snapshot) filters by mode.
- New ids: `lufsM`, `lufsS`, `lufsI`, `dbtp`, `dbtpMax`, `plr`, `toTarget`.
- `laf` / `las` / `leq` / `lzpk` stay in both modes (they are just level; with
  cal forced to 0 they read dBFS). Percentiles, C-A and dose are acoustic-only.
- Cal is forced to 0.0 in Program mode and the control is hidden.
- Target presets: YouTube/Spotify/Twitch −14, Apple Podcasts −16, EBU R128
  −23, ATSC A/85 −24, Custom. Ceiling default −1 dBTP.
- The bottom history strip carries short-term LUFS in Program mode.

## 5. `LoudnessMeter` (`widgets.h`)

Large Integrated readout, M and S bars on a LUFS scale, shaded target zone,
true-peak indicator with over-count. Theme colors from the `theme` namespace.

## 6. API + dashboard

`mode` field in the snapshot, loudness block in the JSON, dashboard tiles that
switch on mode. Stays read-only GET + WS push, no auth, self-contained page.

## 7. Selftests (gate the release)

- Full-scale 1 kHz sine, mono → **−3.01 LUFS**.
- Designed K-weight coefficients vs. the published 48 kHz table.
- Gating: alternating loud/silent must ignore the silence (absolute gate) and
  a −30 LUFS passage must not drag down a −14 LUFS programme by more than the
  relative gate allows.
- True peak: sine at fs/4 phase-shifted 45° has sample peak −3.01 dBFS and
  true peak ≈ 0 dBFS.

---

# v0.8.0 — stream QC + stereo tools

Build order: silence/dropout first (small, highest operational value, and it
defines the `signal` + event plumbing everything else reuses), then the stereo
tab, then the remaining QC items.

## 1. Silence / dropout detection — **implemented**

`SignalMonitor` (metrics.h) fed by `AudioEngine`'s per-tick monitor peak,
which covers the analysis channel in Acoustic and the stereo pair in Program
— so one live channel of a pair still counts as audio.

The design driver: **on an unattended stream box nobody is looking at the
app.** The in-app banner is the least valuable channel; the API push is the
most.

Corollary — **the app cannot report its own death.** If the process crashes or
the NIC drops, the silence alarm goes quiet too, which looks identical to
"everything is fine". So it is two layers:

- the app detects silence and reports it;
- the *consumer* detects the app going away, by watching `time_ms` go stale.
  Every payload already carries it, so this needs no code — but it does need
  documenting as the intended pattern.

Two failure modes, deliberately separated because they mean different things:

| | Trigger | Meaning | Default horizon |
|---|---|---|---|
| **Digital black** | samples exactly 0 | the route is dead | ~1 s |
| **Silence** | below a set floor (e.g. −60 dBFS) | source muted, fader down, wrong scene | 15–20 s |

The silence horizon must default *generous*: a pause between songs, or after
"let's pray", is not a failure. Digital black needs no threshold and can fire
fast.

Transport — both, because they answer different questions:

- a `signal` object in every payload (`state`, `silent_for_s`, `last_audio_ms`)
  so any poller sees current state;
- a distinct WS message `{"type":"event","event":"silence_start"|"silence_end"}`
  so ProdMesh gets an edge without diffing a 10 Hz stream.

Presentation: **solid, not flashing** — flashing reads as decorative and is
accessibility-hostile. Full-width red banner with a running timer
(`NO AUDIO — 47s`), because "how long" is the operational question. Same on the
dashboard, plus rewriting the browser tab title so a minimised dashboard on a
phone still shows it.

**Audible alerts default OFF**: on a stream machine, system sound can land back
in the capture path and go out over the broadcast.

Applies to both modes — a dead mic matters in Acoustic too.

## 2. Stereo tab (Program mode only)

Ranked by what they actually catch:

1. **Correlation meter** (−1…+1) — catches a polarity-flipped channel, which
   sounds fine in the room and *disappears* for mono listeners (phone
   speakers, i.e. most of the audience). Warn on sustained negative.
2. **Goniometer** — L/R rotated 45° so mid is vertical: mono reads as a
   vertical line, out-of-phase as horizontal, one-sided content leans.
   Decimated scatter with persistence. Reads from `latest(ch, …)`.
3. **Mono-compatibility delta** — sum-to-mono loudness minus stereo loudness,
   in LU. More actionable than correlation because it is in the same units as
   the target.
4. **L/R balance** — running level difference.
5. **Per-channel true peak** — catches one-sided clipping.
6. Mid/Side RTA — deliberately deferred; easy to clutter the RTA for
   marginal gain.

## 3. Remaining QC

- **True-peak over-count log** — timestamped events above the ceiling, so
  there is a QC record after the service.
- **LRA** (EBU Tech 3342) — short-term distribution, absolute gate −70 LUFS
  and relative gate −20 LU, then the 10th–95th percentile spread in LU.
  Deferred from 0.7.0: needs an ungated short-term history and a different
  gate from the integrated one.
- Loudness history graph with the target line; dashboard tiles; CSV columns.

## Selftests

Correlation → +1 for identical channels, −1 for inverted; mono-compat delta
strongly negative for an inverted pair; silence detector fires at the
configured horizon and digital black fires faster.

---

# v0.9.0 — "will this survive the listener's device?"

Translate measurements an operator cannot interpret ("correlation 0.3, LRA
14 LU") into the question they actually have: *will this sound OK in a car?*
For a volunteer running a church stream, that translation is the whole value.

Depends on 0.8.0 (correlation, mono-compat, LRA), which is why it sits here.

## The four devices, and why each is a real failure mode

| Device | Actual failure mode | Measured by |
|---|---|---|
| **Phone** | one speaker (mono), nothing below ~500 Hz | mono-compat delta, correlation |
| **TV** | tiny down-firing speakers; dialogue intelligibility | mono-compat, 1–4 kHz balance |
| **Car** | ~70 dB of road noise, mostly LF — quiet passages vanish | LRA |
| **Headphones** | full range, true stereo — exposes everything | correlation, noise floor, true peak |

Phones and TVs are both effectively **mono**, which is why mono-compatibility
carries most of the weight: it is the failure that is inaudible in the room
and affects the largest share of the audience.

## Presentation: per-device status, NOT a letter grade

A–F was considered and **rejected**:

1. **False authority** — a big "C" looks objective, but the weighting (how
   much a 60 Hz rolloff "costs" on a phone) is pure judgement. Two engineers
   would pick different weights and the letter hides that.
2. **Not actionable** — it says something is wrong but not what, and invites
   chasing the letter by re-EQing a mix that was fine.

Instead: compact green / amber / red per device, expanding to the reason.
Same at-a-glance property, no false precision.

```
Phone  ⚠  loses 6 LU summed to mono
Car    ⚠  14 LU range — quiet passages lost under road noise
TV     ✓
Phones ✓
```

## Starter rule set

Each rule maps to a defensible physical cause. Resist adding rules that need
taste to justify.

| Rule | Threshold | Affects |
|---|---|---|
| mono-compat loss | > 3 LU | phone, TV |
| correlation, sustained | < 0 | all |
| LRA | > 12 LU | car |
| integrated vs target | > +2 LU | all (platform turns it down) |
| true peak vs ceiling | over | all (encoder distortion) |

**Deliberately held back**: spectral-balance rules (e.g. penalising LF
rolloff). A sermon and a worship set legitimately have different spectra, and
a rule that marks spoken word down for lacking bass is wrong. Add these only
after checking against real mixes from the venue.

Thresholds above are a starting point, not calibrated truth — they want a
pass against real services before being treated as settled.

---

# Out of scope for this repo

**Insert plugin** (VST3/AU) — the intended long-term route for getting the bus
into the app without a loopback device. It belongs in a **separate package**
(working name `prodmesh-rta-capture`): it needs a new build system (JUCE or
the bare SDKs), a plugin↔app transport, and signing/notarisation for two more
binary formats. Keeping it out preserves this repo's "nothing beyond Qt"
property and keeps the release pipeline simple.
