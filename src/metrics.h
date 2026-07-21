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

struct MetricValues {
    double laf = kNaN, las = kNaN, leq = kNaN;  // filled by the caller
    double leqShort = kNaN;                     // rolling LAeq, short window
    double leqLong = kNaN;                      // rolling LAeq, long window
    double lzpk = kNaN, lcpk = kNaN;            // this tick's peaks, dB SPL
    double ca = kNaN;                           // LCeq - LAeq, short window
    double l10 = kNaN, l50 = kNaN, l90 = kNaN;  // session percentiles
    double doseNiosh = kNaN, doseOsha = kNaN;   // percent of daily dose
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
