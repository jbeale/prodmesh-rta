// Display widgets: RTA bars, spectrogram, SPL history strip, big readouts.
#pragma once

#include <QColor>
#include <QFont>
#include <QImage>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QVBoxLayout>
#include <QWidget>

#include <cstring>
#include <deque>

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
            const double x0 = left + i * slot + (slot - barW) / 2;
            const QString lbl = xAxisLabel(THIRD_OCT_CENTERS[i]);
            if (!lbl.isEmpty()) {
                qp.setPen(theme::text);
                qp.drawText(QRect(int(left + i * slot - slot),
                                  height() - bottom + 4, int(slot * 3), 16),
                            Qt::AlignHCenter, lbl);
            }
            if (i >= int(m_bands.size()) || !std::isfinite(m_bands[i]))
                continue;
            const double v = std::clamp(m_bands[i], m_yMin, m_yMax);
            const int y = int(yOf(v));
            qp.setPen(Qt::NoPen);
            qp.setBrush(theme::bar);
            qp.drawRect(int(x0), y, int(barW), top + h - y);
            qp.setBrush(theme::barTop);
            qp.drawRect(int(x0), y, int(barW), 2);
            if (i < int(m_peaks.size()) && std::isfinite(m_peaks[i])) {
                const double pv = std::clamp(m_peaks[i], m_yMin, m_yMax);
                const int py = int(yOf(pv));
                qp.setPen(QPen(theme::peak, 2));
                qp.drawLine(int(x0), py, int(x0 + barW), py);
            }
        }

        qp.setPen(QPen(theme::grid, 1));
        qp.setBrush(Qt::NoBrush);
        qp.drawRect(left, top, w, h);
    }

private:
    std::vector<double> m_bands;
    std::vector<double> m_peaks;
    double m_yMin = 20.0, m_yMax = 120.0;
};

// ---------------------------------------------------------------------------
// Scrolling log-frequency spectrogram. Internal image is COLS x ROWS; each
// pushed column covers one UPDATE_MS tick, so the view spans ~30 s.

class SpectrogramWidget : public QWidget {
public:
    static constexpr int COLS = 600;
    static constexpr int ROWS = 200;
    static constexpr double OCTAVES = 10.0;  // 20 Hz .. ~20.5 kHz

    SpectrogramWidget() : m_img(COLS, ROWS, QImage::Format_RGB32) {
        m_img.fill(heatColor(0.0));
        setMinimumHeight(240);
    }

    void pushColumn(const std::vector<double> &power, double df, double cal) {
        if (power.empty() || df <= 0)
            return;
        for (int r = 0; r < ROWS; ++r) {
            QRgb *line = reinterpret_cast<QRgb *>(m_img.scanLine(r));
            std::memmove(line, line + 1, (COLS - 1) * sizeof(QRgb));
        }
        const int n = int(power.size());
        for (int r = 0; r < ROWS; ++r) {
            const double frac = double(ROWS - 1 - r) / (ROWS - 1);  // 0 bottom
            const double f = 20.0 * std::pow(2.0, frac * OCTAVES);
            int k = int(std::lround(f / df));
            k = std::clamp(k, 0, n - 1);
            const double db = toDb(power[k]) + cal;
            const double lo = cal - 80.0;
            double t = (db - lo) / 100.0;  // color range [cal-80, cal+20]
            t = std::clamp(t, 0.0, 1.0);
            reinterpret_cast<QRgb *>(m_img.scanLine(r))[COLS - 1] = heatColor(t);
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
    static QRgb heatColor(double t) {
        struct Stop {
            double t;
            int r, g, b;
        };
        static const Stop stops[] = {
            {0.00, 0x10, 0x13, 0x1a}, {0.25, 0x1b, 0x3a, 0x6b},
            {0.50, 0x1f, 0x9e, 0x89}, {0.75, 0xe8, 0xc8, 0x4b},
            {1.00, 0xe0, 0x52, 0x3c},
        };
        for (int i = 1; i < 5; ++i) {
            if (t <= stops[i].t) {
                const double u =
                    (t - stops[i - 1].t) / (stops[i].t - stops[i - 1].t);
                const int r =
                    int(stops[i - 1].r + u * (stops[i].r - stops[i - 1].r));
                const int g =
                    int(stops[i - 1].g + u * (stops[i].g - stops[i - 1].g));
                const int b =
                    int(stops[i - 1].b + u * (stops[i].b - stops[i - 1].b));
                return qRgb(r, g, b);
            }
        }
        return qRgb(stops[4].r, stops[4].g, stops[4].b);
    }

    QImage m_img;
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
