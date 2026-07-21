// DSP core: FFT, frequency weighting, and the SPL/RTA analyzer.
#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <complex>
#include <limits>
#include <utility>
#include <vector>

inline constexpr int FFT_SIZE = 16384;
inline constexpr int UPDATE_MS = 50;
inline constexpr double kPi = 3.14159265358979323846;
inline const double kNaN = std::numeric_limits<double>::quiet_NaN();

inline constexpr double THIRD_OCT_CENTERS[] = {
    20, 25, 31.5, 40, 50, 63, 80, 100, 125, 160, 200, 250, 315, 400, 500,
    630, 800, 1000, 1250, 1600, 2000, 2500, 3150, 4000, 5000, 6300, 8000,
    10000, 12500, 16000, 20000,
};
inline constexpr int NUM_BANDS = int(sizeof(THIRD_OCT_CENTERS) / sizeof(double));

inline double aWeightDb(double f) {
    f = std::max(f, 1e-6);
    const double f2 = f * f;
    const double num = (12194.0 * 12194.0) * f2 * f2;
    const double den = (f2 + 20.6 * 20.6)
                       * std::sqrt((f2 + 107.7 * 107.7) * (f2 + 737.9 * 737.9))
                       * (f2 + 12194.0 * 12194.0);
    return 20.0 * std::log10(num / den) + 2.00;
}

inline double cWeightDb(double f) {
    f = std::max(f, 1e-6);
    const double f2 = f * f;
    const double num = (12194.0 * 12194.0) * f2;
    const double den = (f2 + 20.6 * 20.6) * (f2 + 12194.0 * 12194.0);
    return 20.0 * std::log10(num / den) + 0.06;
}

// In-place iterative radix-2 FFT (n must be a power of two).
inline void fft(std::vector<std::complex<double>> &a) {
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

inline double toDb(double power) {
    return 10.0 * std::log10(std::max(power, 1e-20));
}

// Streaming time-domain C-weighting: two 1st-order highpasses at 20.6 Hz and
// two 1st-order lowpasses at 12.2 kHz (bilinear transform, prewarped), unity
// gain at 1 kHz. Used for LCpk metering — approximate near Nyquist, not an
// IEC 61672 implementation.
class CWeightFilter {
public:
    void design(double fs) {
        auto onePole = [&](double f0, bool hp, double c[3]) {
            const double W = std::tan(kPi * f0 / fs);
            const double n = 1.0 / (1.0 + W);
            c[2] = (W - 1.0) * n;  // a1
            c[0] = hp ? n : W * n;
            c[1] = hp ? -n : W * n;
        };
        onePole(20.598997, true, m_c[0]);
        onePole(20.598997, true, m_c[1]);
        onePole(12194.217, false, m_c[2]);
        onePole(12194.217, false, m_c[3]);
        const std::complex<double> z =
            std::exp(std::complex<double>(0.0, -2.0 * kPi * 1000.0 / fs));
        std::complex<double> H(1.0, 0.0);
        for (const auto &c : m_c)
            H *= (c[0] + c[1] * z) / (1.0 + c[2] * z);
        m_gain = 1.0 / std::abs(H);
        reset();
    }

    void reset() {
        for (int i = 0; i < 4; ++i)
            m_x[i] = m_y[i] = 0.0;
    }

    double step(double x) {
        for (int i = 0; i < 4; ++i) {
            const double y = m_c[i][0] * x + m_c[i][1] * m_x[i] - m_c[i][2] * m_y[i];
            m_x[i] = x;
            m_y[i] = y;
            x = y;
        }
        return x * m_gain;
    }

private:
    double m_c[4][3] = {};
    double m_x[4] = {}, m_y[4] = {};
    double m_gain = 1.0;
};

struct AnalyzerResult {
    double fast = kNaN;
    double slow = kNaN;
    double leq = kNaN;
    // Broadband mean-square power (linear, dBFS domain) under each standard
    // weighting, independent of the display weighting — for derived metrics.
    double powA = 0.0, powC = 0.0, powZ = 0.0;
    std::vector<double> bands;
    std::vector<double> peaks;  // empty when peak hold is off
};

// FFT-based SPL + 1/3-octave band analysis. All levels in dBFS; the GUI adds
// the calibration offset.
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

    // Mic correction: sorted (freq Hz, response dB) points describing the
    // microphone's deviation from flat. The response is SUBTRACTED from the
    // spectrum (standard measurement-mic cal file convention). Empty = off.
    void setMicCorrection(std::vector<std::pair<double, double>> points) {
        std::sort(points.begin(), points.end());
        m_micCorr = std::move(points);
        m_cachedSr = -1;  // force prepare() to rebuild the weighting table
    }
    bool hasMicCorrection() const { return !m_micCorr.empty(); }

    // Weighted per-bin power of the last processed block (for spectrograms).
    const std::vector<double> &lastPower() const { return m_power; }
    double binWidth() const { return double(sr) / FFT_SIZE; }

    AnalyzerResult process(const std::vector<float> &x, double dt,
                           double rtaTau, bool peakHold) {
        prepare();
        for (int i = 0; i < FFT_SIZE; ++i)
            m_fftBuf[i] = std::complex<double>(x[i] * m_window[i], 0.0);
        fft(m_fftBuf);
        const int nBins = FFT_SIZE / 2 + 1;
        const std::vector<double> *sel =
            weighting == 'A' ? &m_wA : weighting == 'C' ? &m_wC : nullptr;
        for (int k = 0; k < nBins; ++k) {
            double p = std::norm(m_fftBuf[k]);
            if (k != 0 && k != FFT_SIZE / 2)
                p *= 2.0;
            const double pz = p / m_winNorm * m_micLin[k];  // mic-corrected
            m_powerZ[k] = pz;
            m_power[k] = sel ? pz * (*sel)[k] : pz;  // display weighting
        }

        // Broadband SPL (20 Hz - 20 kHz) under all three weightings;
        // exponential time weighting on power for the displayed one.
        double PA = 0.0, PC = 0.0, PZ = 0.0;
        for (int k = m_splLo; k <= m_splHi; ++k) {
            PA += m_powerZ[k] * m_wA[k];
            PC += m_powerZ[k] * m_wC[k];
            PZ += m_powerZ[k];
        }
        const double P = weighting == 'A' ? PA : weighting == 'C' ? PC : PZ;
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
        res.powA = PA;
        res.powC = PC;
        res.powZ = PZ;
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
        m_powerZ.resize(nBins);
        m_wA.resize(nBins);
        m_wC.resize(nBins);
        m_micLin.resize(nBins);
        for (int k = 0; k < nBins; ++k) {
            const double f = k * df;
            m_wA[k] = std::pow(10.0, aWeightDb(f) / 10.0);
            m_wC[k] = std::pow(10.0, cWeightDb(f) / 10.0);
            m_micLin[k] =
                m_micCorr.empty()
                    ? 1.0
                    : std::pow(10.0, -micCorrDb(f) / 10.0);
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

    // Log-frequency linear interpolation of the mic response, clamped to the
    // first/last point outside the file's range.
    double micCorrDb(double f) const {
        const auto &v = m_micCorr;
        if (f <= v.front().first)
            return v.front().second;
        if (f >= v.back().first)
            return v.back().second;
        auto hi = std::upper_bound(
            v.begin(), v.end(), std::make_pair(f, -1e300));
        auto lo = hi - 1;
        const double u = (std::log(f) - std::log(lo->first)) /
                         (std::log(hi->first) - std::log(lo->first));
        return lo->second + u * (hi->second - lo->second);
    }

    std::vector<std::pair<double, double>> m_micCorr;
    std::vector<double> m_window;
    double m_winNorm = 1.0;
    std::vector<std::complex<double>> m_fftBuf;
    std::vector<double> m_power;
    std::vector<double> m_powerZ;
    std::vector<double> m_wA, m_wC;
    std::vector<double> m_micLin;
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
