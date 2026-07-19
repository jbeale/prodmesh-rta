// RTA — a minimal cross-platform (Windows/macOS) SPL meter and 1/3-octave
// real-time analyzer. C++/Qt port of rta.py.
//
// SPL:  Fast (125 ms), Slow (1 s), Leq (average since reset), A/C/Z weighting.
// RTA:  31-band 1/3-octave spectrum, 20 Hz - 20 kHz, averaging + peak hold.
//
// Levels are relative (dBFS + calibration offset). To calibrate, expose the
// mic to a known level (e.g. a 94 dB calibrator or a meter you trust) and
// adjust "Cal" until the readout matches.

#include <QApplication>
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QIODevice>
#include <QLabel>
#include <QMainWindow>
#include <QMediaDevices>
#include <QMutex>
#include <QMutexLocker>
#include <QPainter>
#include <QPushButton>
#include <QStatusBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <complex>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static constexpr int FFT_SIZE = 16384;
static constexpr int UPDATE_MS = 50;
static constexpr double kPi = 3.14159265358979323846;
static const double kNaN = std::numeric_limits<double>::quiet_NaN();

static const double THIRD_OCT_CENTERS[] = {
    20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
    630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
    10000, 12500, 16000, 20000,
};
static constexpr int NUM_BANDS = int(sizeof(THIRD_OCT_CENTERS) / sizeof(double));

static QString xAxisLabel(double center) {
    if (center == 31.5) return "31";
    if (center == 63) return "63";
    if (center == 125) return "125";
    if (center == 250) return "250";
    if (center == 500) return "500";
    if (center == 1000) return "1k";
    if (center == 2000) return "2k";
    if (center == 4000) return "4k";
    if (center == 8000) return "8k";
    if (center == 16000) return "16k";
    return QString();
}

static double aWeightDb(double f) {
    f = std::max(f, 1e-6);
    const double f2 = f * f;
    const double num = (12194.0 * 12194.0) * f2 * f2;
    const double den = (f2 + 20.6 * 20.6)
                       * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9))
                       * (f2 + 12194.0 * 12194.0);
    return 20.0 * std::log10(num / den) + 2.00;
}

static double cWeightDb(double f) {
    f = std::max(f, 1e-6);
    const double f2 = f * f;
    const double num = (12194.0 * 12194.0) * f2;
    const double den = (f2 + 20.6 * 20.6) * (f2 + 12194.0 * 12194.0);
    return 20.0 * std::log10(num / den) + 0.06;
}

// In-place iterative radix-2 FFT (n must be a power of two).
static void fft(std::vector<std::complex<double>> &a) {
    const size_t n = a.size();
    for (size_t i = 1, j = 0; i < n; i++) {
        size_t bit = n >> 1;
        for (; j & bit; bit >>= 1)
            j ^= bit;
        j |= bit;
        if (i < j)
            std::swap(a[i], a[j]);
    }
    for (size_t len = 2; len <= n; len <<= 1) {
        const double ang = -2.0 * kPi / double(len);
        const std::complex<double> wlen(std::cos(ang), std::sin(ang));
        for (size_t i = 0; i < n; i += len) {
            std::complex<double> w(1.0, 0.0);
            for (size_t k = 0; k < len / 2; k++) {
                const auto u = a[i + k];
                const auto v = a[i + k + len / 2] * w;
                a[i + k] = u + v;
                a[i + k + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

static double toDb(double power) {
    return 10.0 * std::log10(std::max(power, 1e-20));
}

// ---------------------------------------------------------------------------
// Audio capture into a ring buffer.

class AudioEngine {
public:
    AudioEngine() : m_buf(FFT_SIZE * 4, 0.0f) {}
    ~AudioEngine() { stop(); }

    void start(const QAudioDevice &dev) {
        stop();
        QAudioFormat fmt = dev.preferredFormat();
        QAudioFormat want = fmt;
        want.setChannelCount(1);
        want.setSampleFormat(QAudioFormat::Float);
        if (dev.isFormatSupported(want))
            fmt = want;
        m_format = fmt;
        {
            QMutexLocker lock(&m_mutex);
            std::fill(m_buf.begin(), m_buf.end(), 0.0f);
            m_pos = 0;
            m_total = 0;
            m_peak = 0.0f;
        }
        m_source = new QAudioSource(dev, fmt);
        m_io = m_source->start();
        if (!m_io) {
            delete m_source;
            m_source = nullptr;
            throw std::runtime_error("could not open audio input");
        }
        QObject::connect(m_io, &QIODevice::readyRead, m_source,
                         [this] { onReady(); });
    }

    void stop() {
        if (m_source) {
            m_source->stop();
            delete m_source;
            m_source = nullptr;
            m_io = nullptr;
        }
    }

    int sampleRate() const { return m_format.sampleRate(); }

    // Fills `out` with the most recent n samples. Returns false while the
    // ring buffer is still filling. `peakOut` is the peak since last call.
    bool latest(int n, std::vector<float> &out, float &peakOut) {
        QMutexLocker lock(&m_mutex);
        peakOut = m_peak;
        m_peak = 0.0f;
        const qint64 filled = std::min<qint64>(m_total, qint64(m_buf.size()));
        if (filled < n)
            return false;
        out.resize(n);
        const int size = int(m_buf.size());
        int start = int((m_pos - n) % size);
        if (start < 0)
            start += size;
        if (start < m_pos) {
            std::copy(m_buf.begin() + start, m_buf.begin() + m_pos, out.begin());
        } else {
            const int tail = size - start;
            std::copy(m_buf.begin() + start, m_buf.end(), out.begin());
            std::copy(m_buf.begin(), m_buf.begin() + m_pos, out.begin() + tail);
        }
        return true;
    }

private:
    void onReady() {
        if (!m_io)
            return;
        const QByteArray data = m_io->readAll();
        const int ch = m_format.channelCount();
        const int bps = m_format.bytesPerSample();
        if (ch <= 0 || bps <= 0)
            return;
        const int frames = int(data.size()) / (ch * bps);
        if (frames <= 0)
            return;
        m_conv.resize(frames);
        const char *p = data.constData();
        for (int i = 0; i < frames; ++i) {
            double acc = 0.0;
            for (int c = 0; c < ch; ++c) {
                const char *sp = p + (size_t(i) * ch + c) * bps;
                double v = 0.0;
                switch (m_format.sampleFormat()) {
                case QAudioFormat::Float: {
                    float f;
                    std::memcpy(&f, sp, 4);
                    v = f;
                    break;
                }
                case QAudioFormat::Int16: {
                    qint16 s;
                    std::memcpy(&s, sp, 2);
                    v = s / 32768.0;
                    break;
                }
                case QAudioFormat::Int32: {
                    qint32 s;
                    std::memcpy(&s, sp, 4);
                    v = s / 2147483648.0;
                    break;
                }
                case QAudioFormat::UInt8: {
                    quint8 s;
                    std::memcpy(&s, sp, 1);
                    v = (int(s) - 128) / 128.0;
                    break;
                }
                default:
                    break;
                }
                acc += v;
            }
            m_conv[i] = float(acc / ch);
        }
        QMutexLocker lock(&m_mutex);
        const int size = int(m_buf.size());
        for (int i = 0; i < frames; ++i) {
            m_buf[m_pos] = m_conv[i];
            m_pos = (m_pos + 1) % size;
            const float mag = std::fabs(m_conv[i]);
            if (mag > m_peak)
                m_peak = mag;
        }
        m_total += frames;
    }

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QAudioFormat m_format;
    QMutex m_mutex;
    std::vector<float> m_buf;
    std::vector<float> m_conv;
    int m_pos = 0;
    qint64 m_total = 0;
    float m_peak = 0.0f;
};

// ---------------------------------------------------------------------------
// FFT-based SPL + 1/3-octave band analysis. All levels in dBFS; the GUI adds
// the calibration offset.

struct AnalyzerResult {
    double fast = kNaN;
    double slow = kNaN;
    double leq = kNaN;
    std::vector<double> bands;
    std::vector<double> peaks;  // empty when peak hold is off
};

class Analyzer {
public:
    int sr = 48000;
    char weighting = 'A';  // 'A', 'C' or 'Z'

    Analyzer() {
        m_window.resize(FFT_SIZE);
        double sumw2 = 0.0;
        for (int i = 0; i < FFT_SIZE; ++i) {
            m_window[i] = 0.5 - 0.5 * std::cos(2.0 * kPi * i / (FFT_SIZE - 1));
            sumw2 += m_window[i] * m_window[i];
        }
        m_winNorm = double(FFT_SIZE) * sumw2;
        resetAll();
    }

    void resetAll() {
        m_hasFast = false;
        m_hasSlow = false;
        resetLeq();
        m_bandP.assign(NUM_BANDS, kNaN);
        m_hasBandP = false;
        resetPeaks();
    }

    void resetLeq() {
        m_leqSum = 0.0;
        m_leqN = 0;
    }

    void resetPeaks() { m_bandPeakDb.clear(); }

    AnalyzerResult process(const std::vector<float> &x, double dt,
                           double rtaTau, bool peakHold) {
        prepare();
        for (int i = 0; i < FFT_SIZE; ++i)
            m_fftBuf[i] = std::complex<double>(x[i] * m_window[i], 0.0);
        fft(m_fftBuf);
        const int nBins = FFT_SIZE / 2 + 1;
        for (int k = 0; k < nBins; ++k) {
            double p = std::norm(m_fftBuf[k]);
            if (k != 0 && k != FFT_SIZE / 2)
                p *= 2.0;
            m_power[k] = p / m_winNorm * m_wlin[k];  // per-bin weighted power
        }

        // Broadband SPL (20 Hz - 20 kHz), exponential time weighting on power
        double P = 0.0;
        for (int k = m_splLo; k <= m_splHi; ++k)
            P += m_power[k];
        const double aFast = std::exp(-dt / 0.125);
        const double aSlow = std::exp(-dt / 1.0);
        m_fastP = m_hasFast ? aFast * m_fastP + (1 - aFast) * P : P;
        m_slowP = m_hasSlow ? aSlow * m_slowP + (1 - aSlow) * P : P;
        m_hasFast = m_hasSlow = true;
        m_leqSum += P;
        m_leqN += 1;

        // 1/3-octave bands
        std::vector<double> bandP(NUM_BANDS, kNaN);
        for (int i = 0; i < NUM_BANDS; ++i) {
            const Band &b = m_bands[i];
            if (!b.valid)
                continue;
            if (!b.idx.empty()) {
                double s = 0.0;
                for (int k : b.idx)
                    s += m_power[k];
                bandP[i] = s;
            } else {
                bandP[i] = m_power[b.nearBin] * b.factor;
            }
        }
        if (rtaTau > 0.0 && m_hasBandP) {
            const double a = std::exp(-dt / rtaTau);
            for (int i = 0; i < NUM_BANDS; ++i) {
                if (std::isfinite(bandP[i]) && std::isfinite(m_bandP[i]))
                    m_bandP[i] = a * m_bandP[i] + (1 - a) * bandP[i];
                else
                    m_bandP[i] = bandP[i];
            }
        } else {
            m_bandP = bandP;
        }
        m_hasBandP = true;

        AnalyzerResult res;
        res.fast = toDb(m_fastP);
        res.slow = toDb(m_slowP);
        res.leq = toDb(m_leqSum / std::max<qint64>(m_leqN, 1));
        res.bands.resize(NUM_BANDS);
        for (int i = 0; i < NUM_BANDS; ++i)
            res.bands[i] = std::isfinite(m_bandP[i]) ? toDb(m_bandP[i]) : kNaN;

        if (peakHold) {
            const double decay = 6.0 * dt;  // 6 dB/s
            if (m_bandPeakDb.empty()) {
                m_bandPeakDb = res.bands;
            } else {
                for (int i = 0; i < NUM_BANDS; ++i) {
                    const double held = m_bandPeakDb[i] - decay;
                    m_bandPeakDb[i] = std::fmax(held, res.bands[i]);
                }
            }
            res.peaks = m_bandPeakDb;
        } else {
            m_bandPeakDb.clear();
        }
        return res;
    }

private:
    struct Band {
        bool valid = false;
        std::vector<int> idx;
        int nearBin = 0;
        double factor = 1.0;
    };

    void prepare() {
        if (m_cachedSr == sr && m_cachedWeighting == weighting)
            return;
        m_cachedSr = sr;
        m_cachedWeighting = weighting;
        const int nBins = FFT_SIZE / 2 + 1;
        const double df = double(sr) / FFT_SIZE;
        m_fftBuf.resize(FFT_SIZE);
        m_power.resize(nBins);
        m_wlin.resize(nBins);
        for (int k = 0; k < nBins; ++k) {
            const double f = k * df;
            double wdb = 0.0;
            if (weighting == 'A')
                wdb = aWeightDb(f);
            else if (weighting == 'C')
                wdb = cWeightDb(f);
            m_wlin[k] = std::pow(10.0, wdb / 10.0);
        }
        m_splLo = int(std::ceil(20.0 / df));
        m_splHi = std::min(nBins - 1, int(std::floor(20000.0 / df)));
        const double nyq = sr / 2.0;
        m_bands.assign(NUM_BANDS, Band());
        for (int i = 0; i < NUM_BANDS; ++i) {
            const double c = THIRD_OCT_CENTERS[i];
            if (c > nyq)
                continue;
            Band &b = m_bands[i];
            b.valid = true;
            const double lo = c * std::pow(2.0, -1.0 / 6.0);
            const double hi = c * std::pow(2.0, 1.0 / 6.0);
            for (int k = 0; k < nBins; ++k) {
                const double f = k * df;
                if (f >= lo && f < hi)
                    b.idx.push_back(k);
            }
            if (b.idx.empty()) {
                b.nearBin = int(std::lround(c / df));
                b.nearBin = std::clamp(b.nearBin, 0, nBins - 1);
                b.factor = (hi - lo) / df;
            }
        }
        // Reset smoothing state when the spectrum layout changes.
        m_hasFast = m_hasSlow = false;
        m_bandP.assign(NUM_BANDS, kNaN);
        m_hasBandP = false;
        m_bandPeakDb.clear();
    }

    std::vector<double> m_window;
    double m_winNorm = 1.0;
    std::vector<std::complex<double>> m_fftBuf;
    std::vector<double> m_power;
    std::vector<double> m_wlin;
    std::vector<Band> m_bands;
    int m_splLo = 0, m_splHi = 0;
    int m_cachedSr = -1;
    char m_cachedWeighting = 0;

    double m_fastP = 0.0, m_slowP = 0.0;
    bool m_hasFast = false, m_hasSlow = false;
    double m_leqSum = 0.0;
    qint64 m_leqN = 0;
    std::vector<double> m_bandP;
    bool m_hasBandP = false;
    std::vector<double> m_bandPeakDb;
};

// ---------------------------------------------------------------------------
// 1/3-octave bar display.

class RtaWidget : public QWidget {
public:
    RtaWidget() { setMinimumHeight(280); }

    void setData(const std::vector<double> &bands,
                 const std::vector<double> &peaks, double yMin, double yMax) {
        m_bands = bands;
        m_peaks = peaks;
        m_yMin = yMin;
        m_yMax = yMax;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        const QColor bg("#14161c"), grid("#2a2e39"), text("#8a92a6"),
            bar("#2fbf9b"), barTop("#5ce0bd"), peakCol("#e0c05c");
        QPainter qp(this);
        qp.fillRect(rect(), bg);
        const int left = 40, right = 10, top = 10, bottom = 24;
        const int w = width() - left - right;
        const int h = height() - top - bottom;
        if (w <= 10 || h <= 10)
            return;
        const double span = m_yMax - m_yMin;
        auto yOf = [&](double db) {
            return top + h * (1.0 - (db - m_yMin) / span);
        };

        QFont f = font();
        f.setPointSize(8);
        qp.setFont(f);
        const int step = 10;
        for (double g = std::ceil(m_yMin / step) * step; g <= m_yMax; g += step) {
            const int y = int(yOf(g));
            qp.setPen(QPen(grid, 1));
            qp.drawLine(left, y, left + w, y);
            qp.setPen(text);
            qp.drawText(QRect(0, y - 8, left - 6, 16),
                        Qt::AlignRight | Qt::AlignVCenter,
                        QString::number(g, 'f', 0));
        }

        const double slot = double(w) / NUM_BANDS;
        const double barW = std::max(2.0, slot - 2.0);
        for (int i = 0; i < NUM_BANDS; ++i) {
            const double x0 = left + i * slot + (slot - barW) / 2;
            const QString lbl = xAxisLabel(THIRD_OCT_CENTERS[i]);
            if (!lbl.isEmpty()) {
                qp.setPen(text);
                qp.drawText(QRect(int(left + i * slot - slot),
                                  height() - bottom + 4, int(slot * 3), 16),
                            Qt::AlignHCenter, lbl);
            }
            if (i >= int(m_bands.size()) || !std::isfinite(m_bands[i]))
                continue;
            const double v = std::clamp(m_bands[i], m_yMin, m_yMax);
            const int y = int(yOf(v));
            qp.setPen(Qt::NoPen);
            qp.setBrush(bar);
            qp.drawRect(int(x0), y, int(barW), top + h - y);
            qp.setBrush(barTop);
            qp.drawRect(int(x0), y, int(barW), 2);
            if (i < int(m_peaks.size()) && std::isfinite(m_peaks[i])) {
                const double pv = std::clamp(m_peaks[i], m_yMin, m_yMax);
                const int py = int(yOf(pv));
                qp.setPen(QPen(peakCol, 2));
                qp.drawLine(int(x0), py, int(x0 + barW), py);
            }
        }

        qp.setPen(QPen(grid, 1));
        qp.setBrush(Qt::NoBrush);
        qp.drawRect(left, top, w, h);
    }

private:
    std::vector<double> m_bands;
    std::vector<double> m_peaks;
    double m_yMin = 20.0, m_yMax = 120.0;
};

// ---------------------------------------------------------------------------
// One big SPL number with a caption.

class SplReadout : public QWidget {
public:
    explicit SplReadout(const QString &caption) {
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(12, 4, 12, 4);
        lay->setSpacing(0);
        m_cap = new QLabel(caption);
        m_cap->setStyleSheet("color:#8a92a6; font-size:11px;");
        m_val = new QLabel("--.-");
        QFont f;
        f.setFamilies({"Consolas", "Menlo", "Courier New"});
        f.setPointSize(28);
        f.setBold(true);
        m_val->setFont(f);
        m_val->setStyleSheet("color:#e8ecf4;");
        m_val->setMinimumWidth(120);
        lay->addWidget(m_cap);
        lay->addWidget(m_val);
    }

    void set(const QString &caption, double valueDb) {
        m_cap->setText(caption);
        m_val->setText(std::isfinite(valueDb)
                           ? QString::number(valueDb, 'f', 1)
                           : QString("--.-"));
    }

private:
    QLabel *m_cap;
    QLabel *m_val;
};

// ---------------------------------------------------------------------------

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("RTA — SPL Meter & Spectrum Analyzer");

        auto *central = new QWidget;
        setCentralWidget(central);
        central->setStyleSheet("background:#1a1d24; color:#c8cede;");
        auto *root = new QVBoxLayout(central);

        // --- controls row ---
        auto *ctl = new QHBoxLayout;
        ctl->addWidget(new QLabel("Input:"));
        m_deviceCombo = new QComboBox;
        m_deviceCombo->setMinimumWidth(280);
        ctl->addWidget(m_deviceCombo);
        ctl->addSpacing(12);
        ctl->addWidget(new QLabel("Weighting:"));
        m_weightCombo = new QComboBox;
        m_weightCombo->addItems({"A", "C", "Z"});
        ctl->addWidget(m_weightCombo);
        ctl->addSpacing(12);
        ctl->addWidget(new QLabel("RTA avg:"));
        m_avgCombo = new QComboBox;
        m_avgCombo->addItems({"Off", "Fast (125 ms)", "Slow (1 s)", "2 s"});
        m_avgCombo->setCurrentIndex(1);
        ctl->addWidget(m_avgCombo);
        ctl->addSpacing(12);
        m_peakCheck = new QCheckBox("Peak hold");
        ctl->addWidget(m_peakCheck);
        ctl->addSpacing(12);
        ctl->addWidget(new QLabel("Cal:"));
        m_calSpin = new QDoubleSpinBox;
        m_calSpin->setRange(0.0, 200.0);
        m_calSpin->setDecimals(1);
        m_calSpin->setSingleStep(0.5);
        m_calSpin->setValue(100.0);
        m_calSpin->setSuffix(" dB");
        m_calSpin->setToolTip(
            "Calibration offset added to dBFS.\nPlay a known level (e.g. a 94 "
            "dB calibrator) and adjust until the meter matches.");
        ctl->addWidget(m_calSpin);
        ctl->addStretch(1);
        auto *resetBtn = new QPushButton("Reset Leq/Peaks");
        ctl->addWidget(resetBtn);
        root->addLayout(ctl);

        // --- SPL readouts ---
        auto *splRow = new QHBoxLayout;
        m_rFast = new SplReadout("LAF (Fast)");
        m_rSlow = new SplReadout("LAS (Slow)");
        m_rLeq = new SplReadout("LAeq");
        splRow->addWidget(m_rFast);
        splRow->addWidget(m_rSlow);
        splRow->addWidget(m_rLeq);
        m_clipLbl = new QLabel("CLIP");
        setClipStyle(false);
        splRow->addWidget(m_clipLbl);
        splRow->addStretch(1);
        root->addLayout(splRow);

        // --- RTA ---
        m_rta = new RtaWidget;
        root->addWidget(m_rta, 1);

        statusBar()->setStyleSheet("color:#8a92a6;");

        // --- wiring ---
        populateDevices();
        QObject::connect(m_deviceCombo, &QComboBox::currentIndexChanged,
                         this, [this](int row) { changeDevice(row); });
        QObject::connect(m_weightCombo, &QComboBox::currentTextChanged,
                         this, [this](const QString &w) {
                             m_analyzer.weighting = w.isEmpty() ? 'Z' : w[0].toLatin1();
                             m_analyzer.resetAll();
                         });
        QObject::connect(resetBtn, &QPushButton::clicked, this, [this] {
            m_analyzer.resetLeq();
            m_analyzer.resetPeaks();
        });

        auto *timer = new QTimer(this);
        timer->setInterval(UPDATE_MS);
        QObject::connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start();

        changeDevice(m_deviceCombo->currentIndex());
    }

    ~MainWindow() override { m_engine.stop(); }

private:
    void setClipStyle(bool lit) {
        m_clipLbl->setStyleSheet(
            QString("color:%1; font-weight:bold; font-size:16px; padding:0 12px;")
                .arg(lit ? "#e05c5c" : "#3a3f4d"));
    }

    void populateDevices() {
        m_deviceCombo->blockSignals(true);
        m_deviceCombo->clear();
        m_devices = QMediaDevices::audioInputs();
        const QAudioDevice def = QMediaDevices::defaultAudioInput();
        int selectRow = 0;
        for (int i = 0; i < m_devices.size(); ++i) {
            m_deviceCombo->addItem(m_devices[i].description());
            if (m_devices[i].id() == def.id())
                selectRow = i;
        }
        m_deviceCombo->setCurrentIndex(selectRow);
        m_deviceCombo->blockSignals(false);
    }

    void changeDevice(int row) {
        if (row < 0 || row >= m_devices.size()) {
            statusBar()->showMessage("No audio input devices found");
            return;
        }
        try {
            m_engine.start(m_devices[row]);
            m_analyzer.sr = m_engine.sampleRate();
            m_analyzer.resetAll();
            statusBar()->showMessage(
                QString("Listening at %1 Hz — FFT %2 (%3 ms window)")
                    .arg(m_engine.sampleRate())
                    .arg(FFT_SIZE)
                    .arg(double(FFT_SIZE) / m_engine.sampleRate() * 1000.0, 0,
                         'f', 0));
        } catch (const std::exception &e) {
            statusBar()->showMessage(
                QString("Could not open input: %1").arg(e.what()));
        }
    }

    double rtaTau() const {
        static const double taus[] = {0.0, 0.125, 1.0, 2.0};
        return taus[m_avgCombo->currentIndex()];
    }

    void tick() {
        float pk = 0.0f;
        const bool have = m_engine.latest(FFT_SIZE, m_samples, pk);
        if (pk >= 0.99f)
            m_clipTicks = 20;
        if (m_clipTicks > 0) {
            --m_clipTicks;
            setClipStyle(true);
        } else {
            setClipStyle(false);
        }
        if (!have)
            return;
        const double dt = UPDATE_MS / 1000.0;
        AnalyzerResult res = m_analyzer.process(m_samples, dt, rtaTau(),
                                                m_peakCheck->isChecked());
        const double cal = m_calSpin->value();
        const QString w = m_weightCombo->currentText();
        m_rFast->set(QString("L%1F (Fast)").arg(w), res.fast + cal);
        m_rSlow->set(QString("L%1S (Slow)").arg(w), res.slow + cal);
        m_rLeq->set(QString("L%1eq").arg(w), res.leq + cal);
        for (double &b : res.bands)
            b += cal;
        for (double &p : res.peaks)
            p += cal;
        m_rta->setData(res.bands, res.peaks, cal - 80.0, cal + 20.0);
    }

    AudioEngine m_engine;
    Analyzer m_analyzer;
    std::vector<float> m_samples;
    QList<QAudioDevice> m_devices;
    QComboBox *m_deviceCombo;
    QComboBox *m_weightCombo;
    QComboBox *m_avgCombo;
    QCheckBox *m_peakCheck;
    QDoubleSpinBox *m_calSpin;
    QLabel *m_clipLbl;
    SplReadout *m_rFast;
    SplReadout *m_rSlow;
    SplReadout *m_rLeq;
    RtaWidget *m_rta;
    int m_clipTicks = 0;
};

// ---------------------------------------------------------------------------

// Verify DSP math with a synthetic full-scale 1 kHz sine (no audio, no GUI).
static int selftest() {
    Analyzer an;
    an.sr = 48000;
    an.weighting = 'A';
    std::vector<float> x(FFT_SIZE);
    for (int i = 0; i < FFT_SIZE; ++i)
        x[i] = float(std::sin(2.0 * kPi * 1000.0 * i / an.sr));
    AnalyzerResult res;
    for (int i = 0; i < 100; ++i)  // let Fast/Slow converge
        res = an.process(x, 0.05, 0.125, true);
    // Full-scale sine: mean square 0.5 -> -3.01 dBFS; A-weight at 1 kHz = 0 dB.
    std::printf("fast  = %7.2f dBFS (expected ~ -3.01)\n", res.fast);
    std::printf("slow  = %7.2f dBFS (expected ~ -3.01)\n", res.slow);
    std::printf("leq   = %7.2f dBFS (expected ~ -3.01)\n", res.leq);
    int kMax = 0;
    for (int i = 1; i < NUM_BANDS; ++i)
        if (std::isfinite(res.bands[i]) &&
            (!std::isfinite(res.bands[kMax]) || res.bands[i] > res.bands[kMax]))
            kMax = i;
    std::printf("loudest band = %g Hz at %.2f dBFS (expected 1000 Hz ~ -3.01)\n",
                THIRD_OCT_CENTERS[kMax], res.bands[kMax]);
    double others = 0.0;
    for (int i = 0; i < NUM_BANDS; ++i)
        if (i != kMax && std::isfinite(res.bands[i]))
            others += std::pow(10.0, res.bands[i] / 10.0);
    std::printf("energy outside 1 kHz band = %.1f dBFS (expected far below -3)\n",
                toDb(others));
    const bool ok = std::fabs(res.fast + 3.01) < 0.2 &&
                    THIRD_OCT_CENTERS[kMax] == 1000.0 &&
                    std::fabs(res.bands[kMax] + 3.01) < 0.3;
    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0)
            return selftest();
    QApplication app(argc, argv);
    MainWindow win;
    win.resize(1000, 560);
    win.show();
    return app.exec();
}
