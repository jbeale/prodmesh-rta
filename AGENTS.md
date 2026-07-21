# ProdMesh Remote RTA — contributor / agent guide

A Qt 6 C++ SPL meter + 1/3-octave RTA for live sound (Windows/macOS), with a
spectrogram, Smaart-style SPL metrics, an HTTP/WebSocket API, and a built-in
web dashboard. `rta.py` is the original minimal Python version — new features
go in the C++ app only; keep the Python version untouched unless fixing a bug.

## Architecture (header-only, no .cpp except main)

| File | Contents |
|---|---|
| `src/dsp.h` | FFT, A/C/Z weighting, `Analyzer` (bands + hi-res spectrum), `CWeightFilter` |
| `src/audio.h` | `AudioEngine`: QAudioSource capture ring, channel select, peak tracking |
| `src/metrics.h` | `MetricsEngine`: rolling Leq, percentiles, dose (pure math, no Qt GUI) |
| `src/widgets.h` | All custom views: RTA, spectrogram, history, readouts, breakout window |
| `src/api.h` | `ApiServer`: hand-rolled HTTP + WebSocket (RFC 6455) on QTcpServer |
| `src/webui.h` | The dashboard page as one inline HTML string (self-contained, no CDNs) |
| `src/main.cpp` | `MainWindow`, dialogs, settings persistence, the 50 ms tick loop |

Data flow, once per 50 ms tick: `AudioEngine::latest()` → `Analyzer::process()`
→ `MetricsEngine::push()/values()` → widgets + `ApiServer::setSnapshot()`.

To add a metric: `MetricValues` + `MetricsEngine` (metrics.h), then
`kMetricInfos`, `metricCaption`, `metricValue`, `metricSuffix` (main.cpp),
and the id lists in `webui.h`. Everything else (settings dialog, breakout,
CSV log, API) picks it up from the registry.

## Build & test

- macOS: `./build.sh` (handles everything, see gotchas below). Windows:
  `build.bat`, or see README for manual CMake.
- **Always run the selftest after DSP/metrics changes**:
  `./build/ProdMeshRemoteRTA.app/Contents/MacOS/ProdMeshRemoteRTA --selftest`
  (must print `PASS`; CI runs it on both platforms and it gates releases).
- Run the app with `--api 8600` to test the API/dashboard headlessly:
  `curl localhost:8600/api/spl`, dashboard at `http://localhost:8600/`.

## Gotchas (each of these has burned real time)

- **clangd/IDE diagnostics in this repo are false positives** (Qt include
  paths unresolved — "QString unknown" etc.). The compiler via build.sh is
  the arbiter; a clean build + selftest PASS means the code is fine.
- **macOS bundles**: CMake writes `Info.plist` at *configure* time — any
  `rm -rf` of the .app must happen before `cmake -B`, or the bundle ships
  without a plist. Never `cmake --build` into an already-macdeployqt'd
  bundle (two Qt copies → startup crash). After macdeployqt, always ad-hoc
  re-sign or Apple Silicon SIGKILLs the app. `build.sh` encodes all three;
  macdeployqt's "ERROR: codesign verification error" line is harmless noise.
- **Qt stylesheets**: do NOT add QSS box rules (background/border) to
  QComboBox/QSpinBox — any box rule suppresses Fusion's native arrow glyphs
  and the dropdowns render broken. Style comes from Fusion + the dark
  QPalette + the single `APP_STYLE` sheet in main.cpp.
- **macOS mic permission**: launch via `open build/ProdMeshRemoteRTA.app`
  (running the binary from some shells breaks TCC attribution). Ad-hoc
  signatures change per rebuild and can re-trigger the permission prompt.
  If the API port accepts TCP but HTTP times out, the main thread is almost
  certainly blocked on a pending mic-permission dialog.
- CI pins Qt **6.8 on Windows** and **6.11 on macOS** — the proven-green
  combination (6.8's macOS binaries link the removed AGL framework). Keep
  `build.yml` and `release.yml` in sync when touching either.

## Conventions

- C++17, Qt-idiomatic; 4-space indent, ~80-col lines, sparse comments that
  explain *why* (constraints, units, standards), not what.
- New user-facing options go in **Settings menu dialogs**, not extra inline
  controls crammed into the main window (owner's explicit preference).
- Theme colors live in the `theme` namespace (widgets.h) and are mirrored in
  `webui.h` CSS variables and `assets/make_icon.py` — keep them in sync.
- All levels are dBFS internally; the cal offset (dB) is added at display /
  metrics / API boundaries. History-based metrics bake cal in at
  accumulation time.
- The API is read-only GET + WS push, CORS `*`, no auth — it must stay
  side-effect-free. The dashboard must remain self-contained (CSP-less
  closed show networks; no external resources).

## Releases

Bump `VERSION` in CMakeLists.txt, commit, then `git tag vX.Y.Z && git push
origin vX.Y.Z`. The release workflow verifies the tag matches the CMake
version, builds, selftests, and publishes zips. Hyphenated tags (v1.0-rc1)
publish as pre-releases. Plain pushes never release.
