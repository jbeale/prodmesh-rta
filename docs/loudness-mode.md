# Acoustic / Program modes — design plan (v0.7.0, v0.8.0)

Status: v0.7.0 implemented on `feature/loudness-mode`; v0.8.0 not started.

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

Qt captures from *input* devices, and the stream bus is not one.

- **macOS**: BlackHole (2ch) as OBS's Monitoring Device, sources set to
  "Monitor and Output". Appears as a normal input. Still trips the mic TCC
  prompt.
- **Windows**: Qt6 `QAudioSource` enumerates capture endpoints, not WASAPI
  loopback — use VB-Audio Virtual Cable (or Stereo Mix) as the OBS monitor
  target.

Tapping OBS's monitor path is correct: post-fader, post-filter, and it matches
what gets encoded.

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

# v0.8.0 — stereo tools + stream QC

## Stereo tab (Program mode only)

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

## Stream QC

- **Silence / dropout detection** — both channels below a floor for N seconds
  raises an alarm on the dashboard and over the API. For an unattended stream
  box this is arguably worth more than any loudness metric: stream-is-dead is
  the failure that costs the whole service.
- **True-peak over-count log** — timestamped events above the ceiling, so
  there is a QC record after the service.
- **LRA** (EBU Tech 3342) — short-term distribution, absolute gate −70 LUFS
  and relative gate −20 LU, then the 10th–95th percentile spread in LU.
- Loudness history graph with the target line; dashboard tiles; CSV columns.

## Selftests

Correlation → +1 for identical channels, −1 for inverted; mono-compat delta
strongly negative for an inverted pair; silence detector fires at the
configured horizon.
