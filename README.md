# ProdMesh Remote RTA

A minimal, free, cross-platform (Windows / macOS) SPL meter and spectrum
analyzer, part of the ProdMesh production toolkit: point it at any microphone
input and get

- **SPL meter** — Fast (125 ms), Slow (1 s), and Leq (average since reset),
  with **A / C / Z** frequency weighting
- **RTA** — 31-band 1/3-octave real-time spectrum, 20 Hz – 20 kHz, with
  selectable averaging and peak hold
- Input device picker, clip indicator, calibration offset

The C++ version additionally has:

- **Spectrogram** — scrolling 30 s log-frequency heat map (tab next to RTA)
- **SPL history strip** — the last 10 minutes of Fast/Slow at a glance
- **Persistent settings** — cal, weighting, device, averaging, API config,
  and window geometry survive restarts
- **HTTP + WebSocket API** — JSON endpoints other machines can poll, plus a
  live push stream (see below); configured from **Settings → API & Streaming**

Two equivalent implementations live in this repo:

- **Python** — `rta.py` (PySide6 + sounddevice + NumPy). Zero build step.
- **C++** — `src/main.cpp` (pure Qt 6 Widgets + Multimedia, no other
  dependencies; FFT included). Compiles to a native binary with CMake.

## Running the Python version

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

## Building the C++ version

### The easy way

- **Windows**: install Qt from [qt.io](https://www.qt.io/download-qt-installer)
  (Qt 6.x Desktop with the **MinGW** kit, **Qt Multimedia** under Additional
  Libraries, and CMake/Ninja/MinGW under Build Tools), then double-click
  **`build.bat`**. It finds Qt automatically and leaves a self-contained
  `build\` folder — `ProdMeshRemoteRTA.exe` runs on any Windows PC.
- **macOS**: `chmod +x build.sh && ./build.sh` — installs qt/cmake/ninja via
  Homebrew if needed and produces `build/ProdMeshRemoteRTA.app` with the Qt
  frameworks bundled.

### By hand

Needs CMake 3.16+, a C++17 compiler, and Qt 6 (Widgets + Multimedia + Network).

### Windows (MSYS2/MinGW)

```bash
pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
          mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-multimedia
cmake -B build -G Ninja
cmake --build build
./build/ProdMeshRemoteRTA.exe
```

With the official Qt installer (Qt 6.x + MinGW kit):

```
cmake -B build -G Ninja -DCMAKE_PREFIX_PATH=C:\Qt\6.11.1\mingw_64
cmake --build build
```

With MSVC + the official Qt installer instead:
`cmake -B build -DCMAKE_PREFIX_PATH=C:\Qt\6.x.x\msvc2022_64 && cmake --build build --config Release`

To make a self-contained folder you can copy to another PC, run
`windeployqt build/ProdMeshRemoteRTA.exe`.

### macOS

```bash
brew install qt cmake ninja
cmake -B build -G Ninja
cmake --build build
open build/ProdMeshRemoteRTA.app
```

The app bundle includes the microphone-permission string, so macOS will
prompt on first launch. `macdeployqt build/ProdMeshRemoteRTA.app` makes it
portable.

`--selftest` runs the same DSP check as the Python version (on Windows the
output only appears when redirected:
`ProdMeshRemoteRTA.exe --selftest > out.txt`).

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

Mics are not calibrated out of the box, so absolute dB SPL is only as good as
the **Cal** offset (displayed level = dBFS + Cal). In the C++ app use
**Settings → Calibrate SPL…**: put the mic on a calibrator (or play steady
pink noise measured by a meter/app you trust), enter that reference level,
press **Capture** — the app averages ~1.5 s and computes the offset for you.
The offset stays valid for that mic *at that preamp/input-gain setting*;
change the gain and you must recalibrate. Uncalibrated, the numbers are still
perfectly usable as relative measurements.

If you have a Smaart rig calibrated on the same mic/interface/gain, its
dBFS→SPL offset is conceptually the same number — but verify side-by-side
once, since different driver paths can shift full-scale by a fixed dB.

### Mic correction files (C++ version)

**Settings → Load Mic Correction…** accepts standard measurement-mic
calibration text files (REW / miniDSP UMIK style): lines of
`<frequency Hz> <response dB>`, whitespace- or comma-separated; comment and
header lines are skipped. The response curve is interpolated log-frequency
and **subtracted** from the spectrum (the usual convention — the file
describes the mic's deviation from flat). Applies to the SPL readouts, RTA,
spectrogram, and everything served over the API; the loaded file persists
across restarts and is reported in `/api/status` as `mic_correction`.

## HTTP + WebSocket API (C++ version)

Enable it under **Settings → API & Streaming…** (or launch with
`ProdMeshRemoteRTA --api 8517`). The URL in the status bar is reachable from
any machine on the LAN — allow the app through the firewall when Windows
asks. All endpoints are read-only GETs returning JSON with
`Access-Control-Allow-Origin: *`:

| Endpoint | Returns |
|---|---|
| `/api/status` | sample rate, weighting, cal, uptime, history length |
| `/api/spl` | current `fast_db`, `slow_db`, `leq_db` (dB SPL, cal applied) |
| `/api/rta` | `centers_hz` + `bands_db` (31 values) + `peaks_db` |
| `/api/history?since_ms=&limit=` | 1 Hz SPL samples, up to 6 hours |
| `ws://…/api/stream` | WebSocket: pushes SPL + bands at the configured rate |

### Live streaming

Connect a WebSocket to `/api/stream` on the same port and you'll receive a
`{"type":"levels", …}` message (same fields as `/api/spl` + `/api/rta`) at
the stream rate chosen in Settings (1/5/10/20 Hz, default 10):

```js
// Node.js 21+ / browsers (Node <21: npm i ws, then `new (require("ws"))(url)`)
const ws = new WebSocket("ws://192.168.1.18:8517/api/stream");
ws.onmessage = (ev) => {
  const m = JSON.parse(ev.data);
  console.log(m.fast_db, m.slow_db, m.bands_db);
};
```

`/api/history` is designed for logging a whole event with cheap incremental
polls — pass the timestamp of the last sample you already have:

```js
// Node.js: collect SPL over the course of a service
const BASE = "http://192.168.1.18:8517";   // shown in the RTA app
let since = 0;
setInterval(async () => {
  const { samples } = await (
    await fetch(`${BASE}/api/history?since_ms=${since}`)
  ).json();
  if (samples.length) {
    since = samples.at(-1).t;
    for (const s of samples) {
      // s = { t: epoch ms, fast_db, slow_db, leq_db }
      store(s);
    }
  }
}, 30_000);  // any interval ≤ 6 h works; history survives between polls
```

Levels are `null` in JSON until the input has data. History is in-memory and
clears when the app closes.

## Troubleshooting

- **Levels drop to nothing a few seconds after you stop talking** — that's a
  noise gate / noise suppression applied by the OS or audio driver, not the
  app. On Windows: Settings → System → Sound → your microphone → Advanced →
  turn **Audio enhancements** off (on some Realtek systems it's in the
  Realtek Audio Console instead). For measurement use you want every
  "enhancement" (noise suppression, AGC, echo cancellation) disabled.
- **Meter pinned near the floor (~0–5 dB)** — wrong input selected (e.g. an
  unconnected line-in jack) or the mic's input gain is near zero in the OS
  sound settings.

## License

This project's code is MIT-licensed (see [LICENSE](LICENSE)). It dynamically
links Qt (and, for the Python version, PySide6), which are used under the
terms of the **LGPLv3** — the Qt libraries are shipped as separate,
replaceable DLLs/frameworks and remain under their own license. Qt source is
available at <https://code.qt.io>. Do not switch to a statically linked Qt
build without revisiting LGPL compliance.

## Notes / limits

- Levels are computed from a 16k FFT (Hann window); Fast/Slow are exponential
  time weightings applied to the band-limited (20 Hz – 20 kHz) power, so the
  ballistics closely track a real meter but this is not a Class 1 instrument.
- Below ~50 Hz the 1/3-octave bands are narrower than the FFT resolution at
  44.1/48 kHz; those bands are estimated from spectral density and are
  correspondingly coarser.
- A quick DSP sanity check is built in: `python rta.py --selftest` verifies a
  full-scale 1 kHz sine reads −3.01 dBFS in the 1 kHz band.
