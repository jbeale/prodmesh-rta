#!/usr/bin/env python3
"""
RTA — a minimal cross-platform (Windows/macOS) SPL meter and 1/3-octave
real-time analyzer. Point it at any microphone input.

SPL:  Fast (125 ms), Slow (1 s), Leq (average since reset), A/C/Z weighting.
RTA:  31-band 1/3-octave spectrum, 20 Hz - 20 kHz, averaging + peak hold.

Levels are relative (dBFS + calibration offset). To calibrate, expose the mic
to a known level (e.g. a 94 dB calibrator or a meter you trust) and adjust
"Cal" until the readout matches.
"""

import sys

import numpy as np
import sounddevice as sd
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QColor, QFont, QPainter, QPen
from PySide6.QtWidgets import (
    QApplication,
    QCheckBox,
    QComboBox,
    QDoubleSpinBox,
    QHBoxLayout,
    QLabel,
    QMainWindow,
    QPushButton,
    QVBoxLayout,
    QWidget,
)

FFT_SIZE = 16384
UPDATE_MS = 50

THIRD_OCT_CENTERS = [
    20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
    630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
    10000, 12500, 16000, 20000,
]
X_AXIS_LABELS = {31.5: "31", 63: "63", 125: "125", 250: "250", 500: "500",
                 1000: "1k", 2000: "2k", 4000: "4k", 8000: "8k", 16000: "16k"}

FLOOR_DB = -200.0


def a_weight_db(f):
    f = np.maximum(np.asarray(f, dtype=np.float64), 1e-6)
    f2 = f * f
    num = (12194.0 ** 2) * f2 * f2
    den = ((f2 + 20.6 ** 2)
           * np.sqrt((f2 + 107.7 ** 2) * (f2 + 737.9 ** 2))
           * (f2 + 12194.0 ** 2))
    return 20.0 * np.log10(num / den) + 2.00


def c_weight_db(f):
    f = np.maximum(np.asarray(f, dtype=np.float64), 1e-6)
    f2 = f * f
    num = (12194.0 ** 2) * f2
    den = (f2 + 20.6 ** 2) * (f2 + 12194.0 ** 2)
    return 20.0 * np.log10(num / den) + 0.06


class AudioEngine:
    """Captures mic input into a ring buffer from the PortAudio callback thread."""

    def __init__(self):
        import threading
        self.lock = threading.Lock()
        self.buf = np.zeros(FFT_SIZE * 4, dtype=np.float32)
        self.pos = 0
        self.total = 0
        self.peak = 0.0
        self.stream = None
        self.samplerate = 48000

    def _callback(self, indata, frames, time_info, status):
        mono = indata.mean(axis=1) if indata.shape[1] > 1 else indata[:, 0]
        with self.lock:
            n = len(mono)
            p = self.pos
            end = p + n
            if end <= len(self.buf):
                self.buf[p:end] = mono
            else:
                k = len(self.buf) - p
                self.buf[p:] = mono[:k]
                self.buf[: n - k] = mono[k:]
            self.pos = end % len(self.buf)
            self.total += n
            if n:
                self.peak = max(self.peak, float(np.max(np.abs(mono))))

    def latest(self, n):
        """Return (most recent n samples or None, peak since last call)."""
        with self.lock:
            p = self.pos
            filled = min(self.total, len(self.buf))
            pk = self.peak
            self.peak = 0.0
        if filled < n:
            return None, pk
        start = (p - n) % len(self.buf)
        if start < p:
            return self.buf[start:p].copy(), pk
        return np.concatenate([self.buf[start:], self.buf[:p]]), pk

    def start(self, device_index):
        self.stop()
        info = sd.query_devices(device_index)
        sr = int(info["default_samplerate"])
        with self.lock:
            self.buf[:] = 0
            self.pos = 0
            self.total = 0
            self.peak = 0.0
        try:
            self.stream = sd.InputStream(
                device=device_index, channels=1, samplerate=sr,
                dtype="float32", callback=self._callback)
            self.stream.start()
        except Exception:
            ch = max(1, min(2, int(info["max_input_channels"])))
            self.stream = sd.InputStream(
                device=device_index, channels=ch, samplerate=sr,
                dtype="float32", callback=self._callback)
            self.stream.start()
        self.samplerate = sr

    def stop(self):
        if self.stream is not None:
            try:
                self.stream.stop()
                self.stream.close()
            except Exception:
                pass
            self.stream = None


class Analyzer:
    """FFT-based SPL + 1/3-octave band analysis. All levels in dBFS;
    the GUI adds the calibration offset."""

    def __init__(self):
        self.window = np.hanning(FFT_SIZE)
        self.win_norm = FFT_SIZE * float(np.sum(self.window ** 2))
        self._cache_key = None
        self.sr = 48000
        self.weighting = "A"
        self.reset_all()

    def reset_all(self):
        self.fast_p = None
        self.slow_p = None
        self.reset_leq()
        self.band_p = None
        self.band_peak_db = None

    def reset_leq(self):
        self.leq_sum = 0.0
        self.leq_n = 0

    def _prepare(self):
        key = (self.sr, self.weighting)
        if key == self._cache_key:
            return
        self._cache_key = key
        freqs = np.fft.rfftfreq(FFT_SIZE, 1.0 / self.sr)
        self.freqs = freqs
        if self.weighting == "A":
            wdb = a_weight_db(freqs)
        elif self.weighting == "C":
            wdb = c_weight_db(freqs)
        else:
            wdb = np.zeros_like(freqs)
        self.wlin = 10.0 ** (wdb / 10.0)
        self.spl_mask = (freqs >= 20.0) & (freqs <= 20000.0)
        df = freqs[1] - freqs[0]
        nyq = self.sr / 2.0
        self.bands = []  # (indices or None, fallback_bin, fallback_factor)
        for c in THIRD_OCT_CENTERS:
            lo, hi = c * 2 ** (-1 / 6), c * 2 ** (1 / 6)
            if c > nyq:
                self.bands.append(None)
                continue
            idx = np.nonzero((freqs >= lo) & (freqs < hi))[0]
            if len(idx):
                self.bands.append((idx, None, None))
            else:
                near = int(np.argmin(np.abs(freqs - c)))
                self.bands.append((None, near, (hi - lo) / df))
        # Reset smoothing state when the spectrum layout changes.
        self.band_p = None
        self.band_peak_db = None
        self.fast_p = None
        self.slow_p = None

    def process(self, x, dt, rta_tau, peak_hold):
        """x: FFT_SIZE samples. Returns dict of dBFS levels."""
        self._prepare()
        X = np.fft.rfft(x * self.window)
        p = np.abs(X) ** 2
        p[1:-1] *= 2.0
        p /= self.win_norm  # per-bin power; sums to the signal's mean square
        pw = p * self.wlin

        # Broadband SPL (20 Hz - 20 kHz), exponential time weighting on power
        P = float(np.sum(pw[self.spl_mask]))
        a_fast = np.exp(-dt / 0.125)
        a_slow = np.exp(-dt / 1.0)
        self.fast_p = P if self.fast_p is None else a_fast * self.fast_p + (1 - a_fast) * P
        self.slow_p = P if self.slow_p is None else a_slow * self.slow_p + (1 - a_slow) * P
        self.leq_sum += P
        self.leq_n += 1

        # 1/3-octave bands
        band_P = np.full(len(THIRD_OCT_CENTERS), np.nan)
        for i, b in enumerate(self.bands):
            if b is None:
                continue
            idx, near, factor = b
            if idx is not None:
                band_P[i] = float(np.sum(pw[idx]))
            else:
                band_P[i] = float(pw[near]) * factor
        if rta_tau and self.band_p is not None:
            a = np.exp(-dt / rta_tau)
            valid = ~np.isnan(band_P)
            sm = self.band_p.copy()
            sm[valid] = a * sm[valid] + (1 - a) * band_P[valid]
            self.band_p = sm
        else:
            self.band_p = band_P.copy()

        def db(v):
            return 10.0 * np.log10(np.maximum(v, 1e-20))

        band_db = np.where(np.isnan(self.band_p), np.nan,
                           db(np.nan_to_num(self.band_p, nan=1e-20)))
        if peak_hold:
            decay = 6.0 * dt  # dB per tick at 6 dB/s
            if self.band_peak_db is None:
                self.band_peak_db = band_db.copy()
            else:
                held = self.band_peak_db - decay
                self.band_peak_db = np.fmax(held, band_db)
        else:
            self.band_peak_db = None

        return {
            "fast": db(self.fast_p),
            "slow": db(self.slow_p),
            "leq": db(self.leq_sum / max(self.leq_n, 1)),
            "bands": band_db,
            "peaks": None if self.band_peak_db is None else self.band_peak_db.copy(),
        }


class RtaWidget(QWidget):
    BG = QColor("#14161c")
    GRID = QColor("#2a2e39")
    TEXT = QColor("#8a92a6")
    BAR = QColor("#2fbf9b")
    BAR_TOP = QColor("#5ce0bd")
    PEAK = QColor("#e0c05c")

    def __init__(self):
        super().__init__()
        self.setMinimumHeight(280)
        self.band_db = None
        self.peak_db = None
        self.y_min = 20.0
        self.y_max = 120.0

    def set_data(self, band_db, peak_db, y_min, y_max):
        self.band_db = band_db
        self.peak_db = peak_db
        self.y_min = y_min
        self.y_max = y_max
        self.update()

    def paintEvent(self, event):
        qp = QPainter(self)
        qp.fillRect(self.rect(), self.BG)
        left, right, top, bottom = 40, 10, 10, 24
        w = self.width() - left - right
        h = self.height() - top - bottom
        if w <= 10 or h <= 10:
            return
        span = self.y_max - self.y_min

        def y_of(db_val):
            return top + h * (1.0 - (db_val - self.y_min) / span)

        qp.setFont(QFont(self.font().family(), 8))
        step = 10
        g = int(np.ceil(self.y_min / step)) * step
        while g <= self.y_max:
            y = y_of(g)
            qp.setPen(QPen(self.GRID, 1))
            qp.drawLine(left, int(y), left + w, int(y))
            qp.setPen(self.TEXT)
            qp.drawText(0, int(y) - 8, left - 6, 16,
                        Qt.AlignRight | Qt.AlignVCenter, f"{g:.0f}")
            g += step

        n = len(THIRD_OCT_CENTERS)
        slot = w / n
        bar_w = max(2.0, slot - 2.0)
        for i, c in enumerate(THIRD_OCT_CENTERS):
            x0 = left + i * slot + (slot - bar_w) / 2
            if c in X_AXIS_LABELS:
                qp.setPen(self.TEXT)
                qp.drawText(int(left + i * slot - slot), self.height() - bottom + 4,
                            int(slot * 3), 16, Qt.AlignHCenter, X_AXIS_LABELS[c])
            if self.band_db is None or np.isnan(self.band_db[i]):
                continue
            v = min(max(self.band_db[i], self.y_min), self.y_max)
            y = y_of(v)
            qp.setPen(Qt.NoPen)
            qp.setBrush(self.BAR)
            qp.drawRect(int(x0), int(y), int(bar_w), int(top + h - y))
            qp.setBrush(self.BAR_TOP)
            qp.drawRect(int(x0), int(y), int(bar_w), 2)
            if self.peak_db is not None and not np.isnan(self.peak_db[i]):
                pv = min(max(self.peak_db[i], self.y_min), self.y_max)
                py = y_of(pv)
                qp.setPen(QPen(self.PEAK, 2))
                qp.drawLine(int(x0), int(py), int(x0 + bar_w), int(py))

        qp.setPen(QPen(self.GRID, 1))
        qp.setBrush(Qt.NoBrush)
        qp.drawRect(left, top, w, h)


class SplReadout(QWidget):
    def __init__(self, caption):
        super().__init__()
        lay = QVBoxLayout(self)
        lay.setContentsMargins(12, 4, 12, 4)
        lay.setSpacing(0)
        self.cap = QLabel(caption)
        self.cap.setStyleSheet("color:#8a92a6; font-size:11px;")
        self.val = QLabel("--.-")
        f = QFont()
        f.setFamilies(["Consolas", "Menlo", "Courier New"])
        f.setPointSize(28)
        f.setBold(True)
        self.val.setFont(f)
        self.val.setStyleSheet("color:#e8ecf4;")
        self.val.setMinimumWidth(120)
        lay.addWidget(self.cap)
        lay.addWidget(self.val)

    def set(self, caption, value_db):
        self.cap.setText(caption)
        self.val.setText("--.-" if value_db is None else f"{value_db:5.1f}")


class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        self.setWindowTitle("RTA — SPL Meter & Spectrum Analyzer")
        self.engine = AudioEngine()
        self.analyzer = Analyzer()
        self.clip_ticks = 0

        central = QWidget()
        self.setCentralWidget(central)
        root = QVBoxLayout(central)
        central.setStyleSheet("background:#1a1d24; color:#c8cede;")

        # --- controls row ---
        ctl = QHBoxLayout()
        ctl.addWidget(QLabel("Input:"))
        self.device_combo = QComboBox()
        self.device_combo.setMinimumWidth(280)
        ctl.addWidget(self.device_combo)
        ctl.addSpacing(12)
        ctl.addWidget(QLabel("Weighting:"))
        self.weight_combo = QComboBox()
        self.weight_combo.addItems(["A", "C", "Z"])
        ctl.addWidget(self.weight_combo)
        ctl.addSpacing(12)
        ctl.addWidget(QLabel("RTA avg:"))
        self.avg_combo = QComboBox()
        self.avg_combo.addItems(["Off", "Fast (125 ms)", "Slow (1 s)", "2 s"])
        self.avg_combo.setCurrentIndex(1)
        ctl.addWidget(self.avg_combo)
        ctl.addSpacing(12)
        self.peak_check = QCheckBox("Peak hold")
        ctl.addWidget(self.peak_check)
        ctl.addSpacing(12)
        ctl.addWidget(QLabel("Cal:"))
        self.cal_spin = QDoubleSpinBox()
        self.cal_spin.setRange(0.0, 200.0)
        self.cal_spin.setDecimals(1)
        self.cal_spin.setSingleStep(0.5)
        self.cal_spin.setValue(100.0)
        self.cal_spin.setSuffix(" dB")
        self.cal_spin.setToolTip(
            "Calibration offset added to dBFS.\nPlay a known level (e.g. a 94 dB "
            "calibrator) and adjust until the meter matches.")
        ctl.addWidget(self.cal_spin)
        ctl.addStretch(1)
        self.reset_btn = QPushButton("Reset Leq/Peaks")
        ctl.addWidget(self.reset_btn)
        root.addLayout(ctl)

        # --- SPL readouts ---
        spl_row = QHBoxLayout()
        self.r_fast = SplReadout("LAF (Fast)")
        self.r_slow = SplReadout("LAS (Slow)")
        self.r_leq = SplReadout("LAeq")
        for r in (self.r_fast, self.r_slow, self.r_leq):
            spl_row.addWidget(r)
        self.clip_lbl = QLabel("CLIP")
        self.clip_lbl.setStyleSheet(
            "color:#3a3f4d; font-weight:bold; font-size:16px; padding:0 12px;")
        spl_row.addWidget(self.clip_lbl)
        spl_row.addStretch(1)
        root.addLayout(spl_row)

        # --- RTA ---
        self.rta = RtaWidget()
        root.addWidget(self.rta, 1)

        self.status = self.statusBar()
        self.status.setStyleSheet("color:#8a92a6;")

        # --- wiring ---
        self.devices = []  # combo row -> sd device index
        self.populate_devices()
        self.device_combo.currentIndexChanged.connect(self.change_device)
        self.weight_combo.currentTextChanged.connect(self.change_weighting)
        self.reset_btn.clicked.connect(self.do_reset)

        self.timer = QTimer(self)
        self.timer.setInterval(UPDATE_MS)
        self.timer.timeout.connect(self.tick)
        self.timer.start()

        self.change_device(self.device_combo.currentIndex())

    def populate_devices(self):
        self.device_combo.blockSignals(True)
        self.device_combo.clear()
        self.devices = []
        default_in = sd.default.device[0]
        select_row = 0
        for i, d in enumerate(sd.query_devices()):
            if d["max_input_channels"] <= 0:
                continue
            api = sd.query_hostapis(d["hostapi"])["name"]
            if "WDM-KS" in api:  # PortAudio can't open these on most systems
                continue
            self.device_combo.addItem(f"{d['name']}  [{api}]")
            if i == default_in:
                select_row = len(self.devices)
            self.devices.append(i)
        self.device_combo.setCurrentIndex(select_row)
        self.device_combo.blockSignals(False)

    def change_device(self, row):
        if row < 0 or row >= len(self.devices):
            return
        try:
            self.engine.start(self.devices[row])
            self.analyzer.sr = self.engine.samplerate
            self.analyzer.reset_all()
            self.status.showMessage(
                f"Listening at {self.engine.samplerate} Hz — "
                f"FFT {FFT_SIZE} ({FFT_SIZE / self.engine.samplerate * 1000:.0f} ms window)")
        except Exception as e:
            self.status.showMessage(f"Could not open input: {e}")

    def change_weighting(self, w):
        self.analyzer.weighting = w
        self.analyzer.reset_all()

    def do_reset(self):
        self.analyzer.reset_leq()
        self.analyzer.band_peak_db = None

    def rta_tau(self):
        return [None, 0.125, 1.0, 2.0][self.avg_combo.currentIndex()]

    def tick(self):
        x, pk = self.engine.latest(FFT_SIZE)
        if pk >= 0.99:
            self.clip_ticks = 20
        if self.clip_ticks > 0:
            self.clip_ticks -= 1
            self.clip_lbl.setStyleSheet(
                "color:#e05c5c; font-weight:bold; font-size:16px; padding:0 12px;")
        else:
            self.clip_lbl.setStyleSheet(
                "color:#3a3f4d; font-weight:bold; font-size:16px; padding:0 12px;")
        if x is None:
            return
        dt = UPDATE_MS / 1000.0
        res = self.analyzer.process(x, dt, self.rta_tau(),
                                    self.peak_check.isChecked())
        cal = self.cal_spin.value()
        w = self.weight_combo.currentText()
        self.r_fast.set(f"L{w}F (Fast)", res["fast"] + cal)
        self.r_slow.set(f"L{w}S (Slow)", res["slow"] + cal)
        self.r_leq.set(f"L{w}eq", res["leq"] + cal)
        bands = res["bands"] + cal
        peaks = None if res["peaks"] is None else res["peaks"] + cal
        self.rta.set_data(bands, peaks, cal - 80.0, cal + 20.0)

    def closeEvent(self, event):
        self.timer.stop()
        self.engine.stop()
        super().closeEvent(event)


def selftest():
    """Verify DSP math with a synthetic full-scale 1 kHz sine (no audio, no GUI)."""
    an = Analyzer()
    an.sr = 48000
    an.weighting = "A"
    t = np.arange(FFT_SIZE) / an.sr
    x = np.sin(2 * np.pi * 1000.0 * t).astype(np.float32)
    res = None
    for _ in range(100):  # let Fast/Slow converge
        res = an.process(x, 0.05, 0.125, True)
    # Full-scale sine: mean square 0.5 -> -3.01 dBFS; A-weight at 1 kHz = 0 dB.
    print(f"fast  = {res['fast']:7.2f} dBFS (expected ~ -3.01)")
    print(f"slow  = {res['slow']:7.2f} dBFS (expected ~ -3.01)")
    print(f"leq   = {res['leq']:7.2f} dBFS (expected ~ -3.01)")
    b = res["bands"]
    k = int(np.nanargmax(b))
    print(f"loudest band = {THIRD_OCT_CENTERS[k]} Hz at {b[k]:.2f} dBFS "
          f"(expected 1000 Hz ~ -3.01)")
    others = np.nansum(10 ** (np.delete(b, k) / 10))
    print(f"energy outside 1 kHz band = {10 * np.log10(max(others, 1e-30)):.1f} dBFS "
          f"(expected far below -3)")
    ok = (abs(res["fast"] + 3.01) < 0.2 and THIRD_OCT_CENTERS[k] == 1000
          and abs(b[k] + 3.01) < 0.3)
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    app = QApplication(sys.argv)
    win = MainWindow()
    win.resize(1000, 560)
    win.show()
    sys.exit(app.exec())


if __name__ == "__main__":
    main()
