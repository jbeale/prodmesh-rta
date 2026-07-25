// Smaart-style SPL metrics derived from per-tick broadband powers: rolling
// Leq windows, peaks, C-A ratio, L10/L50/L90 percentiles, and OSHA/NIOSH
// noise dose. History-based metrics bake in the calibration offset at
// accumulation time, so cal changes only affect new data.
#pragma once

#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <deque>
#include <vector>

#include "dsp.h"

struct LoudnessValues {
    double momentary = kNaN;    // LUFS over 400 ms
    double shortTerm = kNaN;    // LUFS over 3 s
    double integrated = kNaN;   // LUFS, gated, since the last session reset
    double truePeak = kNaN;     // dBTP over the last 400 ms
    double truePeakMax = kNaN;  // dBTP since the last session reset
    double plr = kNaN;          // peak-to-loudness ratio, LU
};

struct MetricValues {
    double laf = kNaN, las = kNaN, leq = kNaN;  // filled by the caller
    double leqShort = kNaN;                     // rolling LAeq, short window
    double leqLong = kNaN;                      // rolling LAeq, long window
    double lzpk = kNaN, lcpk = kNaN;            // this tick's peaks, dB SPL
    double ca = kNaN;                           // LCeq - LAeq, short window
    double l10 = kNaN, l50 = kNaN, l90 = kNaN;  // session percentiles
    double doseNiosh = kNaN, doseOsha = kNaN;   // percent of daily dose
    LoudnessValues loud;                        // Program mode only
    double toTarget = kNaN;                     // integrated - target, LU
};

class MetricsEngine {
public:
    int shortWindowS = 60;
    int longWindowS = 900;

    // Session statistics (percentiles, dose) — tied to the Reset button.
    void resetSession() {
        m_secLevels.clear();
        m_sorted.clear();
        m_pctDirty = false;
        m_secP = 0.0;
        m_secT = 0.0;
        m_doseN = 0.0;
        m_doseO = 0.0;
    }

    void resetAll() {
        resetSession();
        m_roll.clear();
        m_rollPA = m_rollPC = 0.0;
        m_rollT = 0.0;
        m_sinceRebuild = 0;
    }

    void push(double powA, double powC, double peakZ, double peakC, double dt,
              double cal) {
        m_lzpk = peakZ > 0 ? 20.0 * std::log10(peakZ) + cal : kNaN;
        m_lcpk = peakC > 0 ? 20.0 * std::log10(peakC) + cal : kNaN;

        // Rolling window (kept at the long horizon; the short window is
        // scanned from the back on demand). Time-weighted sums; rebuilt
        // periodically to cap floating-point drift.
        m_roll.push_back({dt, powA, powC});
        m_rollT += dt;
        m_rollPA += powA * dt;
        m_rollPC += powC * dt;
        const double horizon = std::max(shortWindowS, longWindowS);
        while (!m_roll.empty() && m_rollT - m_roll.front().dt > horizon) {
            const Slice &s = m_roll.front();
            m_rollT -= s.dt;
            m_rollPA -= s.pa * s.dt;
            m_rollPC -= s.pc * s.dt;
            m_roll.pop_front();
        }
        if (++m_sinceRebuild >= 4096) {
            m_sinceRebuild = 0;
            m_rollT = m_rollPA = m_rollPC = 0.0;
            for (const Slice &s : m_roll) {
                m_rollT += s.dt;
                m_rollPA += s.pa * s.dt;
                m_rollPC += s.pc * s.dt;
            }
        }

        // 1-second LAeq series for the percentile levels.
        m_secP += powA * dt;
        m_secT += dt;
        if (m_secT >= 1.0) {
            m_secLevels.push_back(toDb(m_secP / m_secT) + cal);
            m_secP = 0.0;
            m_secT = 0.0;
            m_pctDirty = true;
        }

        // Noise dose (criterion / exchange-rate / 80 dB threshold):
        // NIOSH 85 dBA & 3 dB, OSHA 90 dBA & 5 dB, both against 8 h.
        const double la = toDb(powA) + cal;
        if (la >= 80.0) {
            static constexpr double kT8 = 8.0 * 3600.0;
            m_doseN += dt / kT8 * std::pow(2.0, (la - 85.0) / 3.0);
            m_doseO += dt / kT8 * std::pow(2.0, (la - 90.0) / 5.0);
        }
    }

    MetricValues values(double cal) {
        MetricValues v;
        v.lzpk = m_lzpk;
        v.lcpk = m_lcpk;
        if (m_rollT > 0.0) {
            double t = 0.0, pa = 0.0, pc = 0.0;
            for (auto it = m_roll.rbegin();
                 it != m_roll.rend() && t < shortWindowS; ++it) {
                t += it->dt;
                pa += it->pa * it->dt;
                pc += it->pc * it->dt;
            }
            if (t > 0.0 && pa > 0.0) {
                v.leqShort = toDb(pa / t) + cal;
                v.ca = toDb(pc / t) - toDb(pa / t);
            }
            if (m_rollPA > 0.0)
                v.leqLong = toDb(m_rollPA / m_rollT) + cal;
        }
        if (m_secLevels.size() >= 10) {
            if (m_pctDirty) {
                m_pctDirty = false;
                m_sorted = m_secLevels;
                std::sort(m_sorted.begin(), m_sorted.end());
            }
            // LN = level exceeded N% of the time = (100-N)th percentile.
            auto q = [&](double frac) {
                const size_t i =
                    size_t(frac * double(m_sorted.size() - 1) + 0.5);
                return m_sorted[i];
            };
            v.l10 = q(0.90);
            v.l50 = q(0.50);
            v.l90 = q(0.10);
        }
        v.doseNiosh = m_doseN * 100.0;
        v.doseOsha = m_doseO * 100.0;
        return v;
    }

private:
    struct Slice {
        double dt, pa, pc;
    };

    std::deque<Slice> m_roll;
    double m_rollT = 0.0, m_rollPA = 0.0, m_rollPC = 0.0;
    int m_sinceRebuild = 0;
    std::vector<double> m_secLevels;
    std::vector<double> m_sorted;
    bool m_pctDirty = false;
    double m_secP = 0.0, m_secT = 0.0;
    double m_doseN = 0.0, m_doseO = 0.0;
    double m_lzpk = kNaN, m_lcpk = kNaN;
};

// ---------------------------------------------------------------------------

// EBU R128 / ITU-R BS.1770 loudness from gapless 100 ms sub-blocks.
//
// Momentary and short-term are plain sliding windows over those sub-blocks.
// Integrated applies the two-stage gate over 400 ms blocks at a 100 ms hop
// (75 % overlap): drop anything at or below -70 LUFS, then drop anything more
// than 10 LU below the mean of what survived. That is what stops the silence
// between songs — or between a sermon's sentences — from dragging the
// programme number down.
//
// The gate is evaluated from a 0.1 LU histogram holding a count and the summed
// power per bin, so only the threshold comparison is quantized; the mean over
// the selected set stays exact.
class LoudnessEngine {
public:
    static constexpr double kOffset = -0.691;  // BS.1770 loudness offset
    static constexpr double kAbsGate = -70.0;  // LUFS
    static constexpr double kRelDrop = -10.0;  // LU below the gated mean
    static constexpr int kSubMs = 100;
    static constexpr int kMomSubs = 4;     // 400 ms
    static constexpr int kShortSubs = 30;  // 3 s

    // Loudness of a channel-summed mean square. NaN for digital silence,
    // which also makes the absolute-gate comparison below reject it.
    static double loudness(double z) {
        return z > 0.0 ? kOffset + 10.0 * std::log10(z) : kNaN;
    }

    // Mean square corresponding to a loudness level — the inverse of the
    // above, handy for tests and for target lines.
    static double powerFor(double lufs) {
        return std::pow(10.0, (lufs - kOffset) / 10.0);
    }

    void resetAll() {
        m_sub.clear();
        resetSession();
    }

    // Integrated loudness and the true-peak maximum are session statistics,
    // tied to the same Reset button as Leq and the percentiles.
    void resetSession() {
        m_binN.assign(kBins, 0);
        m_binP.assign(kBins, 0.0);
        m_tpMax = 0.0;
    }

    void push(const LoudnessBlock &b) {
        m_tpMax = std::max(m_tpMax, b.tpLin);
        m_sub.push_back(b);
        while (m_sub.size() > size_t(kShortSubs))
            m_sub.pop_front();
        // Every sub-block completes another 400 ms gating block.
        if (m_sub.size() >= size_t(kMomSubs)) {
            const double z = meanZ(kMomSubs);
            const double l = loudness(z);
            if (l > kAbsGate) {
                const int i =
                    std::clamp(int((l - kBinLo) / kBinW), 0, kBins - 1);
                ++m_binN[i];
                m_binP[i] += z;
            }
        }
    }

    LoudnessValues values() const {
        LoudnessValues v;
        if (m_sub.size() >= size_t(kMomSubs)) {
            v.momentary = loudness(meanZ(kMomSubs));
            double tp = 0.0;
            for (size_t i = m_sub.size() - kMomSubs; i < m_sub.size(); ++i)
                tp = std::max(tp, m_sub[i].tpLin);
            if (tp > 0.0)
                v.truePeak = 20.0 * std::log10(tp);
        }
        if (m_sub.size() >= size_t(kShortSubs))
            v.shortTerm = loudness(meanZ(kShortSubs));
        v.integrated = integrated();
        if (m_tpMax > 0.0)
            v.truePeakMax = 20.0 * std::log10(m_tpMax);
        if (std::isfinite(v.integrated) && std::isfinite(v.truePeakMax))
            v.plr = v.truePeakMax - v.integrated;
        return v;
    }

private:
    static constexpr double kBinLo = -70.0;
    static constexpr double kBinHi = 10.0;
    static constexpr double kBinW = 0.1;
    static constexpr int kBins = int((kBinHi - kBinLo) / kBinW);

    // Mean of the last n sub-block powers. Equal durations, so no weighting.
    double meanZ(int n) const {
        double s = 0.0;
        for (size_t i = m_sub.size() - n; i < m_sub.size(); ++i)
            s += m_sub[i].z;
        return s / n;
    }

    double integrated() const {
        // Everything in the histogram already passed the absolute gate.
        qint64 nA = 0;
        double pA = 0.0;
        for (int i = 0; i < kBins; ++i) {
            nA += m_binN[i];
            pA += m_binP[i];
        }
        if (nA == 0)
            return kNaN;
        const double relGate = loudness(pA / double(nA)) + kRelDrop;
        qint64 nR = 0;
        double pR = 0.0;
        for (int i = 0; i < kBins; ++i) {
            if (m_binN[i] == 0)
                continue;
            if (kBinLo + (i + 0.5) * kBinW > relGate) {
                nR += m_binN[i];
                pR += m_binP[i];
            }
        }
        return nR == 0 ? kNaN : loudness(pR / double(nR));
    }

    std::deque<LoudnessBlock> m_sub;
    std::vector<qint64> m_binN = std::vector<qint64>(kBins, 0);
    std::vector<double> m_binP = std::vector<double>(kBins, 0.0);
    double m_tpMax = 0.0;
};
