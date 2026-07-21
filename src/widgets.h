// Display widgets: RTA bars, spectrogram, SPL history strip, big readouts.
#pragma once

#include <QCheckBox>
#include <QCloseEvent>
#include <QColor>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QStringList>
#include <QStyleOption>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>
#include <deque>
#include <functional>

#include "dsp.h"

inline QString xAxisLabel(double center) {
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

namespace theme {
inline const QColor bg("#14161c");
inline const QColor grid("#2a2e39");
inline const QColor text("#8a92a6");
inline const QColor bar("#2fbf9b");
inline const QColor barTop("#5ce0bd");
inline const QColor peak("#e0c05c");
inline const QColor faint("#3a6b5f");
}  // namespace theme

// ---------------------------------------------------------------------------

class RtaWidget : public QWidget {
public:
    RtaWidget() { setMinimumHeight(240); }

    void setViewMode(int mode) {  // 0 = bars, 1 = line
        m_mode = mode;
        update();
    }

    // Display fall rate in dB/s; bands rise instantly and decay at this
    // rate once the level drops. 0 disables (display follows the data).
    void setDecayRate(double dbPerSec) { m_decay = dbPerSec; }

    void setData(const std::vector<double> &bands,
                 const std::vector<double> &peaks, double yMin, double yMax,
                 double dt) {
        if (m_decay <= 0.0 || m_disp.size() != bands.size()) {
            m_disp = bands;
        } else {
            for (size_t i = 0; i < bands.size(); ++i) {
                const double fallen = m_disp[i] - m_decay * dt;
                if (!std::isfinite(fallen))
                    m_disp[i] = bands[i];
                else if (std::isfinite(bands[i]))
                    m_disp[i] = std::max(bands[i], fallen);
                else
                    m_disp[i] = fallen;
            }
        }
        m_peaks = peaks;
        m_yMin = yMin;
        m_yMax = yMax;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter qp(this);
        qp.fillRect(rect(), theme::bg);
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
            qp.setPen(QPen(theme::grid, 1));
            qp.drawLine(left, y, left + w, y);
            qp.setPen(theme::text);
            qp.drawText(QRect(0, y - 8, left - 6, 16),
                        Qt::AlignRight | Qt::AlignVCenter,
                        QString::number(g, 'f', 0));
        }

        const double slot = double(w) / NUM_BANDS;
        const double barW = std::max(2.0, slot - 2.0);
        for (int i = 0; i < NUM_BANDS; ++i) {
            const QString lbl = xAxisLabel(THIRD_OCT_CENTERS[i]);
            if (!lbl.isEmpty()) {
                qp.setPen(theme::text);
                qp.drawText(QRect(int(left + i * slot - slot),
                                  height() - bottom + 4, int(slot * 3), 16),
                            Qt::AlignHCenter, lbl);
            }
        }

        if (m_mode == 1) {
            QPainterPath line;
            double firstX = 0, lastX = 0;
            bool started = false;
            for (int i = 0; i < NUM_BANDS && i < int(m_disp.size()); ++i) {
                if (!std::isfinite(m_disp[i]))
                    continue;
                const double x = left + (i + 0.5) * slot;
                const double v = std::clamp(m_disp[i], m_yMin, m_yMax);
                if (!started) {
                    line.moveTo(x, yOf(v));
                    firstX = x;
                    started = true;
                } else {
                    line.lineTo(x, yOf(v));
                }
                lastX = x;
            }
            if (started) {
                QPainterPath fill = line;
                fill.lineTo(lastX, top + h);
                fill.lineTo(firstX, top + h);
                fill.closeSubpath();
                QColor fc = theme::bar;
                fc.setAlpha(60);
                qp.setRenderHint(QPainter::Antialiasing);
                qp.setPen(Qt::NoPen);
                qp.setBrush(fc);
                qp.drawPath(fill);
                qp.setPen(QPen(theme::barTop, 2));
                qp.setBrush(Qt::NoBrush);
                qp.drawPath(line);
                qp.setRenderHint(QPainter::Antialiasing, false);
            }
        } else {
            for (int i = 0; i < NUM_BANDS && i < int(m_disp.size()); ++i) {
                if (!std::isfinite(m_disp[i]))
                    continue;
                const double x0 = left + i * slot + (slot - barW) / 2;
                const double v = std::clamp(m_disp[i], m_yMin, m_yMax);
                const int y = int(yOf(v));
                qp.setPen(Qt::NoPen);
                qp.setBrush(theme::bar);
                qp.drawRect(int(x0), y, int(barW), top + h - y);
                qp.setBrush(theme::barTop);
                qp.drawRect(int(x0), y, int(barW), 2);
            }
        }

        for (int i = 0; i < NUM_BANDS && i < int(m_peaks.size()); ++i) {
            if (!std::isfinite(m_peaks[i]))
                continue;
            const double x0 = left + i * slot + (slot - barW) / 2;
            const double pv = std::clamp(m_peaks[i], m_yMin, m_yMax);
            const int py = int(yOf(pv));
            qp.setPen(QPen(theme::peak, 2));
            qp.drawLine(int(x0), py, int(x0 + barW), py);
        }

        qp.setPen(QPen(theme::grid, 1));
        qp.setBrush(Qt::NoBrush);
        qp.drawRect(left, top, w, h);
    }

private:
    std::vector<double> m_disp;
    std::vector<double> m_peaks;
    double m_yMin = 20.0, m_yMax = 120.0;
    int m_mode = 0;
    double m_decay = 0.0;
};

// ---------------------------------------------------------------------------
// Scrolling log-frequency spectrogram. Internal image is COLS x ROWS; each
// pushed column covers one UPDATE_MS tick, so the view spans ~30 s. Raw dB
// values are kept alongside the image so theme/range changes recolor the
// whole history, not just new columns.

class SpectrogramWidget : public QWidget {
public:
    static constexpr int COLS = 600;
    static constexpr int ROWS = 200;
    static constexpr double OCTAVES = 10.0;  // 20 Hz .. ~20.5 kHz

    SpectrogramWidget()
        : m_img(COLS, ROWS, QImage::Format_RGB32),
          m_vals(size_t(COLS) * ROWS, kEmpty) {
        rebuildLut();
        m_img.fill(m_lut[0]);
        setMinimumHeight(240);
    }

    static QStringList themeNames() {
        return {"ProdMesh", "Green-Red", "Inferno",
                "Viridis",  "Rainbow",   "Grayscale"};
    }

    void setTheme(int idx) {
        m_theme = std::clamp(idx, 0, int(themeNames().size()) - 1);
        rebuildLut();
        recolor();
    }

    // Dynamic range of the color scale, in dB (floor color .. max color).
    void setRangeDb(double range) {
        m_range = std::max(10.0, range);
        recolor();
    }

    // Positive values shift the scale down so quieter material lights up.
    void setSensitivityDb(double sens) {
        m_sens = sens;
        recolor();
    }

    void pushColumn(const std::vector<double> &power, double df) {
        if (power.empty() || df <= 0)
            return;
        for (int r = 0; r < ROWS; ++r) {
            QRgb *line = reinterpret_cast<QRgb *>(m_img.scanLine(r));
            std::memmove(line, line + 1, (COLS - 1) * sizeof(QRgb));
            float *vals = &m_vals[size_t(r) * COLS];
            std::memmove(vals, vals + 1, (COLS - 1) * sizeof(float));
        }
        const int n = int(power.size());
        for (int r = 0; r < ROWS; ++r) {
            const double frac = double(ROWS - 1 - r) / (ROWS - 1);  // 0 bottom
            const double f = 20.0 * std::pow(2.0, frac * OCTAVES);
            int k = int(std::lround(f / df));
            k = std::clamp(k, 0, n - 1);
            const float v = float(toDb(power[k]));
            m_vals[size_t(r) * COLS + COLS - 1] = v;
            reinterpret_cast<QRgb *>(m_img.scanLine(r))[COLS - 1] = colorFor(v);
        }
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter qp(this);
        qp.fillRect(rect(), theme::bg);
        const int left = 40, right = 10, top = 10, bottom = 20;
        const int w = width() - left - right;
        const int h = height() - top - bottom;
        if (w <= 10 || h <= 10)
            return;
        qp.setRenderHint(QPainter::SmoothPixmapTransform);
        qp.drawImage(QRect(left, top, w, h), m_img);

        QFont f = font();
        f.setPointSize(8);
        qp.setFont(f);
        for (double fr : {100.0, 1000.0, 10000.0}) {
            const double frac = std::log2(fr / 20.0) / OCTAVES;  // 0 bottom
            const int y = int(top + h * (1.0 - frac));
            qp.setPen(QPen(QColor(255, 255, 255, 40), 1, Qt::DotLine));
            qp.drawLine(left, y, left + w, y);
            qp.setPen(theme::text);
            qp.drawText(QRect(0, y - 8, left - 6, 16),
                        Qt::AlignRight | Qt::AlignVCenter,
                        fr >= 1000 ? QString("%1k").arg(fr / 1000)
                                   : QString::number(fr, 'f', 0));
        }
        qp.setPen(theme::text);
        const double spanS = COLS * UPDATE_MS / 1000.0;
        qp.drawText(QRect(left, height() - bottom + 2, w, 16), Qt::AlignLeft,
                    QString("-%1 s").arg(spanS, 0, 'f', 0));
        qp.drawText(QRect(left, height() - bottom + 2, w, 16), Qt::AlignRight,
                    "now");
        qp.setPen(QPen(theme::grid, 1));
        qp.setBrush(Qt::NoBrush);
        qp.drawRect(left, top, w, h);
    }

private:
    static constexpr float kEmpty = -1e9f;

    // Default scale matches the RTA plot: 100 dB of range ending 20 dB above
    // full scale. Sensitivity slides that window down; range shrinks it.
    QRgb colorFor(float dbfs) const {
        const double top = 20.0 - m_sens;
        const double t = (dbfs - (top - m_range)) / m_range;
        const int i = int(std::clamp(t, 0.0, 1.0) * 255.0 + 0.5);
        return m_lut[i];
    }

    void recolor() {
        for (int r = 0; r < ROWS; ++r) {
            QRgb *line = reinterpret_cast<QRgb *>(m_img.scanLine(r));
            const float *vals = &m_vals[size_t(r) * COLS];
            for (int c = 0; c < COLS; ++c)
                line[c] = colorFor(vals[c]);
        }
        update();
    }

    void rebuildLut() {
        struct Stop {
            double t;
            int r, g, b;
        };
        static const std::vector<std::vector<Stop>> themes = {
            // ProdMesh: navy -> blue -> teal -> yellow -> red
            {{0.00, 0x10, 0x13, 0x1a}, {0.25, 0x1b, 0x3a, 0x6b},
             {0.50, 0x1f, 0x9e, 0x89}, {0.75, 0xe8, 0xc8, 0x4b},
             {1.00, 0xe0, 0x52, 0x3c}},
            // Green-Red: dark -> green -> yellow -> red -> white at max
            {{0.00, 0x05, 0x12, 0x08}, {0.30, 0x2f, 0xbf, 0x40},
             {0.55, 0xe8, 0xc8, 0x4b}, {0.80, 0xe0, 0x40, 0x2c},
             {1.00, 0xff, 0xff, 0xff}},
            // Inferno (matplotlib, approximated)
            {{0.00, 0, 0, 4},
             {0.20, 40, 11, 84},
             {0.40, 101, 21, 110},
             {0.60, 187, 55, 84},
             {0.80, 249, 142, 9},
             {1.00, 252, 255, 164}},
            // Viridis (matplotlib, approximated)
            {{0.00, 68, 1, 84},
             {0.25, 59, 82, 139},
             {0.50, 33, 145, 140},
             {0.75, 94, 201, 98},
             {1.00, 253, 231, 37}},
            // Rainbow (jet-style, REW-like)
            {{0.00, 0, 0, 40},
             {0.15, 0, 0, 200},
             {0.40, 0, 220, 220},
             {0.65, 255, 255, 0},
             {0.90, 250, 0, 0},
             {1.00, 130, 0, 0}},
            // Grayscale
            {{0.00, 0, 0, 0}, {1.00, 255, 255, 255}},
        };
        const auto &stops = themes[size_t(m_theme)];
        for (int i = 0; i < 256; ++i) {
            const double t = i / 255.0;
            size_t s = 1;
            while (s + 1 < stops.size() && t > stops[s].t)
                ++s;
            const double u =
                (t - stops[s - 1].t) / (stops[s].t - stops[s - 1].t);
            const double uc = std::clamp(u, 0.0, 1.0);
            m_lut[i] =
                qRgb(int(stops[s - 1].r + uc * (stops[s].r - stops[s - 1].r)),
                     int(stops[s - 1].g + uc * (stops[s].g - stops[s - 1].g)),
                     int(stops[s - 1].b + uc * (stops[s].b - stops[s - 1].b)));
        }
    }

    QImage m_img;
    std::vector<float> m_vals;
    QRgb m_lut[256];
    int m_theme = 0;
    double m_range = 100.0;
    double m_sens = 0.0;
};

// ---------------------------------------------------------------------------
// Strip chart of the last 10 minutes of SPL (Fast faint, Slow bright).

class HistoryWidget : public QWidget {
public:
    static constexpr qint64 SPAN_MS = 10 * 60 * 1000;

    HistoryWidget() {
        setMinimumHeight(110);
        setMaximumHeight(160);
    }

    void push(qint64 tMs, double fastDb, double slowDb) {
        m_pts.push_back({tMs, fastDb, slowDb});
        while (!m_pts.empty() && m_pts.front().t < tMs - SPAN_MS)
            m_pts.pop_front();
        update();
    }

    void setRange(double yMin, double yMax) {
        m_yMin = yMin;
        m_yMax = yMax;
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter qp(this);
        qp.fillRect(rect(), theme::bg);
        const int left = 40, right = 10, top = 6, bottom = 16;
        const int w = width() - left - right;
        const int h = height() - top - bottom;
        if (w <= 10 || h <= 10)
            return;
        const double span = m_yMax - m_yMin;

        QFont f = font();
        f.setPointSize(8);
        qp.setFont(f);
        for (double g = std::ceil(m_yMin / 20.0) * 20.0; g <= m_yMax; g += 20.0) {
            const int y = int(top + h * (1.0 - (g - m_yMin) / span));
            qp.setPen(QPen(theme::grid, 1));
            qp.drawLine(left, y, left + w, y);
            qp.setPen(theme::text);
            qp.drawText(QRect(0, y - 8, left - 6, 16),
                        Qt::AlignRight | Qt::AlignVCenter,
                        QString::number(g, 'f', 0));
        }
        qp.setPen(theme::text);
        qp.drawText(QRect(left, height() - bottom + 1, w, 14), Qt::AlignLeft,
                    "-10 min");
        qp.drawText(QRect(left, height() - bottom + 1, w, 14), Qt::AlignRight,
                    "now");

        if (!m_pts.empty()) {
            const qint64 now = m_pts.back().t;
            auto xOf = [&](qint64 t) {
                return left + w * (1.0 - double(now - t) / SPAN_MS);
            };
            auto yOf = [&](double db) {
                return top + h * (1.0 - (std::clamp(db, m_yMin, m_yMax) - m_yMin) / span);
            };
            drawSeries(qp, xOf, yOf, /*fast=*/true, QPen(theme::faint, 1));
            drawSeries(qp, xOf, yOf, /*fast=*/false, QPen(theme::barTop, 2));
        }

        qp.setPen(QPen(theme::grid, 1));
        qp.setBrush(Qt::NoBrush);
        qp.drawRect(left, top, w, h);
    }

private:
    struct Pt {
        qint64 t;
        double fast, slow;
    };

    template <typename FX, typename FY>
    void drawSeries(QPainter &qp, FX xOf, FY yOf, bool fast, const QPen &pen) {
        qp.setPen(pen);
        QPainterPath path;
        bool started = false;
        for (const Pt &p : m_pts) {
            const double v = fast ? p.fast : p.slow;
            if (!std::isfinite(v)) {
                started = false;
                continue;
            }
            const QPointF pt(xOf(p.t), yOf(v));
            if (!started) {
                path.moveTo(pt);
                started = true;
            } else {
                path.lineTo(pt);
            }
        }
        qp.drawPath(path);
    }

    std::deque<Pt> m_pts;
    double m_yMin = 20.0, m_yMax = 120.0;
};

// ---------------------------------------------------------------------------

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

    void set(const QString &caption, double value,
             const QString &suffix = QString()) {
        m_cap->setText(caption);
        m_val->setText(std::isfinite(value)
                           ? QString::number(value, 'f', 1) + suffix
                           : QString("--.-"));
    }

private:
    QLabel *m_cap;
    QLabel *m_val;
};

// One displayed metric: which, how to caption it, and its current value.
struct MetricDisplay {
    QString id;
    QString caption;
    double value = kNaN;
    QString suffix;  // e.g. "%" for dose metrics
};

// ---------------------------------------------------------------------------
// Metric breakout: a narrow vertical window of readouts meant to sit next
// to another app (e.g. Waves SuperRack). Each metric records its maximum;
// clicking the MAX line resets it.

class ClickableLabel : public QLabel {
public:
    using QLabel::QLabel;
    std::function<void()> onClick;

protected:
    void mousePressEvent(QMouseEvent *e) override {
        if (e->button() == Qt::LeftButton && onClick)
            onClick();
        QLabel::mousePressEvent(e);
    }
};

class MetricTile : public QWidget {
public:
    MetricTile() {
        setAttribute(Qt::WA_StyledBackground, true);
        setObjectName("metricTile");
        setStyleSheet("#metricTile { background:#20242e; "
                      "border:1px solid #2a2f3d; border-radius:6px; }");
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(14, 8, 14, 10);
        lay->setSpacing(0);
        m_cap = new QLabel;
        m_cap->setStyleSheet("color:#8a92a6; font-size:11px;");
        m_val = new QLabel("--.-");
        QFont f;
        f.setFamilies({"Consolas", "Menlo", "Courier New"});
        f.setPointSize(32);
        f.setBold(true);
        m_val->setFont(f);
        m_val->setStyleSheet("color:#e8ecf4;");
        m_max = new ClickableLabel("MAX --.-");
        m_max->setStyleSheet("color:#e0c05c; font-size:12px;");
        m_max->setCursor(Qt::PointingHandCursor);
        m_max->setToolTip("Highest value since last reset — click to reset");
        m_max->onClick = [this] { resetMax(); };
        lay->addWidget(m_cap);
        lay->addWidget(m_val);
        lay->addWidget(m_max);
    }

    void set(const QString &caption, double v, const QString &suffix) {
        m_cap->setText(caption);
        m_suffix = suffix;
        m_val->setText(fmt(v) + (std::isfinite(v) ? suffix : QString()));
        if (std::isfinite(v) && (!std::isfinite(m_maxVal) || v > m_maxVal)) {
            m_maxVal = v;
            m_max->setText("MAX " + fmt(m_maxVal) + suffix);
        }
    }

    void resetMax() {
        m_maxVal = kNaN;
        m_max->setText("MAX --.-");
    }

private:
    static QString fmt(double v) {
        return std::isfinite(v) ? QString::number(v, 'f', 1)
                                : QString("--.-");
    }

    double m_maxVal = kNaN;
    QString m_suffix;
    QLabel *m_cap;
    QLabel *m_val;
    ClickableLabel *m_max;
};

// Compact 10-minute SPL history strip for the breakout window.
class SparkTile : public QWidget {
public:
    static constexpr qint64 SPAN_MS = 10 * 60 * 1000;

    SparkTile() {
        setAttribute(Qt::WA_StyledBackground, true);
        setObjectName("metricTile");
        setStyleSheet("#metricTile { background:#20242e; "
                      "border:1px solid #2a2f3d; border-radius:6px; }");
        setMinimumHeight(88);
    }

    void push(qint64 t, double v) {
        if (std::isfinite(v))
            m_pts.push_back({t, v});
        while (!m_pts.empty() && m_pts.front().t < t - SPAN_MS)
            m_pts.pop_front();
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override {
        QPainter qp(this);
        QStyleOption opt;
        opt.initFrom(this);
        style()->drawPrimitive(QStyle::PE_Widget, &opt, &qp, this);

        QFont f = font();
        f.setPointSize(8);
        qp.setFont(f);
        qp.setPen(theme::text);
        qp.drawText(QRect(14, 6, width() - 28, 14), Qt::AlignLeft,
                    "SPL — 10 MIN");

        const int left = 14, right = 14, top = 24, bottom = 10;
        const int w = width() - left - right;
        const int h = height() - top - bottom;
        if (w <= 10 || h <= 10 || m_pts.size() < 2)
            return;
        double lo = m_pts.front().v, hi = lo;
        for (const Pt &p : m_pts) {
            lo = std::min(lo, p.v);
            hi = std::max(hi, p.v);
        }
        const double mid = (lo + hi) / 2;
        const double span = std::max(hi - lo, 10.0);  // at least 10 dB tall
        lo = mid - span / 2 - 2;
        hi = mid + span / 2 + 2;

        const qint64 now = m_pts.back().t;
        QPainterPath path;
        bool started = false;
        for (const Pt &p : m_pts) {
            const double x = left + w * (1.0 - double(now - p.t) / SPAN_MS);
            const double y =
                top + h * (1.0 - (p.v - lo) / (hi - lo));
            if (!started) {
                path.moveTo(x, y);
                started = true;
            } else {
                path.lineTo(x, y);
            }
        }
        qp.setRenderHint(QPainter::Antialiasing);
        qp.setPen(QPen(theme::barTop, 1.5));
        qp.drawPath(path);

        qp.setPen(theme::text);
        qp.drawText(QRect(left, top - 14, w, 14), Qt::AlignRight,
                    QString::number(hi, 'f', 0));
        qp.drawText(QRect(left, height() - bottom - 12, w, 12),
                    Qt::AlignRight, QString::number(lo, 'f', 0));
    }

private:
    struct Pt {
        qint64 t;
        double v;
    };
    std::deque<Pt> m_pts;
};

class BreakoutWindow : public QWidget {
public:
    std::function<void()> onClosed;

    explicit BreakoutWindow(QWidget *parent) : QWidget(parent, Qt::Window) {
        setAttribute(Qt::WA_StyledBackground, true);
        setWindowTitle("Metrics");
        setMinimumWidth(190);
        auto *lay = new QVBoxLayout(this);
        lay->setContentsMargins(8, 8, 8, 8);
        lay->setSpacing(6);
        m_tilesBox = new QWidget;
        m_tilesLay = new QVBoxLayout(m_tilesBox);
        m_tilesLay->setContentsMargins(0, 0, 0, 0);
        m_tilesLay->setSpacing(6);
        lay->addWidget(m_tilesBox);
        m_onTop = new QCheckBox("Always on top");
        QObject::connect(m_onTop, &QCheckBox::toggled, this, [this](bool on) {
            const bool vis = isVisible();
            setWindowFlag(Qt::WindowStaysOnTopHint, on);
            if (vis)
                show();  // changing flags hides the window
        });
        lay->addWidget(m_onTop);
        lay->addStretch(1);
    }

    // Rebuild the tile stack. "spark" is the SPL history sparkline; any
    // other id becomes a MetricTile fed by update(). Maxima reset.
    void setTiles(const QStringList &ids) {
        for (MetricTile *t : m_tiles)
            delete t;
        m_tiles.clear();
        delete m_spark;
        m_spark = nullptr;
        for (const QString &id : ids) {
            if (id == "spark") {
                m_spark = new SparkTile;
                m_tilesLay->addWidget(m_spark);
            } else {
                auto *t = new MetricTile;
                m_tiles.insert(id, t);
                m_tilesLay->addWidget(t);
            }
        }
    }

    void updateMetrics(const std::vector<MetricDisplay> &vals) {
        for (const MetricDisplay &v : vals)
            if (MetricTile *t = m_tiles.value(v.id))
                t->set(v.caption, v.value, v.suffix);
    }

    void pushSpark(qint64 t, double splDb) {
        if (m_spark)
            m_spark->push(t, splDb);
    }

    void resetMaxima() {
        for (MetricTile *t : m_tiles)
            t->resetMax();
    }

    bool alwaysOnTop() const { return m_onTop->isChecked(); }
    void setAlwaysOnTop(bool on) { m_onTop->setChecked(on); }

protected:
    void closeEvent(QCloseEvent *e) override {
        if (onClosed)
            onClosed();
        QWidget::closeEvent(e);
    }

private:
    QWidget *m_tilesBox;
    QVBoxLayout *m_tilesLay;
    QHash<QString, MetricTile *> m_tiles;
    SparkTile *m_spark = nullptr;
    QCheckBox *m_onTop;
};
