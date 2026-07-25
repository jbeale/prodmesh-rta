// ProdMesh Remote RTA — a minimal cross-platform (Windows/macOS) SPL meter
// and 1/3-octave real-time analyzer with spectrogram, SPL history, and an
// HTTP/WebSocket API for remote monitoring (e.g. by ProdMesh).
//
// See dsp.h (analysis), audio.h (capture), widgets.h (views), api.h (API).

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QStandardPaths>
#include <QTextStream>

#include <functional>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHostAddress>
#include <QIcon>
#include <QLabel>
#include <QNetworkInterface>
#include <QMainWindow>
#include <QMediaDevices>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QSplitter>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>

#include "api.h"
#include "audio.h"
#include "dsp.h"
#include "metrics.h"
#include "widgets.h"

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static const char *APP_NAME = "ProdMesh Remote RTA";

// Application-wide dark theme (on the Fusion style) — one place instead of
// scattered per-widget stylesheets so dialogs and menus stay consistent.
static const char *APP_STYLE = R"(
QWidget { background: #1a1d24; color: #c8cede; }
QLabel, QCheckBox { background: transparent; }
/* QComboBox / QSpinBox boxes are deliberately unstyled: any QSS box rule
   stops Fusion from drawing its (crisp, palette-aware) arrow glyphs. Their
   colors come from the QPalette set in main(). */
QComboBox QAbstractItemView {
    background: #232733; border: 1px solid #46506a;
    selection-background-color: #2a2e39; selection-color: #e8ecf4; outline: 0;
}
QPushButton {
    background: #2a2f3d; border: 1px solid #323848; border-radius: 4px;
    padding: 4px 14px;
}
QPushButton:hover { background: #323848; border-color: #46506a; }
QPushButton:pressed { background: #232733; }
QPushButton:disabled { color: #5a6172; }
QCheckBox::indicator {
    width: 14px; height: 14px; border: 1px solid #46506a;
    border-radius: 3px; background: #232733;
}
QCheckBox::indicator:hover { border-color: #2fbf9b; }
QCheckBox::indicator:checked { background: #2fbf9b; border-color: #2fbf9b; }
QTabWidget::pane { border: 1px solid #2a2e39; }
QTabBar::tab {
    background: #20242e; color: #8a92a6; padding: 5px 16px;
    border: 1px solid #2a2e39; border-bottom: none;
    border-top-left-radius: 4px; border-top-right-radius: 4px;
}
QTabBar::tab:selected { background: #2a2e39; color: #e8ecf4; }
QTabBar::tab:hover:!selected { color: #c8cede; }
QMenuBar { background: #1a1d24; }
QMenuBar::item { background: transparent; padding: 4px 10px; }
QMenuBar::item:selected { background: #2a2e39; border-radius: 4px; }
QMenu { background: #232733; border: 1px solid #46506a; }
QMenu::item { background: transparent; padding: 4px 24px; }
QMenu::item:selected { background: #2a2e39; }
QMenu::separator { height: 1px; background: #323848; margin: 4px 8px; }
QStatusBar { color: #8a92a6; }
QToolTip { background: #232733; color: #c8cede; border: 1px solid #46506a; }
)";

// What the app is measuring. Acoustic is a microphone in a room, where every
// level is anchored to 20 uPa by the cal offset. Program is a digital bus (a
// stream or console feed), where full scale is the reference, there is
// nothing to calibrate, and loudness replaces the exposure metrics.
enum AppMode { ModeAcoustic = 0, ModeProgram = 1 };

static constexpr int kAcousticOnly = 1;
static constexpr int kProgramOnly = 2;
static constexpr int kBothModes = kAcousticOnly | kProgramOnly;

// Metric registry: ids for config/persistence/API, names for the settings
// dialog, and which modes the metric means anything in. Captions, values, and
// suffixes are resolved by MainWindow since they depend on the display
// weighting and configured Leq windows.
struct MetricInfo {
    const char *id;
    const char *name;
    int modes;  // bitmask of kAcousticOnly / kProgramOnly
};
static const MetricInfo kMetricInfos[] = {
    // Levels are just levels: in Program mode cal is 0, so they read dBFS.
    {"laf", "Fast (displayed weighting)", kBothModes},
    {"las", "Slow (displayed weighting)", kBothModes},
    {"leq", "Leq — session", kBothModes},
    {"lzpk", "Peak (Z, unweighted)", kBothModes},
    {"leqS", "LAeq — short window", kAcousticOnly},
    {"leqL", "LAeq — long window", kAcousticOnly},
    {"lcpk", "Peak (C-weighted)", kAcousticOnly},
    {"ca", "C-A ratio (short window)", kAcousticOnly},
    {"l10", "L10 — session", kAcousticOnly},
    {"l50", "L50 — session", kAcousticOnly},
    {"l90", "L90 — session", kAcousticOnly},
    {"doseN", "Dose (NIOSH 85/3)", kAcousticOnly},
    {"doseO", "Dose (OSHA 90/5)", kAcousticOnly},
    {"lufsM", "Momentary loudness (400 ms)", kProgramOnly},
    {"lufsS", "Short-term loudness (3 s)", kProgramOnly},
    {"lufsI", "Integrated loudness (gated)", kProgramOnly},
    {"toTarget", "Distance to target", kProgramOnly},
    {"dbtp", "True peak (400 ms)", kProgramOnly},
    {"dbtpMax", "True peak — session max", kProgramOnly},
    {"plr", "Peak to loudness ratio", kProgramOnly},
};

static bool metricInMode(const MetricInfo &mi, int mode) {
    return (mi.modes &
            (mode == ModeProgram ? kProgramOnly : kAcousticOnly)) != 0;
}

// Delivery targets. Streaming platforms normalise *down* to their target, so
// going louder than it only costs dynamics; broadcast specs are two-sided.
struct LoudnessTarget {
    const char *name;
    double lufs;
    double ceilDbtp;
};
static const LoudnessTarget kLoudnessTargets[] = {
    {"YouTube / Spotify / Twitch (-14 LUFS)", -14.0, -1.0},
    {"Apple Podcasts (-16 LUFS)", -16.0, -1.0},
    {"EBU R128 broadcast (-23 LUFS)", -23.0, -1.0},
    {"ATSC A/85 (-24 LKFS)", -24.0, -2.0},
    {"Custom", -14.0, -1.0},
};
static constexpr int kCustomTargetIdx =
    int(sizeof(kLoudnessTargets) / sizeof(kLoudnessTargets[0])) - 1;

static QString windowLabel(int secs) {
    if (secs % 3600 == 0)
        return QString("%1 h").arg(secs / 3600);
    if (secs % 60 == 0)
        return QString("%1 min").arg(secs / 60);
    return QString("%1 s").arg(secs);
}

// Parse a measurement-mic calibration file: text lines of "<freq Hz> <dB>"
// (whitespace separated; REW / miniDSP style). Lines that don't start with
// two numbers (comments, "Sens Factor" headers, quotes) are skipped.
static bool parseMicCorrection(const QString &path,
                               std::vector<std::pair<double, double>> &points,
                               QString &error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) {
        error = "cannot open file";
        return false;
    }
    QTextStream in(&f);
    points.clear();
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty())
            continue;
        const QStringList tok =
            line.split(QRegularExpression("[\\s,;]+"), Qt::SkipEmptyParts);
        if (tok.size() < 2)
            continue;
        bool okF = false, okD = false;
        const double freq = tok[0].toDouble(&okF);
        const double db = tok[1].toDouble(&okD);
        if (!okF || !okD || freq <= 0.0)
            continue;
        points.emplace_back(freq, db);
    }
    if (points.size() < 2) {
        error = "no usable \"<frequency> <dB>\" data lines found";
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------

class CalibrationDialog : public QDialog {
public:
    CalibrationDialog(QWidget *parent, std::function<double()> slowDbfs,
                      double currentCal)
        : QDialog(parent), m_slowDbfs(std::move(slowDbfs)),
          m_currentCal(currentCal) {
        setWindowTitle("Calibrate SPL");
        auto *form = new QFormLayout(this);

        refSpin = new QDoubleSpinBox;
        refSpin->setRange(30.0, 140.0);
        refSpin->setDecimals(1);
        refSpin->setValue(94.0);
        refSpin->setSuffix(" dB SPL");
        form->addRow("Reference level:", refSpin);

        m_liveLbl = new QLabel("--.-");
        form->addRow("Currently reading:", m_liveLbl);

        m_captureBtn = new QPushButton("Capture");
        form->addRow(m_captureBtn);

        m_resultLbl = new QLabel(
            "Put the mic on a calibrator (or play steady pink noise measured\n"
            "by a trusted meter), enter that level above, then press Capture.\n"
            "The level is averaged for ~1.5 s.");
        m_resultLbl->setStyleSheet("color:#8a92a6; font-size:11px;");
        form->addRow(m_resultLbl);

        m_buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                         QDialogButtonBox::Cancel);
        m_buttons->button(QDialogButtonBox::Ok)->setText("Apply");
        m_buttons->button(QDialogButtonBox::Ok)->setEnabled(false);
        connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        form->addRow(m_buttons);

        connect(m_captureBtn, &QPushButton::clicked, this, [this] {
            m_capturing = true;
            m_accumPower = 0.0;
            m_accumCount = 0;
            m_capturePolls = 0;
            m_captureBtn->setEnabled(false);
            m_resultLbl->setText("Capturing…");
        });

        auto *poll = new QTimer(this);
        poll->setInterval(100);
        connect(poll, &QTimer::timeout, this, [this] { onPoll(); });
        poll->start();
    }

    double newCal() const { return m_newCal; }

    QDoubleSpinBox *refSpin;

private:
    void onPoll() {
        const double dbfs = m_slowDbfs();
        m_liveLbl->setText(
            std::isfinite(dbfs)
                ? QString("%1 dB SPL (with current cal)")
                      .arg(dbfs + m_currentCal, 0, 'f', 1)
                : QString("no signal"));
        if (!m_capturing)
            return;
        ++m_capturePolls;
        if (std::isfinite(dbfs)) {
            m_accumPower += std::pow(10.0, dbfs / 10.0);
            ++m_accumCount;
        }
        if (m_accumCount >= 15) {
            m_capturing = false;
            m_captureBtn->setEnabled(true);
            const double avgDbfs = 10.0 * std::log10(m_accumPower / m_accumCount);
            m_newCal = refSpin->value() - avgDbfs;
            m_resultLbl->setText(
                QString("Measured %1 dBFS -> new cal %2 dB (was %3 dB).\n"
                        "Press Apply to use it.")
                    .arg(avgDbfs, 0, 'f', 1)
                    .arg(m_newCal, 0, 'f', 1)
                    .arg(m_currentCal, 0, 'f', 1));
            m_buttons->button(QDialogButtonBox::Ok)->setEnabled(true);
        } else if (m_capturePolls > 50) {  // ~5 s without enough signal
            m_capturing = false;
            m_captureBtn->setEnabled(true);
            m_resultLbl->setText("No signal — check the selected input.");
        }
    }

    std::function<double()> m_slowDbfs;
    double m_currentCal;
    double m_newCal = kNaN;
    bool m_capturing = false;
    double m_accumPower = 0.0;
    int m_accumCount = 0;
    int m_capturePolls = 0;
    QLabel *m_liveLbl;
    QLabel *m_resultLbl;
    QPushButton *m_captureBtn;
    QDialogButtonBox *m_buttons;
};

// ---------------------------------------------------------------------------

class MetricsDialog : public QDialog {
public:
    MetricsDialog(QWidget *parent, int mode, const QStringList &mainIds,
                  const QStringList &breakoutIds, int shortS, int longS,
                  const QHash<QString, QPair<double, double>> &targets,
                  int sizeIdx)
        : QDialog(parent) {
        setWindowTitle("Metrics");
        auto *root = new QVBoxLayout(this);

        auto *grid = new QGridLayout;
        grid->setHorizontalSpacing(18);
        grid->addWidget(new QLabel("<b>Metric</b>"), 0, 0);
        grid->addWidget(new QLabel("<b>Top bar</b>"), 0, 1);
        grid->addWidget(new QLabel("<b>Breakout</b>"), 0, 2);
        grid->addWidget(new QLabel("<b>Target range</b>"), 0, 3);
        int row = 1;
        for (const MetricInfo &mi : kMetricInfos) {
            if (!metricInMode(mi, mode))
                continue;
            grid->addWidget(new QLabel(mi.name), row, 0);
            auto *cm = new QCheckBox;
            cm->setChecked(mainIds.contains(mi.id));
            auto *cb = new QCheckBox;
            cb->setChecked(breakoutIds.contains(mi.id));
            grid->addWidget(cm, row, 1, Qt::AlignHCenter);
            grid->addWidget(cb, row, 2, Qt::AlignHCenter);

            Row r{mi.id, cm, cb, new QCheckBox, makeSpin(), makeSpin()};
            const auto it = targets.constFind(mi.id);
            if (it != targets.constEnd()) {
                r.tgt->setChecked(true);
                r.lo->setValue(it->first);
                r.hi->setValue(it->second);
            }
            r.lo->setEnabled(r.tgt->isChecked());
            r.hi->setEnabled(r.tgt->isChecked());
            connect(r.tgt, &QCheckBox::toggled, this,
                    [lo = r.lo, hi = r.hi](bool on) {
                        lo->setEnabled(on);
                        hi->setEnabled(on);
                    });
            auto *box = new QHBoxLayout;
            box->setSpacing(4);
            box->addWidget(r.tgt);
            box->addWidget(r.lo);
            box->addWidget(new QLabel("–"));
            box->addWidget(r.hi);
            grid->addLayout(box, row, 3);
            m_rows.push_back(r);
            ++row;
        }
        grid->addWidget(new QLabel("SPL history sparkline"), row, 0);
        m_sparkCheck = new QCheckBox;
        m_sparkCheck->setChecked(breakoutIds.contains("spark"));
        grid->addWidget(m_sparkCheck, row, 2, Qt::AlignHCenter);
        root->addLayout(grid);

        auto *form = new QFormLayout;
        m_shortCombo = new QComboBox;
        for (int s : {10, 30, 60, 300})
            m_shortCombo->addItem(windowLabel(s), s);
        selectData(m_shortCombo, shortS);
        m_longCombo = new QComboBox;
        for (int s : {300, 600, 900, 1800, 3600})
            m_longCombo->addItem(windowLabel(s), s);
        selectData(m_longCombo, longS);
        // The rolling Leq windows only feed acoustic metrics; loudness
        // integration times are fixed by BS.1770.
        if (mode != ModeProgram) {
            form->addRow("Short Leq window:", m_shortCombo);
            form->addRow("Long Leq window:", m_longCombo);
        }
        m_sizeCombo = new QComboBox;
        m_sizeCombo->addItems({"Small", "Medium", "Large"});
        m_sizeCombo->setCurrentIndex(std::clamp(sizeIdx, 0, 2));
        form->addRow("Breakout number size:", m_sizeCombo);
        root->addLayout(form);

        auto *hint = new QLabel(
            mode == ModeProgram
                ? "Integrated loudness and the true-peak maximum are session\n"
                  "metrics — they reset with the Reset button, so start a\n"
                  "fresh session when the stream starts.\n"
                  "Target ranges draw a gauge under the readout: green in\n"
                  "the band, amber outside. E.g. Integrated -15 to -13 LUFS\n"
                  "keeps a stream inside YouTube's normalisation window."
                : "Session metrics (Leq, L10/L50/L90, dose) reset with the\n"
                  "Reset button. Dose assumes the Cal offset gives true dB\n"
                  "SPL. Target ranges draw a gauge under the readout: green\n"
                  "in the band, amber outside. E.g. C-A ratio 8–12 dB keeps\n"
                  "low-end energy in the range that avoids \"it's too loud\"\n"
                  "complaints.");
        hint->setStyleSheet("color:#8a92a6; font-size:11px;");
        root->addWidget(hint);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        root->addWidget(buttons);
    }

    QStringList mainIds() const {
        QStringList out;
        for (const Row &r : m_rows)
            if (r.main->isChecked())
                out << r.id;
        return out;
    }

    QStringList breakoutIds() const {
        QStringList out;
        for (const Row &r : m_rows)
            if (r.brk->isChecked())
                out << r.id;
        if (m_sparkCheck->isChecked())
            out << "spark";
        return out;
    }

    int shortSecs() const { return m_shortCombo->currentData().toInt(); }
    int longSecs() const { return m_longCombo->currentData().toInt(); }
    int sizeIdx() const { return m_sizeCombo->currentIndex(); }

    QHash<QString, QPair<double, double>> targets() const {
        QHash<QString, QPair<double, double>> out;
        for (const Row &r : m_rows)
            if (r.tgt->isChecked() && r.hi->value() > r.lo->value())
                out.insert(r.id, {r.lo->value(), r.hi->value()});
        return out;
    }

private:
    static void selectData(QComboBox *combo, int value) {
        const int i = combo->findData(value);
        if (i >= 0)
            combo->setCurrentIndex(i);
    }

    static QDoubleSpinBox *makeSpin() {
        auto *s = new QDoubleSpinBox;
        s->setRange(-70.0, 140.0);  // spans dB SPL and LUFS/dBTP
        s->setDecimals(1);
        s->setFixedWidth(72);
        return s;
    }

    struct Row {
        QString id;
        QCheckBox *main;
        QCheckBox *brk;
        QCheckBox *tgt;
        QDoubleSpinBox *lo;
        QDoubleSpinBox *hi;
    };
    std::vector<Row> m_rows;
    QCheckBox *m_sparkCheck;
    QComboBox *m_shortCombo;
    QComboBox *m_longCombo;
    QComboBox *m_sizeCombo;
};

// ---------------------------------------------------------------------------

class AlarmsDialog : public QDialog {
public:
    AlarmsDialog(QWidget *parent, int mode, bool enabled,
                 const QString &metricId, double warnDb, double alertDb)
        : QDialog(parent) {
        const bool program = mode == ModeProgram;
        setWindowTitle(program ? "Loudness Alarms" : "SPL Alarms");
        auto *form = new QFormLayout(this);

        enableCheck = new QCheckBox("Color readouts when levels get high");
        enableCheck->setChecked(enabled);
        form->addRow("Alarms:", enableCheck);

        metricCombo = new QComboBox;
        for (const MetricInfo &mi : kMetricInfos)
            if (metricInMode(mi, mode))
                metricCombo->addItem(mi.name, QString(mi.id));
        const int i = metricCombo->findData(metricId);
        if (i >= 0)
            metricCombo->setCurrentIndex(i);
        form->addRow("Watch metric:", metricCombo);

        // Loudness thresholds are negative (LUFS/dBTP); SPL ones are not.
        const double lo = program ? -60.0 : 40.0;
        warnSpin = new QDoubleSpinBox;
        warnSpin->setRange(lo, 140.0);
        warnSpin->setDecimals(1);
        warnSpin->setSuffix(program ? "" : " dB");
        warnSpin->setValue(warnDb);
        form->addRow("Warning (yellow) at:", warnSpin);

        alertSpin = new QDoubleSpinBox;
        alertSpin->setRange(lo, 140.0);
        alertSpin->setDecimals(1);
        alertSpin->setSuffix(program ? "" : " dB");
        alertSpin->setValue(alertDb);
        form->addRow("Alert (red) at:", alertSpin);

        auto *hint = new QLabel(
            program
                ? "Traffic-light coloring on the watched metric, everywhere\n"
                  "it is displayed (top bar, breakout, web dashboard, API).\n"
                  "For a -14 LUFS stream: warn at -12, alert at -9 on the\n"
                  "short-term level, or watch true peak against -1 dBTP."
                : "Traffic-light coloring on the watched metric, everywhere\n"
                  "it is displayed (top bar, breakout, web dashboard, API).\n"
                  "Typical show limits: warn 95-99, alert 102-103 dBA LAeq.");
        hint->setStyleSheet("color:#8a92a6; font-size:11px;");
        form->addRow(hint);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        form->addRow(buttons);
    }

    QCheckBox *enableCheck;
    QComboBox *metricCombo;
    QDoubleSpinBox *warnSpin;
    QDoubleSpinBox *alertSpin;
};

// ---------------------------------------------------------------------------

class ApiSettingsDialog : public QDialog {
public:
    ApiSettingsDialog(QWidget *parent, bool enabled, int port, int rateIdx,
                      const QString &bind)
        : QDialog(parent) {
        setWindowTitle("API Settings");
        auto *form = new QFormLayout(this);

        enableCheck = new QCheckBox("Serve levels to other machines");
        enableCheck->setChecked(enabled);
        form->addRow("HTTP API:", enableCheck);

        portSpin = new QSpinBox;
        portSpin->setRange(1024, 65535);
        portSpin->setValue(port);
        form->addRow("Port:", portSpin);

        // Multi-NIC FOH machines (Dante / SoundGrid / control / internet):
        // bind the server to one network so audio-transport LANs stay clean.
        ifaceCombo = new QComboBox;
        ifaceCombo->addItem("All interfaces", QString());
        ifaceCombo->addItem("Localhost only (127.0.0.1)",
                            QString("127.0.0.1"));
        for (const QNetworkInterface &ni : QNetworkInterface::allInterfaces()) {
            if (!(ni.flags() & QNetworkInterface::IsUp))
                continue;
            for (const QNetworkAddressEntry &e : ni.addressEntries()) {
                const QHostAddress a = e.ip();
                if (a.protocol() != QAbstractSocket::IPv4Protocol ||
                    a.isLoopback())
                    continue;
                ifaceCombo->addItem(
                    QString("%1 — %2").arg(ni.humanReadableName(),
                                           a.toString()),
                    a.toString());
            }
        }
        int bi = ifaceCombo->findData(bind);
        if (bi < 0 && !bind.isEmpty()) {  // saved NIC not present right now
            ifaceCombo->addItem(QString("(saved) %1").arg(bind), bind);
            bi = ifaceCombo->count() - 1;
        }
        ifaceCombo->setCurrentIndex(std::max(0, bi));
        ifaceCombo->setToolTip(
            "Which network the API listens on. Pick your control LAN to "
            "keep it off Dante / SoundGrid networks.");
        form->addRow("Interface:", ifaceCombo);

        rateCombo = new QComboBox;
        rateCombo->addItems({"1 Hz", "5 Hz", "10 Hz", "20 Hz"});
        rateCombo->setCurrentIndex(rateIdx);
        rateCombo->setToolTip("How often /api/stream WebSocket clients "
                              "receive a levels message.");
        form->addRow("Stream rate:", rateCombo);

        auto *hint = new QLabel(
            "Live browser dashboard: http://<this machine>:<port>/\n"
            "Endpoints: /api/status, /api/spl, /api/rta, /api/history\n"
            "WebSocket stream: ws://<this machine>:<port>/api/stream\n"
            "Allow the app through the firewall for LAN access.");
        hint->setStyleSheet("color:#8a92a6; font-size:11px;");
        form->addRow(hint);

        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok |
                                             QDialogButtonBox::Cancel);
        connect(buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
        form->addRow(buttons);
    }

    QString bindAddress() const {
        return ifaceCombo->currentData().toString();
    }

    QCheckBox *enableCheck;
    QSpinBox *portSpin;
    QComboBox *rateCombo;
    QComboBox *ifaceCombo;
};

// ---------------------------------------------------------------------------

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(APP_NAME);

        auto *central = new QWidget;
        setCentralWidget(central);
        auto *root = new QVBoxLayout(central);

        buildMenus();

        // --- controls row ---
        auto *ctl = new QHBoxLayout;
        ctl->addWidget(new QLabel("Input:"));
        m_deviceCombo = new QComboBox;
        m_deviceCombo->setMinimumWidth(280);
        ctl->addWidget(m_deviceCombo);
        ctl->addSpacing(6);
        ctl->addWidget(new QLabel("Ch:"));
        m_chanCombo = new QComboBox;
        m_chanCombo->setToolTip(
            "Which channel of the selected device feeds the analyzer —\n"
            "e.g. the interface input your measurement mic is plugged into.\n"
            "Mix averages all channels.");
        ctl->addWidget(m_chanCombo);
        ctl->addSpacing(12);
        ctl->addWidget(new QLabel("Weighting:"));
        m_weightCombo = new QComboBox;
        m_weightCombo->addItems({"A", "C", "Z"});
        ctl->addWidget(m_weightCombo);
        ctl->addSpacing(12);
        m_calLbl = new QLabel("Cal:");
        ctl->addWidget(m_calLbl);
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

        // --- SPL readouts (populated by rebuildReadouts from config) ---
        m_splRow = new QHBoxLayout;
        m_clipLbl = new QLabel("CLIP");
        setClipStyle(false);
        m_splRow->addWidget(m_clipLbl);
        m_splRow->addStretch(1);
        root->addLayout(m_splRow);

        // --- RTA / spectrogram tabs + SPL history strip ---
        m_rta = new RtaWidget;
        auto *rtaPage = new QWidget;
        auto *rtaLay = new QVBoxLayout(rtaPage);
        rtaLay->setContentsMargins(0, 4, 0, 0);
        rtaLay->addWidget(m_rta, 1);
        m_spectro = new SpectrogramWidget;
        auto *spectroPage = new QWidget;
        auto *spectroLay = new QVBoxLayout(spectroPage);
        spectroLay->setContentsMargins(0, 4, 0, 0);
        spectroLay->addWidget(m_spectro, 1);
        // Split view (Smaart-style): RTA stacked over a bottom-up spectrogram
        // so both share the frequency axis. The two widgets are reparented
        // in/out of the splitter on tab change — history travels with them.
        m_split = new QSplitter(Qt::Vertical);
        m_split->setChildrenCollapsible(false);
        // Program-mode loudness view. Built always, but only added to the tab
        // bar in Program mode (see applyMode).
        // Meter over its own RTA: "am I on target" and "what does it sound
        // like" answered in one view. A second RtaWidget rather than
        // reparenting the main one — they are cheap, and the tab machinery
        // stays simple.
        m_loudMeter = new LoudnessMeter;
        m_loudMeter->setMinimumHeight(130);
        m_loudRta = new RtaWidget;
        m_loudRta->setMinimumHeight(120);
        m_rtas = {m_rta, m_loudRta};
        m_loudPage = new QWidget(this);
        m_loudPage->hide();
        auto *loudLay = new QVBoxLayout(m_loudPage);
        loudLay->setContentsMargins(0, 4, 0, 0);
        auto *loudSplit = new QSplitter(Qt::Vertical);
        loudSplit->setChildrenCollapsible(false);
        loudSplit->addWidget(m_loudMeter);
        loudSplit->addWidget(m_loudRta);
        loudSplit->setStretchFactor(0, 2);
        loudSplit->setStretchFactor(1, 3);
        loudLay->addWidget(loudSplit, 1);
        m_tabs = new QTabWidget;
        m_tabs->addTab(rtaPage, "RTA");
        m_tabs->addTab(spectroPage, "Spectrogram");
        m_tabs->addTab(m_split, "Split");
        root->addWidget(m_tabs, 1);
        m_history = new HistoryWidget;
        root->addWidget(m_history);

        m_apiLbl = new QLabel;
        m_apiLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusBar()->addPermanentWidget(m_apiLbl);

        m_api = new ApiServer(this);

        m_breakout = new BreakoutWindow(this);
        m_breakout->onClosed = [this] { m_breakoutAct->setChecked(false); };

        // --- Display settings dialog (Settings -> Display…) ---
        // All view controls live here instead of inline rows above the
        // plots. Modeless, and every control applies live, so it can sit
        // open next to the plots while tuning.
        m_displayDlg = new QDialog(this);
        m_displayDlg->setWindowTitle("Display");
        auto *dispLay = new QVBoxLayout(m_displayDlg);
        auto *rtaGroup = new QGroupBox("RTA");
        auto *rtaForm = new QFormLayout(rtaGroup);
        m_rtaViewCombo = new QComboBox;
        m_rtaViewCombo->addItems({"Bars", "Line"});
        rtaForm->addRow("View:", m_rtaViewCombo);
        m_avgCombo = new QComboBox;
        m_avgCombo->addItems({"Off", "Fast (125 ms)", "Slow (1 s)", "2 s"});
        m_avgCombo->setCurrentIndex(1);
        rtaForm->addRow("Averaging:", m_avgCombo);
        m_rtaDecayCombo = new QComboBox;
        m_rtaDecayCombo->addItems(
            {"Off", "Slow (6 dB/s)", "Medium (12 dB/s)", "Fast (24 dB/s)"});
        m_rtaDecayCombo->setToolTip(
            "Bands rise instantly and fall at this rate.");
        rtaForm->addRow("Decay:", m_rtaDecayCombo);
        m_rtaRangeCombo = new QComboBox;
        m_rtaRangeCombo->addItems({"40 dB", "60 dB", "80 dB", "100 dB"});
        m_rtaRangeCombo->setCurrentIndex(2);
        m_rtaRangeCombo->setToolTip("Vertical span of the RTA scale.");
        rtaForm->addRow("Range:", m_rtaRangeCombo);
        m_rtaSensSpin = new QSpinBox;
        m_rtaSensSpin->setRange(-20, 60);
        m_rtaSensSpin->setValue(0);
        m_rtaSensSpin->setSuffix(" dB");
        m_rtaSensSpin->setToolTip(
            "Shifts the scale down so quieter band levels fill the plot.\n"
            "Per-band levels sit ~15 dB below the broadband SPL (pink noise\n"
            "spreads across ~31 bands), so 10-20 dB here is typical.");
        rtaForm->addRow("Sensitivity:", m_rtaSensSpin);
        m_peakCheck = new QCheckBox("Hold band maxima");
        rtaForm->addRow("Peak hold:", m_peakCheck);
        m_rtaGridCheck = new QCheckBox("Vertical lines at octave centers");
        rtaForm->addRow("Freq gridlines:", m_rtaGridCheck);
        dispLay->addWidget(rtaGroup);
        auto *spGroup = new QGroupBox("Spectrogram");
        auto *spForm = new QFormLayout(spGroup);
        m_spectroThemeCombo = new QComboBox;
        m_spectroThemeCombo->addItems(SpectrogramWidget::themeNames());
        spForm->addRow("Theme:", m_spectroThemeCombo);
        m_spectroRangeCombo = new QComboBox;
        m_spectroRangeCombo->addItems({"60 dB", "80 dB", "100 dB", "120 dB"});
        m_spectroRangeCombo->setCurrentIndex(2);
        m_spectroRangeCombo->setToolTip(
            "Dynamic range of the color scale — a smaller range gives more "
            "contrast.");
        spForm->addRow("Range:", m_spectroRangeCombo);
        m_spectroSensSpin = new QSpinBox;
        m_spectroSensSpin->setRange(-20, 40);
        m_spectroSensSpin->setValue(0);
        m_spectroSensSpin->setSuffix(" dB");
        m_spectroSensSpin->setToolTip(
            "Shifts the color scale: positive values light up quieter "
            "material.");
        spForm->addRow("Sensitivity:", m_spectroSensSpin);
        m_spectroSpanCombo = new QComboBox;
        m_spectroSpanCombo->addItems(
            {"10 s", "30 s", "1 min", "2 min", "5 min", "10 min"});
        m_spectroSpanCombo->setCurrentIndex(1);
        m_spectroSpanCombo->setToolTip(
            "History the spectrogram covers — longer spans scroll slower.\n"
            "Ticks are averaged into each column, so nothing is dropped.");
        spForm->addRow("Time span:", m_spectroSpanCombo);
        dispLay->addWidget(spGroup);
        auto *dispBtns = new QDialogButtonBox(QDialogButtonBox::Close);
        QObject::connect(dispBtns, &QDialogButtonBox::rejected, m_displayDlg,
                         &QWidget::hide);
        dispLay->addWidget(dispBtns);

        // --- Input & Mode dialog (Settings -> Input & Mode…) ---
        // The mode changes what the app *is*, so it lives in a dialog rather
        // than on the toolbar — nobody should flip it by brushing a control
        // mid-service.
        m_inputDlg = new QDialog(this);
        m_inputDlg->setWindowTitle("Input & Mode");
        auto *inLay = new QVBoxLayout(m_inputDlg);
        auto *modeGroup = new QGroupBox("Measurement mode");
        auto *modeForm = new QFormLayout(modeGroup);
        m_modeCombo = new QComboBox;
        m_modeCombo->addItem("Acoustic — microphone in a room (dB SPL)",
                             ModeAcoustic);
        m_modeCombo->addItem("Program — stream or console bus (LUFS)",
                             ModeProgram);
        modeForm->addRow("Mode:", m_modeCombo);
        auto *modeHint = new QLabel(
            "Acoustic anchors every level to the Cal offset for true dB SPL.\n"
            "Program meters a digital bus against full scale: no calibration,\n"
            "K-weighted loudness and true peak instead of Leq and dose.\n"
            "Route the programme in through an aggregate or loopback device\n"
            "so the bus arrives as an ordinary capture input.");
        modeHint->setStyleSheet("color:#8a92a6; font-size:11px;");
        modeForm->addRow(modeHint);
        inLay->addWidget(modeGroup);

        m_programGroup = new QGroupBox("Program mode");
        auto *progForm = new QFormLayout(m_programGroup);
        m_pairLCombo = new QComboBox;
        m_pairRCombo = new QComboBox;
        m_pairLCombo->setToolTip(
            "Which capture channels carry the stereo programme. Loudness is\n"
            "the BS.1770 sum of this pair; the RTA keeps using the Ch "
            "selector.");
        auto *pairRow = new QHBoxLayout;
        pairRow->setSpacing(4);
        pairRow->addWidget(new QLabel("L"));
        pairRow->addWidget(m_pairLCombo);
        pairRow->addSpacing(8);
        pairRow->addWidget(new QLabel("R"));
        pairRow->addWidget(m_pairRCombo);
        pairRow->addStretch(1);
        progForm->addRow("Stereo pair:", pairRow);
        m_targetCombo = new QComboBox;
        for (const LoudnessTarget &t : kLoudnessTargets)
            m_targetCombo->addItem(t.name);
        progForm->addRow("Delivery target:", m_targetCombo);
        m_targetSpin = new QDoubleSpinBox;
        m_targetSpin->setRange(-40.0, 0.0);
        m_targetSpin->setDecimals(1);
        m_targetSpin->setSingleStep(0.5);
        m_targetSpin->setSuffix(" LUFS");
        progForm->addRow("Target level:", m_targetSpin);
        m_ceilSpin = new QDoubleSpinBox;
        m_ceilSpin->setRange(-9.0, 0.0);
        m_ceilSpin->setDecimals(1);
        m_ceilSpin->setSingleStep(0.5);
        m_ceilSpin->setSuffix(" dBTP");
        m_ceilSpin->setToolTip(
            "True-peak ceiling. Lossy encoders overshoot, so -1 dBTP is the\n"
            "usual safe limit even when the sample peak looks clean.");
        progForm->addRow("True peak ceiling:", m_ceilSpin);
        inLay->addWidget(m_programGroup);
        auto *inBtns = new QDialogButtonBox(QDialogButtonBox::Close);
        QObject::connect(inBtns, &QDialogButtonBox::rejected, m_inputDlg,
                         &QWidget::hide);
        inLay->addWidget(inBtns);

        loadSettings();

        // --- wiring (after loadSettings so restores don't retrigger) ---
        populateDevices();
        QObject::connect(m_deviceCombo, &QComboBox::currentIndexChanged, this,
                         [this](int row) {
                             changeDevice(row);
                             saveSettings();
                         });
        QObject::connect(m_chanCombo, &QComboBox::currentIndexChanged, this,
                         [this](int) {
                             m_inputChannel = m_chanCombo->currentData().toInt();
                             m_engine.setChannel(m_inputChannel);
                             m_analyzer.resetAll();
                             m_metricsEng.resetAll();
                             saveSettings();
                         });
        QObject::connect(m_weightCombo, &QComboBox::currentTextChanged, this,
                         [this](const QString &w) {
                             m_analyzer.weighting =
                                 w.isEmpty() ? 'Z' : w[0].toLatin1();
                             m_analyzer.resetAll();
                             saveSettings();
                         });
        QObject::connect(m_avgCombo, &QComboBox::currentIndexChanged, this,
                         [this](int) { saveSettings(); });
        QObject::connect(m_peakCheck, &QCheckBox::toggled, this,
                         [this](bool) { saveSettings(); });
        QObject::connect(m_calSpin, &QDoubleSpinBox::valueChanged, this,
                         [this](double) { saveSettings(); });
        QObject::connect(m_rtaViewCombo, &QComboBox::currentIndexChanged, this,
                         [this](int i) {
                             for (RtaWidget *r : m_rtas)
                                 r->setViewMode(i);
                             saveSettings();
                         });
        QObject::connect(m_rtaDecayCombo, &QComboBox::currentIndexChanged, this,
                         [this](int i) {
                             for (RtaWidget *r : m_rtas)
                                 r->setDecayRate(rtaDecayRate(i));
                             saveSettings();
                         });
        QObject::connect(m_rtaRangeCombo, &QComboBox::currentIndexChanged,
                         this, [this](int) { saveSettings(); });
        QObject::connect(m_rtaSensSpin, &QSpinBox::valueChanged, this,
                         [this](int) { saveSettings(); });
        QObject::connect(m_rtaGridCheck, &QCheckBox::toggled, this,
                         [this](bool on) {
                             for (RtaWidget *r : m_rtas)
                                 r->setFreqGridlines(on);
                             saveSettings();
                         });
        QObject::connect(m_spectroSpanCombo, &QComboBox::currentIndexChanged,
                         this, [this](int i) {
                             m_spectro->setSpanSeconds(spectroSpanSeconds(i));
                             saveSettings();
                         });
        // Split view: hovering either plot mirrors the frequency cursor onto
        // the other (shared X axis). Frequencies, not pixels, cross the link,
        // so it stays honest whatever the layout.
        m_rta->onHoverFreq = [this](double f) {
            m_spectro->setLinkedCursor(f);
        };
        m_spectro->onHoverFreq = [this](double f) {
            m_rta->setLinkedCursor(f);
        };
        QObject::connect(m_spectroThemeCombo, &QComboBox::currentIndexChanged,
                         this, [this](int i) {
                             m_spectro->setTheme(i);
                             saveSettings();
                         });
        QObject::connect(m_spectroRangeCombo, &QComboBox::currentIndexChanged,
                         this, [this](int i) {
                             m_spectro->setRangeDb(spectroRange(i));
                             saveSettings();
                         });
        QObject::connect(m_spectroSensSpin, &QSpinBox::valueChanged, this,
                         [this](int v) {
                             m_spectro->setSensitivityDb(v);
                             saveSettings();
                         });
        QObject::connect(
            m_tabs, &QTabWidget::currentChanged, this,
            [this, rtaLay, spectroLay](int idx) {
                const bool split = m_tabs->widget(idx) == m_split;
                m_spectro->setVertical(split);
                if (split) {
                    m_split->addWidget(m_rta);
                    m_split->addWidget(m_spectro);
                    m_split->setStretchFactor(0, 1);
                    m_split->setStretchFactor(1, 1);
                } else if (m_rta->parentWidget() == m_split) {
                    rtaLay->addWidget(m_rta, 1);
                    spectroLay->addWidget(m_spectro, 1);
                }
                saveSettings();
            });
        QObject::connect(resetBtn, &QPushButton::clicked, this, [this] {
            m_analyzer.resetLeq();
            m_analyzer.resetPeaks();
            m_metricsEng.resetSession();
            m_loudEng.resetSession();
            m_loudMeter->resetSession();
            m_breakout->resetMaxima();
        });
        QObject::connect(m_modeCombo, &QComboBox::currentIndexChanged, this,
                         [this](int) {
                             changeMode(m_modeCombo->currentData().toInt());
                         });
        QObject::connect(m_pairLCombo, &QComboBox::currentIndexChanged, this,
                         [this](int) { applyStereoPair(); });
        QObject::connect(m_pairRCombo, &QComboBox::currentIndexChanged, this,
                         [this](int) { applyStereoPair(); });
        QObject::connect(m_targetCombo, &QComboBox::currentIndexChanged, this,
                         [this](int i) {
                             if (i >= 0 && i != kCustomTargetIdx) {
                                 // blockSignals: filling in a preset must not
                                 // bounce the combo back to Custom.
                                 m_targetSpin->blockSignals(true);
                                 m_ceilSpin->blockSignals(true);
                                 m_targetSpin->setValue(
                                     kLoudnessTargets[i].lufs);
                                 m_ceilSpin->setValue(
                                     kLoudnessTargets[i].ceilDbtp);
                                 m_targetSpin->blockSignals(false);
                                 m_ceilSpin->blockSignals(false);
                             }
                             applyTarget();
                         });
        QObject::connect(m_targetSpin, &QDoubleSpinBox::valueChanged, this,
                         [this](double) {
                             m_targetCombo->setCurrentIndex(kCustomTargetIdx);
                             applyTarget();
                         });
        QObject::connect(m_ceilSpin, &QDoubleSpinBox::valueChanged, this,
                         [this](double) {
                             m_targetCombo->setCurrentIndex(kCustomTargetIdx);
                             applyTarget();
                         });

        // Applied here rather than in loadSettings so the currentChanged
        // handler above runs and assembles the split layout if needed.
        m_tabs->setCurrentIndex(
            std::clamp(m_savedTabIdx, 0, m_tabs->count() - 1));

        // --api [port] command-line override (handy for headless testing)
        const QStringList args = QApplication::arguments();
        const int apiIdx = args.indexOf("--api");
        if (apiIdx >= 0) {
            if (apiIdx + 1 < args.size()) {
                bool ok = false;
                const int p = args[apiIdx + 1].toInt(&ok);
                if (ok)
                    m_apiPort = p;
            }
            m_apiEnabled = true;
        }

        m_analyzer.weighting =
            m_weightCombo->currentText().isEmpty()
                ? 'A'
                : m_weightCombo->currentText()[0].toLatin1();

        auto *timer = new QTimer(this);
        timer->setTimerType(Qt::PreciseTimer);
        timer->setInterval(UPDATE_MS);
        QObject::connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start();

        // API first: opening the audio device can block on the macOS mic
        // permission prompt, and the API should come up regardless.
        applyApiState();
        changeDevice(m_deviceCombo->currentIndex());
    }

    ~MainWindow() override { m_engine.stop(); }

protected:
    void closeEvent(QCloseEvent *event) override {
        saveSettings();
        stopLog(false);
        m_engine.stop();
        QMainWindow::closeEvent(event);
    }

private:
    void buildMenus() {
        QMenu *fileMenu = menuBar()->addMenu("&File");
        m_logAct = fileMenu->addAction("Start SPL &Log…");
        connect(m_logAct, &QAction::triggered, this, [this] { toggleLog(); });
        fileMenu->addSeparator();
        QAction *quitAct = fileMenu->addAction("&Quit");
        quitAct->setShortcut(QKeySequence::Quit);
        connect(quitAct, &QAction::triggered, this, &QWidget::close);

        QMenu *settingsMenu = menuBar()->addMenu("&Settings");
        QAction *inputAct = settingsMenu->addAction("&Input && Mode…");
        connect(inputAct, &QAction::triggered, this, [this] {
            m_inputDlg->show();
            m_inputDlg->raise();
            m_inputDlg->activateWindow();
        });
        settingsMenu->addSeparator();
        QAction *calAct = settingsMenu->addAction("&Calibrate SPL…");
        connect(calAct, &QAction::triggered, this, [this] { showCalibration(); });
        QAction *micAct = settingsMenu->addAction("Load &Mic Correction…");
        connect(micAct, &QAction::triggered, this, [this] {
            const QString path = QFileDialog::getOpenFileName(
                this, "Load Mic Correction File", QString(),
                "Calibration files (*.txt *.cal *.frd *.mic);;All files (*)");
            if (!path.isEmpty())
                loadMicCorrection(path, true);
        });
        m_clearMicAct = settingsMenu->addAction("Clear Mic Correction");
        m_clearMicAct->setEnabled(false);
        connect(m_clearMicAct, &QAction::triggered, this, [this] {
            m_analyzer.setMicCorrection({});
            m_analyzer.resetAll();
            m_micCorrPath.clear();
            m_micCorrName.clear();
            m_clearMicAct->setText("Clear Mic Correction");
            m_clearMicAct->setEnabled(false);
            statusBar()->showMessage("Mic correction cleared", 5000);
            saveSettings();
        });
        settingsMenu->addSeparator();
        QAction *displayAct = settingsMenu->addAction("&Display…");
        connect(displayAct, &QAction::triggered, this, [this] {
            m_displayDlg->show();
            m_displayDlg->raise();
            m_displayDlg->activateWindow();
        });
        QAction *metricsAct = settingsMenu->addAction("&Metrics…");
        connect(metricsAct, &QAction::triggered, this,
                [this] { showMetricsSettings(); });
        QAction *alarmsAct = settingsMenu->addAction("A&larms…");
        connect(alarmsAct, &QAction::triggered, this,
                [this] { showAlarmSettings(); });
        QAction *apiAct = settingsMenu->addAction("&API && Streaming…");
        connect(apiAct, &QAction::triggered, this, [this] { showApiSettings(); });

        QMenu *viewMenu = menuBar()->addMenu("&View");
        m_breakoutAct = viewMenu->addAction("&Metric Breakout");
        m_breakoutAct->setCheckable(true);
        // Not Ctrl+M: on macOS that maps to Cmd+M, the system Minimize.
        m_breakoutAct->setShortcut(QKeySequence("Ctrl+B"));
        connect(m_breakoutAct, &QAction::toggled, this, [this](bool on) {
            if (on) {
                m_breakout->show();
                m_breakout->raise();
                m_breakout->activateWindow();
            } else {
                m_breakout->hide();
            }
            saveSettings();
        });

        QMenu *helpMenu = menuBar()->addMenu("&Help");
        QAction *aboutAct =
            helpMenu->addAction(QString("&About %1").arg(APP_NAME));
        connect(aboutAct, &QAction::triggered, this, [this] { showAbout(); });
        QAction *aboutQtAct = helpMenu->addAction("About &Qt");
        connect(aboutQtAct, &QAction::triggered, qApp, &QApplication::aboutQt);
    }

    void showCalibration() {
        CalibrationDialog dlg(
            this, [this] { return m_lastSlowDbfs; }, m_calSpin->value());
        if (dlg.exec() == QDialog::Accepted && std::isfinite(dlg.newCal())) {
            m_calSpin->setValue(dlg.newCal());  // valueChanged saves settings
            statusBar()->showMessage(
                QString("Calibrated: cal offset set to %1 dB")
                    .arg(dlg.newCal(), 0, 'f', 1),
                5000);
        }
    }

    void loadMicCorrection(const QString &path, bool interactive) {
        std::vector<std::pair<double, double>> pts;
        QString err;
        if (!parseMicCorrection(path, pts, err)) {
            const QString msg =
                QString("Could not load mic correction \"%1\": %2")
                    .arg(QFileInfo(path).fileName(), err);
            if (interactive)
                QMessageBox::warning(this, "Mic Correction", msg);
            else
                statusBar()->showMessage(msg, 8000);
            return;
        }
        m_analyzer.setMicCorrection(pts);
        m_analyzer.resetAll();
        m_micCorrPath = path;
        m_micCorrName = QFileInfo(path).fileName();
        m_clearMicAct->setText(
            QString("Clear Mic Correction (%1)").arg(m_micCorrName));
        m_clearMicAct->setEnabled(true);
        statusBar()->showMessage(QString("Mic correction: %1 (%2 points)")
                                     .arg(m_micCorrName)
                                     .arg(pts.size()),
                                 5000);
        if (interactive)  // during loadSettings() a save would clobber
            saveSettings();  // settings that are not restored yet
    }

    void showMetricsSettings() {
        MetricsDialog dlg(this, m_mode, m_mainMetrics, m_breakoutMetrics,
                          m_leqShortS, m_leqLongS, m_targets,
                          m_breakoutSizeIdx);
        if (dlg.exec() != QDialog::Accepted)
            return;
        m_mainMetrics = dlg.mainIds();
        m_breakoutMetrics = dlg.breakoutIds();
        m_leqShortS = dlg.shortSecs();
        m_leqLongS = dlg.longSecs();
        m_targets = dlg.targets();
        m_breakoutSizeIdx = dlg.sizeIdx();
        applyMetricsConfig();
        saveSettings();
    }

    void applyMetricsConfig() {
        m_metricsEng.shortWindowS = m_leqShortS;
        m_metricsEng.longWindowS = m_leqLongS;
        rebuildReadouts();
        static constexpr int kValuePt[] = {22, 32, 44};
        m_breakout->setValueSize(kValuePt[std::clamp(m_breakoutSizeIdx, 0, 2)]);
        m_breakout->setTiles(m_breakoutMetrics);
    }

    QPair<double, double> targetFor(const QString &id) const {
        return m_targets.value(id, {kNaN, kNaN});
    }

    void showAlarmSettings() {
        AlarmsDialog dlg(this, m_mode, m_alarmEnabled, m_alarmMetric,
                         m_alarmWarn, m_alarmAlert);
        if (dlg.exec() != QDialog::Accepted)
            return;
        m_alarmEnabled = dlg.enableCheck->isChecked();
        m_alarmMetric = dlg.metricCombo->currentData().toString();
        m_alarmWarn = dlg.warnSpin->value();
        m_alarmAlert = dlg.alertSpin->value();
        saveSettings();
    }

    // 0 = normal, 1 = warning, 2 = alert, for the watched metric only.
    int alarmStateFor(const QString &id, const MetricValues &mv) const {
        if (!m_alarmEnabled || id != m_alarmMetric)
            return 0;
        const double v = metricValue(id, mv);
        if (!std::isfinite(v))
            return 0;
        return v >= m_alarmAlert ? 2 : v >= m_alarmWarn ? 1 : 0;
    }

    // --- 1 Hz CSV logging of all metrics ---

    void toggleLog() {
        if (m_logFile.isOpen()) {
            stopLog(true);
            return;
        }
        const QString def =
            QDir(QStandardPaths::writableLocation(
                     QStandardPaths::DocumentsLocation))
                .filePath(QString("spl-log-%1.csv")
                              .arg(QDateTime::currentDateTime().toString(
                                  "yyyyMMdd-HHmmss")));
        const QString path = QFileDialog::getSaveFileName(
            this, "Start SPL Log", def, "CSV files (*.csv);;All files (*)");
        if (path.isEmpty())
            return;
        m_logFile.setFileName(path);
        if (!m_logFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QMessageBox::warning(
                this, "SPL Log",
                QString("Could not open \"%1\" for writing.").arg(path));
            return;
        }
        // Freeze the column set now: switching mode mid-log must not shift
        // the columns out from under rows already written.
        m_logIds.clear();
        for (const MetricInfo &mi : kMetricInfos)
            if (metricInMode(mi, m_mode))
                m_logIds << mi.id;
        QStringList head{"time"};
        head << m_logIds;
        head << "alarm";
        m_logFile.write(head.join(',').toUtf8() + "\n");
        m_lastLogMs = 0;
        m_logAct->setText(QString("Stop SPL &Log (%1)")
                              .arg(QFileInfo(path).fileName()));
        statusBar()->showMessage(
            QString("Logging SPL metrics to %1 (1 s interval)").arg(path),
            6000);
    }

    void stopLog(bool announce) {
        if (!m_logFile.isOpen())
            return;
        const QString name = QFileInfo(m_logFile.fileName()).fileName();
        m_logFile.close();
        m_logAct->setText("Start SPL &Log…");
        if (announce)
            statusBar()->showMessage(QString("SPL log %1 closed").arg(name),
                                     6000);
    }

    void logTick(qint64 now, const MetricValues &mv, int alarmState) {
        if (!m_logFile.isOpen() || now - m_lastLogMs < 1000)
            return;
        m_lastLogMs = now;
        QStringList row{QDateTime::fromMSecsSinceEpoch(now).toString(
            Qt::ISODateWithMs)};
        for (const QString &id : m_logIds) {
            const double v = metricValue(id, mv);
            row << (std::isfinite(v) ? QString::number(v, 'f', 2) : QString());
        }
        row << QString::number(alarmState);
        m_logFile.write(row.join(',').toUtf8() + "\n");
        if (now - m_lastLogFlush >= 10000) {  // survive crashes/power loss
            m_lastLogFlush = now;
            m_logFile.flush();
        }
    }

    void rebuildReadouts() {
        for (auto &pr : m_readouts) {
            m_splRow->removeWidget(pr.second);
            delete pr.second;
        }
        m_readouts.clear();
        int idx = 0;
        for (const QString &id : m_mainMetrics) {
            auto *r = new SplReadout(metricCaption(id));
            const auto t = targetFor(id);
            r->setTarget(t.first, t.second);
            m_splRow->insertWidget(idx++, r);
            m_readouts.push_back({id, r});
        }
    }

    QString metricCaption(const QString &id) const {
        const QString w = m_weightCombo->currentText();
        if (id == "laf") return QString("L%1F (Fast)").arg(w);
        if (id == "las") return QString("L%1S (Slow)").arg(w);
        if (id == "leq") return QString("L%1eq").arg(w);
        if (id == "leqS") return QString("LAeq %1").arg(windowLabel(m_leqShortS));
        if (id == "leqL") return QString("LAeq %1").arg(windowLabel(m_leqLongS));
        if (id == "lzpk") return QString("LZpk");
        if (id == "lcpk") return QString("LCpk");
        if (id == "ca") return QString("C-A %1").arg(windowLabel(m_leqShortS));
        if (id == "l10") return QString("L10");
        if (id == "l50") return QString("L50");
        if (id == "l90") return QString("L90");
        if (id == "doseN") return QString("DOSE NIOSH");
        if (id == "doseO") return QString("DOSE OSHA");
        // Units ride in the caption: the value line is 28 pt, and " LUFS"
        // after the number pushes the digits out of a glanceable size.
        if (id == "lufsM") return QString("M (LUFS)");
        if (id == "lufsS") return QString("S (LUFS)");
        if (id == "lufsI") return QString("INTEGRATED (LUFS)");
        if (id == "toTarget") return QString("Δ TARGET (LU)");
        if (id == "dbtp") return QString("TRUE PEAK (dBTP)");
        if (id == "dbtpMax") return QString("TP MAX (dBTP)");
        if (id == "plr") return QString("PLR (LU)");
        return id;
    }

    static double metricValue(const QString &id, const MetricValues &v) {
        if (id == "laf") return v.laf;
        if (id == "las") return v.las;
        if (id == "leq") return v.leq;
        if (id == "leqS") return v.leqShort;
        if (id == "leqL") return v.leqLong;
        if (id == "lzpk") return v.lzpk;
        if (id == "lcpk") return v.lcpk;
        if (id == "ca") return v.ca;
        if (id == "l10") return v.l10;
        if (id == "l50") return v.l50;
        if (id == "l90") return v.l90;
        if (id == "doseN") return v.doseNiosh;
        if (id == "doseO") return v.doseOsha;
        if (id == "lufsM") return v.loud.momentary;
        if (id == "lufsS") return v.loud.shortTerm;
        if (id == "lufsI") return v.loud.integrated;
        if (id == "toTarget") return v.toTarget;
        if (id == "dbtp") return v.loud.truePeak;
        if (id == "dbtpMax") return v.loud.truePeakMax;
        if (id == "plr") return v.loud.plr;
        return kNaN;
    }

    static QString metricSuffix(const QString &id) {
        return (id == "doseN" || id == "doseO") ? QString("%") : QString();
    }

    std::vector<MetricDisplay> buildDisplays(const QStringList &ids,
                                             const MetricValues &mv) const {
        std::vector<MetricDisplay> out;
        for (const QString &id : ids) {
            if (id == "spark")
                continue;
            const auto t = targetFor(id);
            out.push_back({id, metricCaption(id), metricValue(id, mv),
                           metricSuffix(id), alarmStateFor(id, mv), t.first,
                           t.second});
        }
        return out;
    }

    void showApiSettings() {
        ApiSettingsDialog dlg(this, m_apiEnabled, m_apiPort, m_streamRateIdx,
                              m_apiBind);
        if (dlg.exec() != QDialog::Accepted)
            return;
        m_apiEnabled = dlg.enableCheck->isChecked();
        m_apiPort = dlg.portSpin->value();
        m_streamRateIdx = dlg.rateCombo->currentIndex();
        m_apiBind = dlg.bindAddress();
        applyApiState();
        saveSettings();
    }

    void showAbout() {
        QMessageBox::about(
            this, QString("About %1").arg(APP_NAME),
            QString(
                "<h3>%1</h3>"
                "<p>Version %2</p>"
                "<p>A free SPL meter and 1/3-octave real-time analyzer:<br>"
                "Fast / Slow / Leq with A/C/Z weighting, spectrogram, SPL "
                "history, and an HTTP + WebSocket API for remote monitoring "
                "by the ProdMesh production toolkit.</p>"
                "<p>Levels are relative until calibrated — set the Cal offset "
                "against a known reference for absolute dB SPL. Not a Class 1 "
                "measurement instrument.</p>"
                "<p>Built with Qt %3.</p>")
                .arg(APP_NAME, APP_VERSION, qVersion()));
    }

    void loadSettings() {
        QSettings st("ProdMesh", "RemoteRTA");
        // One-time migration from the pre-rename settings location.
        if (!st.contains("cal")) {
            QSettings old("RTA", "RTA");
            for (const QString &k : old.allKeys())
                st.setValue(k, old.value(k));
        }
        m_calSpin->setValue(st.value("cal", 100.0).toDouble());
        const QString w = st.value("weighting", "A").toString();
        if (m_weightCombo->findText(w) >= 0)
            m_weightCombo->setCurrentText(w);
        m_avgCombo->setCurrentIndex(st.value("rtaAvg", 1).toInt());
        m_peakCheck->setChecked(st.value("peakHold", false).toBool());
        m_apiEnabled = st.value("apiEnabled", false).toBool();
        m_apiPort = st.value("apiPort", 8517).toInt();
        m_apiBind = st.value("apiBind").toString();  // empty = all interfaces
        m_streamRateIdx = st.value("streamRate", 2).toInt();  // default 10 Hz
        m_savedDevice = st.value("device").toString();
        m_inputChannel = st.value("inputChannel", 0).toInt();
        m_spectroThemeCombo->setCurrentIndex(
            std::clamp(st.value("spectroTheme", 0).toInt(), 0,
                       m_spectroThemeCombo->count() - 1));
        m_spectroRangeCombo->setCurrentIndex(
            std::clamp(st.value("spectroRange", 2).toInt(), 0,
                       m_spectroRangeCombo->count() - 1));
        m_spectroSensSpin->setValue(st.value("spectroSens", 0).toInt());
        m_spectro->setTheme(m_spectroThemeCombo->currentIndex());
        m_spectro->setRangeDb(spectroRange(m_spectroRangeCombo->currentIndex()));
        m_spectro->setSensitivityDb(m_spectroSensSpin->value());
        m_rtaViewCombo->setCurrentIndex(
            std::clamp(st.value("rtaView", 0).toInt(), 0, 1));
        m_rtaDecayCombo->setCurrentIndex(
            std::clamp(st.value("rtaDecay", 0).toInt(), 0, 3));
        m_rtaGridCheck->setChecked(st.value("rtaFreqGrid", true).toBool());
        m_spectroSpanCombo->setCurrentIndex(
            std::clamp(st.value("spectroSpan", 1).toInt(), 0, 5));
        for (RtaWidget *r : m_rtas) {
            r->setViewMode(m_rtaViewCombo->currentIndex());
            r->setDecayRate(rtaDecayRate(m_rtaDecayCombo->currentIndex()));
            r->setFreqGridlines(m_rtaGridCheck->isChecked());
        }
        m_spectro->setSpanSeconds(
            spectroSpanSeconds(m_spectroSpanCombo->currentIndex()));
        m_breakout->setAlwaysOnTop(st.value("breakoutOnTop", false).toBool());
        const QByteArray bgeo = st.value("breakoutGeo").toByteArray();
        if (!bgeo.isEmpty())
            m_breakout->restoreGeometry(bgeo);
        m_leqShortS = st.value("leqShortS", 60).toInt();
        m_leqLongS = st.value("leqLongS", 900).toInt();
        m_breakoutSizeIdx = std::clamp(st.value("breakoutSize", 1).toInt(), 0, 2);

        // Program-mode delivery target. Global rather than mode-scoped: it
        // only means anything in Program mode anyway.
        m_pairL = st.value("pairL", 0).toInt();
        m_pairR = st.value("pairR", 1).toInt();
        m_targetIdx = std::clamp(st.value("targetIdx", 0).toInt(), 0,
                                 kCustomTargetIdx);
        m_targetLufs = st.value("targetLufs",
                                kLoudnessTargets[m_targetIdx].lufs).toDouble();
        m_ceilDbtp = st.value("ceilDbtp",
                              kLoudnessTargets[m_targetIdx].ceilDbtp).toDouble();
        m_targetCombo->blockSignals(true);
        m_targetCombo->setCurrentIndex(m_targetIdx);
        m_targetCombo->blockSignals(false);
        m_targetSpin->blockSignals(true);
        m_targetSpin->setValue(m_targetLufs);
        m_targetSpin->blockSignals(false);
        m_ceilSpin->blockSignals(true);
        m_ceilSpin->setValue(m_ceilDbtp);
        m_ceilSpin->blockSignals(false);

        m_mode = st.value("mode", ModeAcoustic).toInt() == ModeProgram
                     ? ModeProgram
                     : ModeAcoustic;
        m_modeCombo->blockSignals(true);
        m_modeCombo->setCurrentIndex(m_modeCombo->findData(m_mode));
        m_modeCombo->blockSignals(false);
        loadModeSettings();
        applyMode();
        if (st.value("breakoutOpen", false).toBool())
            m_breakoutAct->setChecked(true);  // toggled handler shows it
        const QString micPath = st.value("micCorrFile").toString();
        if (!micPath.isEmpty())
            loadMicCorrection(micPath, false);
        const QByteArray geo = st.value("geometry").toByteArray();
        if (!geo.isEmpty())
            restoreGeometry(geo);
    }

    // Settings keys that differ between modes. Acoustic keeps the original
    // un-prefixed names so existing installs carry over untouched.
    QString mkey(const char *k) const {
        return m_mode == ModeProgram ? QString("program/") + k : QString(k);
    }

    void loadModeSettings() {
        QSettings st("ProdMesh", "RemoteRTA");
        const bool program = m_mode == ModeProgram;
        m_mainMetrics =
            st.value(mkey("metricsMain"),
                     program ? QStringList{"lufsM", "lufsS", "lufsI", "dbtpMax"}
                             : QStringList{"laf", "las", "leq"})
                .toStringList();
        m_breakoutMetrics =
            st.value(mkey("metricsBreakout"),
                     program
                         ? QStringList{"lufsS", "lufsI", "dbtpMax", "spark"}
                         : QStringList{"laf", "las", "leq", "spark"})
                .toStringList();
        m_alarmEnabled = st.value(mkey("alarmEnabled"), false).toBool();
        m_alarmMetric =
            st.value(mkey("alarmMetric"), program ? "lufsS" : "las").toString();
        m_alarmWarn =
            st.value(mkey("alarmWarn"), program ? -12.0 : 96.0).toDouble();
        m_alarmAlert =
            st.value(mkey("alarmAlert"), program ? -9.0 : 102.0).toDouble();
        m_inputChannel = st.value(mkey("inputChannel"), 0).toInt();
        m_savedTabIdx = st.value(mkey("viewTab"), 0).toInt();
        // The RTA's vertical scale is mode-scoped because its top is
        // cal - sensitivity. In Acoustic, cal is ~100-145 dB SPL and per-band
        // levels sit well below broadband, so a large sensitivity shift is
        // normal. In Program, cal is 0 and the natural top is 0 dBFS — the
        // same shift would drop the ceiling below the signal and pin every
        // band against the top.
        m_rtaSensSpin->blockSignals(true);
        m_rtaSensSpin->setValue(st.value(mkey("rtaSens"), 0).toInt());
        m_rtaSensSpin->blockSignals(false);
        m_rtaRangeCombo->blockSignals(true);
        m_rtaRangeCombo->setCurrentIndex(
            std::clamp(st.value(mkey("rtaRange"), 2).toInt(), 0, 3));
        m_rtaRangeCombo->blockSignals(false);
        // Target bands as "id:lo:hi". Acoustic defaults to C-A ratio 8–12 dB,
        // the range that keeps low-end energy below the "it's too loud" zone.
        m_targets.clear();
        const QStringList tgts =
            st.value(mkey("metricTargets"),
                     program ? QStringList{} : QStringList{"ca:8:12"})
                .toStringList();
        for (const QString &t : tgts) {
            const QStringList p = t.split(':');
            if (p.size() != 3)
                continue;
            bool okL = false, okH = false;
            const double lo = p[1].toDouble(&okL), hi = p[2].toDouble(&okH);
            if (okL && okH && hi > lo)
                m_targets.insert(p[0], {lo, hi});
        }
    }

    void saveModeSettings(QSettings &st) {
        st.setValue(mkey("metricsMain"), m_mainMetrics);
        st.setValue(mkey("metricsBreakout"), m_breakoutMetrics);
        st.setValue(mkey("alarmEnabled"), m_alarmEnabled);
        st.setValue(mkey("alarmMetric"), m_alarmMetric);
        st.setValue(mkey("alarmWarn"), m_alarmWarn);
        st.setValue(mkey("alarmAlert"), m_alarmAlert);
        st.setValue(mkey("inputChannel"), m_inputChannel);
        st.setValue(mkey("viewTab"), m_tabs->currentIndex());
        st.setValue(mkey("rtaSens"), m_rtaSensSpin->value());
        st.setValue(mkey("rtaRange"), m_rtaRangeCombo->currentIndex());
        QStringList tgts;
        for (auto it = m_targets.constBegin(); it != m_targets.constEnd(); ++it)
            tgts << QString("%1:%2:%3")
                        .arg(it.key())
                        .arg(it->first)
                        .arg(it->second);
        st.setValue(mkey("metricTargets"), tgts);
    }

    void saveSettings() {
        if (m_applyingMode)  // tab churn mid-switch is not a user choice
            return;
        QSettings st("ProdMesh", "RemoteRTA");
        st.setValue("cal", m_calSpin->value());
        st.setValue("weighting", m_weightCombo->currentText());
        st.setValue("rtaAvg", m_avgCombo->currentIndex());
        st.setValue("peakHold", m_peakCheck->isChecked());
        st.setValue("apiEnabled", m_apiEnabled);
        st.setValue("apiPort", m_apiPort);
        st.setValue("apiBind", m_apiBind);
        st.setValue("streamRate", m_streamRateIdx);
        st.setValue("micCorrFile", m_micCorrPath);
        st.setValue("spectroTheme", m_spectroThemeCombo->currentIndex());
        st.setValue("spectroRange", m_spectroRangeCombo->currentIndex());
        st.setValue("spectroSens", m_spectroSensSpin->value());
        st.setValue("rtaView", m_rtaViewCombo->currentIndex());
        st.setValue("rtaDecay", m_rtaDecayCombo->currentIndex());
        st.setValue("rtaFreqGrid", m_rtaGridCheck->isChecked());
        st.setValue("spectroSpan", m_spectroSpanCombo->currentIndex());
        st.setValue("breakoutOpen", m_breakout->isVisible());
        st.setValue("breakoutOnTop", m_breakout->alwaysOnTop());
        st.setValue("breakoutGeo", m_breakout->saveGeometry());
        st.setValue("leqShortS", m_leqShortS);
        st.setValue("leqLongS", m_leqLongS);
        st.setValue("breakoutSize", m_breakoutSizeIdx);
        st.setValue("mode", m_mode);
        st.setValue("pairL", m_pairL);
        st.setValue("pairR", m_pairR);
        st.setValue("targetIdx", m_targetIdx);
        st.setValue("targetLufs", m_targetLufs);
        st.setValue("ceilDbtp", m_ceilDbtp);
        if (m_deviceCombo->currentIndex() >= 0)
            st.setValue("device", m_deviceCombo->currentText());
        st.setValue("geometry", saveGeometry());
        saveModeSettings(st);
    }

    void changeMode(int mode) {
        if (mode == m_mode)
            return;
        {
            QSettings st("ProdMesh", "RemoteRTA");
            saveModeSettings(st);  // still keyed to the mode we are leaving
        }
        m_mode = mode;
        loadModeSettings();
        applyMode();
        populateChannels();
        saveSettings();
        statusBar()->showMessage(
            mode == ModeProgram
                ? "Program mode — metering the bus against full scale (LUFS)"
                : "Acoustic mode — metering the room in dB SPL",
            6000);
    }

    void applyMode() {
        const bool program = m_mode == ModeProgram;
        m_applyingMode = true;
        setWindowTitle(QString("%1 — %2").arg(
            APP_NAME, program ? "Program (Loudness)" : "Acoustic (SPL)"));
        // Nothing to calibrate against on a digital bus.
        m_calLbl->setVisible(!program);
        m_calSpin->setVisible(!program);
        m_programGroup->setEnabled(program);

        // Loudness leads the tab bar in Program mode; it is the view people
        // open the app for there.
        const int at = m_tabs->indexOf(m_loudPage);
        if (program && at < 0) {
            m_tabs->insertTab(0, m_loudPage, "Loudness");
        } else if (!program && at >= 0) {
            // removeTab does not delete the page and leaves it parentless;
            // re-own it here so it is not leaked and cannot show up as a
            // stray top-level window.
            m_tabs->removeTab(at);
            m_loudPage->setParent(this);
            m_loudPage->hide();
        }

        m_engine.setLoudness(program, m_pairL, m_pairR);
        m_loudEng.resetAll();
        m_loudVals = LoudnessValues();
        m_loudMeter->setTarget(m_targetLufs, m_ceilDbtp);
        m_loudMeter->resetSession();
        m_breakout->setSparkCaption(program ? "LOUDNESS — 10 MIN"
                                            : "SPL — 10 MIN");
        // Wipe every accumulated view and statistic: the two modes measure
        // against different references, so anything carried across would be
        // plotted on a scale it was never measured in.
        m_analyzer.resetAll();
        m_metricsEng.resetAll();
        m_history->clear();
        m_spectro->clear();
        m_breakout->clearSpark();
        m_breakout->resetMaxima();
        m_api->clearHistory();
        for (RtaWidget *r : m_rtas)
            r->clearData();
        applyMetricsConfig();
        m_tabs->setCurrentIndex(
            std::clamp(m_savedTabIdx, 0, m_tabs->count() - 1));
        m_applyingMode = false;
    }

    void applyStereoPair() {
        m_pairL = m_pairLCombo->currentData().toInt();
        m_pairR = m_pairRCombo->currentData().toInt();
        m_engine.setLoudness(m_mode == ModeProgram, m_pairL, m_pairR);
        m_loudEng.resetAll();
        saveSettings();
    }

    void applyTarget() {
        m_targetIdx = m_targetCombo->currentIndex();
        m_targetLufs = m_targetSpin->value();
        m_ceilDbtp = m_ceilSpin->value();
        m_loudMeter->setTarget(m_targetLufs, m_ceilDbtp);
        saveSettings();
    }

    // Rebuild the channel picker for the device that just opened: 1..N plus
    // Mix, keeping the saved selection when it still exists.
    void populateChannels() {
        m_chanCombo->blockSignals(true);
        m_chanCombo->clear();
        const int n = std::max(1, m_engine.channelCount());
        for (int i = 1; i <= n; ++i)
            m_chanCombo->addItem(QString::number(i), i - 1);
        m_chanCombo->addItem("Mix", -1);
        int idx = m_chanCombo->findData(m_inputChannel);
        if (idx < 0) {  // saved channel doesn't exist on this device
            idx = 0;
            m_inputChannel = 0;
        }
        m_chanCombo->setCurrentIndex(idx);
        m_chanCombo->setEnabled(n > 1);
        m_chanCombo->blockSignals(false);
        m_engine.setChannel(m_inputChannel);

        // Stereo pair pickers track the same channel list. A mono device
        // collapses both to channel 1, which BS.1770 handles fine.
        for (QComboBox *c : {m_pairLCombo, m_pairRCombo}) {
            c->blockSignals(true);
            c->clear();
            for (int i = 1; i <= n; ++i)
                c->addItem(QString::number(i), i - 1);
        }
        auto pick = [](QComboBox *c, int &want, int fallback) {
            int i = c->findData(want);
            if (i < 0) {
                i = std::clamp(fallback, 0, c->count() - 1);
                want = c->itemData(i).toInt();
            }
            c->setCurrentIndex(i);
        };
        pick(m_pairLCombo, m_pairL, 0);
        pick(m_pairRCombo, m_pairR, 1);
        m_pairLCombo->blockSignals(false);
        m_pairRCombo->blockSignals(false);
        m_engine.setLoudness(m_mode == ModeProgram, m_pairL, m_pairR);
    }

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
        // Prefer the device used last session, if it is still present.
        if (!m_savedDevice.isEmpty()) {
            const int row = m_deviceCombo->findText(m_savedDevice);
            if (row >= 0)
                selectRow = row;
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
            populateChannels();
            m_analyzer.sr = m_engine.sampleRate();
            m_analyzer.resetAll();
            m_metricsEng.resetAll();
            statusBar()->showMessage(
                QString("Listening at %1 Hz, %2 ch — FFT %3 (%4 ms window)")
                    .arg(m_engine.sampleRate())
                    .arg(m_engine.channelCount())
                    .arg(FFT_SIZE)
                    .arg(double(FFT_SIZE) / m_engine.sampleRate() * 1000.0, 0,
                         'f', 0));
        } catch (const std::exception &e) {
            statusBar()->showMessage(
                QString("Could not open input: %1").arg(e.what()));
        }
    }

    void applyApiState() {
        if (m_apiEnabled) {
            const quint16 port = quint16(m_apiPort);
            const QHostAddress bind =
                m_apiBind.isEmpty() ? QHostAddress(QHostAddress::Any)
                                    : QHostAddress(m_apiBind);
            if (m_api->listen(port, bind)) {
                const QString url =
                    m_apiBind.isEmpty()
                        ? ApiServer::localUrl(port)
                        : QString("http://%1:%2").arg(m_apiBind).arg(port);
                m_apiLbl->setStyleSheet("color:#8a92a6;");
                m_apiLbl->setText(
                    QString("Dashboard %1  ·  API %1/api").arg(url));
            } else {
                m_apiLbl->setStyleSheet("color:#e05c5c;");
                m_apiLbl->setText(
                    QString("API error on %1: %2")
                        .arg(m_apiBind.isEmpty() ? "any interface" : m_apiBind,
                             m_api->errorString()));
                // Also on stderr — the status bar is invisible when run
                // headless via --api.
                std::fprintf(stderr, "API error on port %d (%s): %s\n",
                             m_apiPort,
                             m_apiBind.isEmpty() ? "any"
                                                 : qPrintable(m_apiBind),
                             qPrintable(m_api->errorString()));
            }
        } else {
            m_api->close();
            m_apiLbl->clear();
        }
    }

    static double rtaRange(int idx) {
        static const double ranges[] = {40.0, 60.0, 80.0, 100.0};
        return ranges[std::clamp(idx, 0, 3)];
    }

    // Seconds of history for each Time span choice.
    static double spectroSpanSeconds(int idx) {
        static constexpr double s[] = {10, 30, 60, 120, 300, 600};
        return s[std::clamp(idx, 0, 5)];
    }

    static double spectroRange(int idx) {
        static const double ranges[] = {60.0, 80.0, 100.0, 120.0};
        return ranges[std::clamp(idx, 0, 3)];
    }

    static double rtaDecayRate(int idx) {
        static const double rates[] = {0.0, 6.0, 12.0, 24.0};
        return rates[std::clamp(idx, 0, 3)];
    }

    double rtaTau() const {
        static const double taus[] = {0.0, 0.125, 1.0, 2.0};
        return taus[m_avgCombo->currentIndex()];
    }

    int streamIntervalMs() const {
        static const int ms[] = {1000, 200, 100, 50};
        return ms[std::clamp(m_streamRateIdx, 0, 3)];
    }

    void tick() {
        float pk = 0.0f, pkC = 0.0f;
        const bool have = m_engine.latest(FFT_SIZE, m_samples, pk, pkC);
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
        // Use measured elapsed time so Fast/Slow ballistics stay accurate
        // even when the OS delivers timer ticks late.
        const qint64 nowTick = QDateTime::currentMSecsSinceEpoch();
        const double dt =
            m_lastTickMs > 0
                ? std::clamp((nowTick - m_lastTickMs) / 1000.0, 0.01, 0.5)
                : UPDATE_MS / 1000.0;
        m_lastTickMs = nowTick;
        AnalyzerResult res = m_analyzer.process(m_samples, dt, rtaTau(),
                                                m_peakCheck->isChecked());
        m_lastSlowDbfs = res.slow;
        // Program mode meters a digital bus: full scale is the reference, so
        // there is no offset to add and every level reads dBFS.
        const bool program = m_mode == ModeProgram;
        const double cal = program ? 0.0 : m_calSpin->value();
        const QString w = m_weightCombo->currentText();

        // Loudness runs off the capture side's gapless 100 ms sub-blocks,
        // not off the overlapping FFT windows above.
        if (program) {
            m_engine.takeLoudness(m_loudBlocks);
            for (const LoudnessBlock &b : m_loudBlocks)
                m_loudEng.push(b);
            m_loudVals = m_loudEng.values();
            m_loudMeter->setValues(m_loudVals);
        }

        m_metricsEng.push(res.powA, res.powC, pk, pkC, dt, cal);
        MetricValues mv = m_metricsEng.values(cal);
        mv.laf = res.fast + cal;
        mv.las = res.slow + cal;
        mv.leq = res.leq + cal;
        mv.loud = m_loudVals;
        if (program && std::isfinite(m_loudVals.integrated))
            mv.toTarget = m_loudVals.integrated - m_targetLufs;
        for (auto &pr : m_readouts) {
            pr.second->set(metricCaption(pr.first), metricValue(pr.first, mv),
                           metricSuffix(pr.first));
            pr.second->setAlarmState(alarmStateFor(pr.first, mv));
        }

        for (double &b : res.bands)
            b += cal;
        for (double &p : res.peaks)
            p += cal;
        for (double &v : res.hires)
            v += cal;
        // Scale tops out at cal = SPL at 0 dBFS (the clip point), minus the
        // sensitivity shift: per-band levels run well below broadband SPL,
        // and high-headroom calibrations push them further down still.
        const double rtaTop = cal - m_rtaSensSpin->value();
        // Only the visible one repaints, so feeding both costs a copy.
        for (RtaWidget *r : m_rtas)
            r->setData(res.bands, res.hires, res.peaks,
                       rtaTop - rtaRange(m_rtaRangeCombo->currentIndex()),
                       rtaTop, dt);
        m_spectro->pushColumn(m_analyzer.lastPower(), m_analyzer.binWidth());
        m_breakout->updateMetrics(buildDisplays(m_breakoutMetrics, mv));

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        // The history strip carries whatever the mode's headline level is:
        // Slow SPL in Acoustic, momentary/short-term loudness in Program.
        if (program) {
            m_breakout->pushSpark(now, m_loudVals.shortTerm);
            m_history->setRange(m_targetLufs - 24.0, m_targetLufs + 9.0);
            m_history->push(now, m_loudVals.momentary, m_loudVals.shortTerm);
        } else {
            m_breakout->pushSpark(now, res.slow + cal);
            m_history->setRange(cal - 80.0, cal);
            m_history->push(now, res.fast + cal, res.slow + cal);
        }

        ApiServer::Snapshot snap;
        snap.timeMs = now;
        snap.samplerate = m_engine.sampleRate();
        snap.channels = m_engine.channelCount();
        snap.channel = m_engine.channel();
        snap.weighting = w;
        snap.cal = cal;
        snap.fast = res.fast + cal;
        snap.slow = res.slow + cal;
        snap.leq = res.leq + cal;
        snap.bands = res.bands;
        snap.peaks = res.peaks;
        snap.micCorr = m_micCorrName;
        snap.mode = program ? "program" : "acoustic";
        snap.targetLufs = program ? m_targetLufs : kNaN;
        snap.ceilDbtp = program ? m_ceilDbtp : kNaN;
        snap.metrics.clear();
        for (const MetricInfo &mi : kMetricInfos)
            if (metricInMode(mi, m_mode))
                snap.metrics.push_back({mi.id, metricValue(mi.id, mv)});
        const int alarmState = alarmStateFor(m_alarmMetric, mv);
        snap.alarmEnabled = m_alarmEnabled;
        snap.alarmMetric = m_alarmMetric;
        snap.alarmState = alarmState;
        snap.alarmWarn = m_alarmWarn;
        snap.alarmAlert = m_alarmAlert;
        snap.targets.clear();
        for (auto it = m_targets.constBegin(); it != m_targets.constEnd();
             ++it)
            snap.targets.push_back({it.key(), it->first, it->second});
        m_api->setSnapshot(snap);
        logTick(now, mv, alarmState);
        if (now - m_lastHistPush >= 1000) {
            m_lastHistPush = now;
            m_api->pushHistory(
                {now, res.fast + cal, res.slow + cal, res.leq + cal, mv.ca});
        }
        // -UPDATE_MS/2: the check runs on the tick grid, so without slack a
        // 100 ms interval lands on alternating 100/150 ms ticks (~8 Hz).
        if (now - m_lastStream >= streamIntervalMs() - UPDATE_MS / 2) {
            m_lastStream = now;
            m_api->broadcastSnapshot();
        }
    }

    AudioEngine m_engine;
    Analyzer m_analyzer;
    MetricsEngine m_metricsEng;
    LoudnessEngine m_loudEng;
    LoudnessValues m_loudVals;
    std::vector<LoudnessBlock> m_loudBlocks;
    int m_mode = ModeAcoustic;
    bool m_applyingMode = false;
    int m_pairL = 0, m_pairR = 1;      // capture channels carrying L / R
    int m_targetIdx = 0;               // index into kLoudnessTargets
    double m_targetLufs = -14.0;
    double m_ceilDbtp = -1.0;
    QStringList m_logIds;              // CSV columns, frozen at log start
    QStringList m_mainMetrics;
    QStringList m_breakoutMetrics;
    int m_leqShortS = 60;
    int m_leqLongS = 900;
    QHBoxLayout *m_splRow = nullptr;
    std::vector<std::pair<QString, SplReadout *>> m_readouts;
    std::vector<float> m_samples;
    QList<QAudioDevice> m_devices;
    QString m_savedDevice;
    QString m_micCorrPath;
    QString m_micCorrName;
    QAction *m_clearMicAct = nullptr;
    double m_lastSlowDbfs = kNaN;
    bool m_alarmEnabled = false;
    QString m_alarmMetric = "las";
    double m_alarmWarn = 96.0;
    double m_alarmAlert = 102.0;
    QHash<QString, QPair<double, double>> m_targets;  // id -> {lo, hi} dB
    int m_breakoutSizeIdx = 1;  // 0 small, 1 medium, 2 large
    QAction *m_logAct = nullptr;
    QFile m_logFile;
    qint64 m_lastLogMs = 0;
    qint64 m_lastLogFlush = 0;
    ApiServer *m_api;
    bool m_apiEnabled = false;
    int m_apiPort = 8517;
    QString m_apiBind;  // bind address; empty = all interfaces
    int m_streamRateIdx = 2;  // 10 Hz
    QComboBox *m_deviceCombo;
    QComboBox *m_chanCombo;
    int m_inputChannel = 0;  // 0-based; -1 = mix of all channels
    QComboBox *m_weightCombo;
    QComboBox *m_avgCombo;
    QComboBox *m_rtaViewCombo;
    QComboBox *m_rtaDecayCombo;
    QComboBox *m_rtaRangeCombo;
    QSpinBox *m_rtaSensSpin;
    QComboBox *m_spectroThemeCombo;
    QComboBox *m_spectroRangeCombo;
    QSpinBox *m_spectroSensSpin;
    QComboBox *m_spectroSpanCombo;
    QCheckBox *m_rtaGridCheck;
    QDialog *m_displayDlg;
    QDialog *m_inputDlg;
    QGroupBox *m_programGroup;
    QComboBox *m_modeCombo;
    QComboBox *m_pairLCombo;
    QComboBox *m_pairRCombo;
    QComboBox *m_targetCombo;
    QDoubleSpinBox *m_targetSpin;
    QDoubleSpinBox *m_ceilSpin;
    QLabel *m_calLbl;
    LoudnessMeter *m_loudMeter;
    QWidget *m_loudPage;
    BreakoutWindow *m_breakout = nullptr;
    QAction *m_breakoutAct = nullptr;
    QCheckBox *m_peakCheck;
    QDoubleSpinBox *m_calSpin;
    QLabel *m_apiLbl;
    QLabel *m_clipLbl;
    RtaWidget *m_rta;
    RtaWidget *m_loudRta;              // the one under the loudness meter
    std::vector<RtaWidget *> m_rtas;   // every RTA that view settings apply to
    SpectrogramWidget *m_spectro;
    HistoryWidget *m_history;
    QTabWidget *m_tabs;
    QSplitter *m_split;
    int m_savedTabIdx = 0;
    int m_clipTicks = 0;
    qint64 m_lastHistPush = 0;
    qint64 m_lastStream = 0;
    qint64 m_lastTickMs = 0;
};

// ---------------------------------------------------------------------------

// Verify DSP math with a synthetic full-scale 1 kHz sine (no audio, no GUI).
// MetricsEngine has closed-form expected values for constant inputs, so it
// gets exact regression checks (tolerances only absorb float rounding).
static bool metricsSelftest() {
    bool ok = true;

    // 120 s of constant 0 dB A-power with C-power 3 dB hotter: both Leq
    // windows read 0 dB and C-A reads the 3.01 dB difference.
    MetricsEngine eng;
    for (int i = 0; i < 240; ++i)
        eng.push(1.0, 2.0, 1.0, 1.0, 0.5, 0.0);
    MetricValues v = eng.values(0.0);
    std::printf("metrics: leqS = %.2f, leqL = %.2f (expected 0.00), "
                "C-A = %.2f (expected 3.01), LZpk = %.2f (expected 0.00)\n",
                v.leqShort, v.leqLong, v.ca, v.lzpk);
    ok = ok && std::fabs(v.leqShort) < 0.01 && std::fabs(v.leqLong) < 0.01 &&
         std::fabs(v.ca - 3.01) < 0.01 && std::fabs(v.lzpk) < 0.01;

    // Percentiles: 80 s at 0 dB + 20 s at 10 dB -> L10 = 10, L50 = L90 = 0.
    MetricsEngine pct;
    for (int i = 0; i < 80; ++i)
        pct.push(1.0, 1.0, 1.0, 1.0, 1.0, 0.0);
    for (int i = 0; i < 20; ++i)
        pct.push(10.0, 10.0, 1.0, 1.0, 1.0, 0.0);
    v = pct.values(0.0);
    std::printf("metrics: L10 = %.2f (expected 10), L50 = %.2f, L90 = %.2f "
                "(expected 0)\n",
                v.l10, v.l50, v.l90);
    ok = ok && std::fabs(v.l10 - 10.0) < 0.01 && std::fabs(v.l50) < 0.01 &&
         std::fabs(v.l90) < 0.01;

    // Dose: 8 h at exactly 85 dBA = 100% NIOSH (criterion level) and 50%
    // OSHA (5 dB exchange rate, 90 dB criterion -> half rate at 85).
    MetricsEngine dose;
    const double p85 = std::pow(10.0, 8.5);
    for (int i = 0; i < 8 * 3600; ++i)
        dose.push(p85, p85, 1.0, 1.0, 1.0, 0.0);
    v = dose.values(0.0);
    std::printf("metrics: 8 h @ 85 dBA -> NIOSH %.1f%% (expected 100), "
                "OSHA %.1f%% (expected 50)\n",
                v.doseNiosh, v.doseOsha);
    ok = ok && std::fabs(v.doseNiosh - 100.0) < 0.5 &&
         std::fabs(v.doseOsha - 50.0) < 0.5;
    return ok;
}

// Program-mode DSP chain: K-weighting, true peak, and the two-stage loudness
// gate. Expected values come from BS.1770-4 itself, not from a previous run.
static bool loudnessSelftest() {
    bool ok = true;

    // The K-weight coefficients are derived per sample rate rather than
    // hardcoded, so check the 48 kHz design against the published table.
    KWeightFilter kw;
    kw.design(48000.0);
    const double ref[10] = {1.53512485958697,  -2.69169618940638,
                            1.19839281085285,  -1.69065929318241,
                            0.73248077421585,  1.0,
                            -2.0,              1.0,
                            -1.99004745483398, 0.99007225036621};
    const double got[10] = {kw.shelf().b0,     kw.shelf().b1,
                            kw.shelf().b2,     kw.shelf().a1,
                            kw.shelf().a2,     kw.highpass().b0,
                            kw.highpass().b1,  kw.highpass().b2,
                            kw.highpass().a1,  kw.highpass().a2};
    double worst = 0.0;
    for (int i = 0; i < 10; ++i)
        worst = std::max(worst, std::fabs(got[i] - ref[i]));
    std::printf("K-weight @48k: worst coefficient error = %.1e "
                "(expected < 1e-9)\n",
                worst);
    ok = ok && worst < 1e-9;

    // BS.1770 anchor: a full-scale 1 kHz sine on one channel reads -3.01
    // LUFS — the +0.69 dB K-weight gain at 1 kHz cancels the -0.691 offset.
    kw.reset();
    double sum = 0.0;
    int n = 0;
    for (int i = 0; i < 48000 * 2; ++i) {
        const double y = kw.step(std::sin(2.0 * kPi * 1000.0 * i / 48000.0));
        if (i >= 4800) {  // skip filter settling
            sum += y * y;
            ++n;
        }
    }
    const double lufs = LoudnessEngine::loudness(sum / n);
    std::printf("full-scale 1 kHz sine = %.2f LUFS (expected -3.01)\n", lufs);
    ok = ok && std::fabs(lufs + 3.01) < 0.05;

    // True peak: a sine at fs/4 shifted 45 deg peaks exactly between samples,
    // so sample peak reads -3.01 dBFS while the real peak is full scale.
    TruePeakDetector tpd;
    double sp = 0.0, tpk = 0.0;
    for (int i = 0; i < 4800; ++i) {
        const double x = std::sin(kPi * i / 2.0 + kPi / 4.0);
        sp = std::max(sp, std::fabs(x));
        tpk = std::max(tpk, tpd.step(x));
    }
    std::printf("inter-sample peak: sample = %.2f dBFS (expected -3.01), "
                "true = %.2f dBTP (expected ~0.00)\n",
                20.0 * std::log10(sp), 20.0 * std::log10(tpk));
    ok = ok && std::fabs(20.0 * std::log10(sp) + 3.01) < 0.05 &&
         std::fabs(20.0 * std::log10(tpk)) < 0.3;

    auto run = [](LoudnessEngine &e, double level, int seconds) {
        LoudnessBlock b;
        b.z = LoudnessEngine::powerFor(level);
        for (int i = 0; i < seconds * (1000 / LoudnessEngine::kSubMs); ++i)
            e.push(b);
    };

    // Absolute gate: silence after the programme must not drag it down.
    LoudnessEngine g1;
    run(g1, -14.0, 60);
    for (int i = 0; i < 600; ++i)
        g1.push(LoudnessBlock{});  // digital black
    LoudnessValues lv = g1.values();
    std::printf("gate: -14 LUFS then silence -> integrated %.2f LUFS "
                "(expected -14.00)\n",
                lv.integrated);
    ok = ok && std::fabs(lv.integrated + 14.0) < 0.05;

    // Relative gate: a -40 LUFS passage sits more than 10 LU below the mean,
    // so it drops out instead of pulling the programme level down.
    LoudnessEngine g2;
    run(g2, -14.0, 60);
    run(g2, -40.0, 60);
    lv = g2.values();
    std::printf("gate: -14 then -40 LUFS -> integrated %.2f LUFS "
                "(expected -14.00), M = %.2f, S = %.2f (expected -40.00)\n",
                lv.integrated, lv.momentary, lv.shortTerm);
    ok = ok && std::fabs(lv.integrated + 14.0) < 0.1 &&
         std::fabs(lv.momentary + 40.0) < 0.05 &&
         std::fabs(lv.shortTerm + 40.0) < 0.05;
    return ok;
}

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
    bool ok = std::fabs(res.fast + 3.01) < 0.2 &&
              THIRD_OCT_CENTERS[kMax] == 1000.0 &&
              std::fabs(res.bands[kMax] + 3.01) < 0.3;

    // Hi-res line spectrum: the loudest 1/24-oct point must sit at ~1 kHz
    // near full tone level (small leakage into neighbors is expected).
    int hMax = 0;
    for (int i = 1; i < HIRES_POINTS; ++i)
        if (std::isfinite(res.hires[i]) &&
            (!std::isfinite(res.hires[hMax]) || res.hires[i] > res.hires[hMax]))
            hMax = i;
    std::printf("hi-res peak = %.1f Hz at %.2f dBFS (expected ~1000 Hz ~ -3)\n",
                hiresFreq(hMax), res.hires[hMax]);
    ok = ok && std::fabs(hiresFreq(hMax) - 1000.0) < 25.0 &&
         std::fabs(res.hires[hMax] + 3.01) < 1.0;

    // Parallel weighted powers: A-weight at 1 kHz is 0 dB, so powA ~ -3.01.
    std::printf("powA = %7.2f, powC = %7.2f, powZ = %7.2f dBFS "
                "(expected ~ -3.01 each)\n",
                toDb(res.powA), toDb(res.powC), toDb(res.powZ));
    ok = ok && std::fabs(toDb(res.powA) + 3.01) < 0.3 &&
         std::fabs(toDb(res.powZ) + 3.01) < 0.3;

    // Time-domain C-weighting filter: unity gain at 1 kHz.
    CWeightFilter cw;
    cw.design(48000.0);
    double pkC = 0.0;
    for (int i = 0; i < 48000; ++i)
        pkC = std::max(pkC, std::fabs(cw.step(
                                std::sin(2.0 * kPi * 1000.0 * i / 48000.0))));
    std::printf("C-weight filter peak @1 kHz sine = %.3f (expected ~1.0)\n",
                pkC);
    ok = ok && std::fabs(pkC - 1.0) < 0.05;

    // Mic correction: a flat +6 dB response file must LOWER readings by 6 dB.
    an.setMicCorrection({{20.0, 6.0}, {20000.0, 6.0}});
    an.resetAll();
    for (int i = 0; i < 100; ++i)
        res = an.process(x, 0.05, 0.125, false);
    std::printf("with flat +6 dB mic file: fast = %.2f dBFS (expected ~ -9.01)\n",
                res.fast);
    ok = ok && std::fabs(res.fast + 9.01) < 0.25;

    ok = metricsSelftest() && ok;
    ok = loudnessSelftest() && ok;

    std::printf("%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

int main(int argc, char *argv[]) {
    for (int i = 1; i < argc; ++i)
        if (std::strcmp(argv[i], "--selftest") == 0)
            return selftest();
    QApplication app(argc, argv);
    QApplication::setStyle("Fusion");
    QPalette pal;
    pal.setColor(QPalette::Window, QColor("#1a1d24"));
    pal.setColor(QPalette::WindowText, QColor("#c8cede"));
    pal.setColor(QPalette::Base, QColor("#232733"));
    pal.setColor(QPalette::AlternateBase, QColor("#20242e"));
    pal.setColor(QPalette::Text, QColor("#c8cede"));
    pal.setColor(QPalette::Button, QColor("#2a2f3d"));
    pal.setColor(QPalette::ButtonText, QColor("#c8cede"));
    pal.setColor(QPalette::ToolTipBase, QColor("#232733"));
    pal.setColor(QPalette::ToolTipText, QColor("#c8cede"));
    pal.setColor(QPalette::Highlight, QColor("#2fbf9b"));
    pal.setColor(QPalette::HighlightedText, QColor("#10131a"));
    pal.setColor(QPalette::PlaceholderText, QColor("#5a6172"));
    pal.setColor(QPalette::Disabled, QPalette::Text, QColor("#5a6172"));
    pal.setColor(QPalette::Disabled, QPalette::ButtonText, QColor("#5a6172"));
    app.setPalette(pal);
    app.setStyleSheet(APP_STYLE);
    app.setApplicationName("ProdMesh Remote RTA");
    app.setOrganizationName("ProdMesh");
    app.setApplicationVersion(APP_VERSION);
#ifndef Q_OS_MACOS
    // Windows/Linux taskbar + title bar icon; macOS uses the bundle's
    // AppIcon.icns (a window icon there would show as a document proxy).
    app.setWindowIcon(QIcon(":/icon-256.png"));
#endif
    MainWindow win;
    win.resize(1000, 720);
    win.show();
    return app.exec();
}
