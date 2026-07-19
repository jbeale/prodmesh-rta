# RTA — SPL Meter & Spectrum Analyzer

A minimal, free, cross-platform (Windows / macOS) alternative to a paid RTA app:
point it at any microphone input and get

- **SPL meter** — Fast (125 ms), Slow (1 s), and Leq (average since reset),
  with **A / C / Z** frequency weighting
- **RTA** — 31-band 1/3-octave real-time spectrum, 20 Hz – 20 kHz, with
  selectable averaging and peak hold
- Input device picker, clip indicator, calibration offset

Built with Qt (PySide6) + PortAudio (sounddevice) + NumPy. One file: `rta.py`.

## Running it

### Windows

Double-click **`run.bat`** — it creates a local virtual environment, installs
the three dependencies, and starts the app. Needs Python 3.10+ installed
(from [python.org](https://www.python.org/downloads/) or
`winget install Python.Python.3.12`).

### macOS

```bash
chmod +x run.command      # once
./run.command             # or double-click it in Finder
```

Needs Python 3 (`brew install python` or from python.org). The first launch
will trigger the macOS microphone-permission prompt — grant it to Terminal
(or whatever launched the app) in
System Settings → Privacy & Security → Microphone.

### Manually (either OS)

```bash
python -m venv .venv
.venv/bin/pip install -r requirements.txt      # Windows: .venv\Scripts\pip
.venv/bin/python rta.py                        # Windows: .venv\Scripts\python
```

## Using it

- **Input** — pick your microphone. On Windows the same physical mic shows up
  under several host APIs; the **[Windows WASAPI]** entry usually gives the
  native sample rate (48 kHz) and lowest latency.
- **Weighting** — A (default, matches most SPL specs), C, or Z (flat).
  Applies to both the SPL readouts and the RTA display; the readout labels
  follow (LAF/LCF/LZF, etc.).
- **RTA avg** — smoothing for the spectrum bars. **Peak hold** overlays a
  slowly decaying max line per band.
- **Reset Leq/Peaks** — restarts the Leq average and clears peak hold.
- **CLIP** lights red when the input is within ~0.1 dB of full scale.

### Calibration

Computer mics are not calibrated, so absolute dB SPL is only as good as the
**Cal** offset (displayed level = dBFS + Cal). To calibrate: put a known level
at the mic — a 94 dB calibrator, or side-by-side with a meter/app you trust —
and adjust **Cal** until the readout matches. That offset stays valid for that
mic at that input-gain setting. Uncalibrated, the numbers are still perfectly
usable as *relative* measurements.

## Notes / limits

- Levels are computed from a 16k FFT (Hann window); Fast/Slow are exponential
  time weightings applied to the band-limited (20 Hz – 20 kHz) power, so the
  ballistics closely track a real meter but this is not a Class 1 instrument.
- Below ~50 Hz the 1/3-octave bands are narrower than the FFT resolution at
  44.1/48 kHz; those bands are estimated from spectral density and are
  correspondingly coarser.
- A quick DSP sanity check is built in: `python rta.py --selftest` verifies a
  full-scale 1 kHz sine reads −3.01 dBFS in the 1 kHz band.
