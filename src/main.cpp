// RTA — a minimal cross-platform (Windows/macOS) SPL meter and 1/3-octave
// real-time analyzer with spectrogram, SPL history, and a polling HTTP API.
//
// See dsp.h (analysis), audio.h (capture), widgets.h (views), api.h (HTTP).

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMediaDevices>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStatusBar>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <cstdio>
#include <cstring>

#include "api.h"
#include "audio.h"
#include "dsp.h"
#include "widgets.h"

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle("RTA — SPL Meter & Spectrum Analyzer");

        auto *central = new QWidget;
        setCentralWidget(central);
        central->setStyleSheet("background:#1a1d24; color:#c8cede;");
        auto *root = new QVBoxLayout(central);

        // --- controls row 1: measurement ---
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

        // --- controls row 2: API server ---
        auto *ctl2 = new QHBoxLayout;
        m_apiCheck = new QCheckBox("HTTP API");
        m_apiCheck->setToolTip(
            "Serve live levels and history as JSON so other machines can "
            "poll them.\nEndpoints: /api/status /api/spl /api/rta "
            "/api/history");
        ctl2->addWidget(m_apiCheck);
        ctl2->addWidget(new QLabel("Port:"));
        m_portSpin = new QSpinBox;
        m_portSpin->setRange(1024, 65535);
        m_portSpin->setValue(8517);
        ctl2->addWidget(m_portSpin);
        ctl2->addSpacing(12);
        m_apiUrlLbl = new QLabel;
        m_apiUrlLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        m_apiUrlLbl->setStyleSheet("color:#8a92a6;");
        ctl2->addWidget(m_apiUrlLbl);
        ctl2->addStretch(1);
        root->addLayout(ctl2);

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

        // --- RTA / spectrogram tabs + SPL history strip ---
        m_rta = new RtaWidget;
        m_spectro = new SpectrogramWidget;
        m_tabs = new QTabWidget;
        m_tabs->addTab(m_rta, "RTA");
        m_tabs->addTab(m_spectro, "Spectrogram");
        m_tabs->setStyleSheet(
            "QTabBar::tab { background:#232733; color:#8a92a6; padding:4px 14px; }"
            "QTabBar::tab:selected { background:#2a2e39; color:#e8ecf4; }");
        root->addWidget(m_tabs, 1);
        m_history = new HistoryWidget;
        root->addWidget(m_history);

        statusBar()->setStyleSheet("color:#8a92a6;");

        m_api = new ApiServer(this);

        loadSettings();

        // --- wiring (after loadSettings so restores don't retrigger) ---
        populateDevices();
        QObject::connect(m_deviceCombo, &QComboBox::currentIndexChanged, this,
                         [this](int row) {
                             changeDevice(row);
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
        QObject::connect(resetBtn, &QPushButton::clicked, this, [this] {
            m_analyzer.resetLeq();
            m_analyzer.resetPeaks();
        });
        QObject::connect(m_apiCheck, &QCheckBox::toggled, this, [this](bool) {
            applyApiState();
            saveSettings();
        });
        QObject::connect(m_portSpin, &QSpinBox::valueChanged, this, [this](int) {
            applyApiState();
            saveSettings();
        });

        // --api [port] command-line override (handy for headless testing)
        const QStringList args = QApplication::arguments();
        const int apiIdx = args.indexOf("--api");
        if (apiIdx >= 0) {
            if (apiIdx + 1 < args.size()) {
                bool ok = false;
                const int p = args[apiIdx + 1].toInt(&ok);
                if (ok)
                    m_portSpin->setValue(p);
            }
            m_apiCheck->setChecked(true);
        }

        m_analyzer.weighting =
            m_weightCombo->currentText().isEmpty()
                ? 'A'
                : m_weightCombo->currentText()[0].toLatin1();

        auto *timer = new QTimer(this);
        timer->setInterval(UPDATE_MS);
        QObject::connect(timer, &QTimer::timeout, this, [this] { tick(); });
        timer->start();

        changeDevice(m_deviceCombo->currentIndex());
        applyApiState();
    }

    ~MainWindow() override { m_engine.stop(); }

protected:
    void closeEvent(QCloseEvent *event) override {
        saveSettings();
        m_engine.stop();
        QMainWindow::closeEvent(event);
    }

private:
    void loadSettings() {
        QSettings st("RTA", "RTA");
        m_calSpin->setValue(st.value("cal", 100.0).toDouble());
        const QString w = st.value("weighting", "A").toString();
        if (m_weightCombo->findText(w) >= 0)
            m_weightCombo->setCurrentText(w);
        m_avgCombo->setCurrentIndex(st.value("rtaAvg", 1).toInt());
        m_peakCheck->setChecked(st.value("peakHold", false).toBool());
        m_apiCheck->setChecked(st.value("apiEnabled", false).toBool());
        m_portSpin->setValue(st.value("apiPort", 8517).toInt());
        m_savedDevice = st.value("device").toString();
        const QByteArray geo = st.value("geometry").toByteArray();
        if (!geo.isEmpty())
            restoreGeometry(geo);
    }

    void saveSettings() {
        QSettings st("RTA", "RTA");
        st.setValue("cal", m_calSpin->value());
        st.setValue("weighting", m_weightCombo->currentText());
        st.setValue("rtaAvg", m_avgCombo->currentIndex());
        st.setValue("peakHold", m_peakCheck->isChecked());
        st.setValue("apiEnabled", m_apiCheck->isChecked());
        st.setValue("apiPort", m_portSpin->value());
        if (m_deviceCombo->currentIndex() >= 0)
            st.setValue("device", m_deviceCombo->currentText());
        st.setValue("geometry", saveGeometry());
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

    void applyApiState() {
        if (m_apiCheck->isChecked()) {
            const quint16 port = quint16(m_portSpin->value());
            if (m_api->listen(port)) {
                m_apiUrlLbl->setText(
                    QString("%1/api/spl").arg(ApiServer::localUrl(port)));
            } else {
                m_apiUrlLbl->setText(
                    QString("API error: %1").arg(m_api->errorString()));
            }
        } else {
            m_api->close();
            m_apiUrlLbl->clear();
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
        m_spectro->pushColumn(m_analyzer.lastPower(), m_analyzer.binWidth(),
                              cal);

        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        m_history->setRange(cal - 80.0, cal + 20.0);
        m_history->push(now, res.fast + cal, res.slow + cal);

        ApiServer::Snapshot snap;
        snap.timeMs = now;
        snap.samplerate = m_engine.sampleRate();
        snap.weighting = w;
        snap.cal = cal;
        snap.fast = res.fast + cal;
        snap.slow = res.slow + cal;
        snap.leq = res.leq + cal;
        snap.bands = res.bands;
        snap.peaks = res.peaks;
        m_api->setSnapshot(snap);
        if (now - m_lastHistPush >= 1000) {
            m_lastHistPush = now;
            m_api->pushHistory(
                {now, res.fast + cal, res.slow + cal, res.leq + cal});
        }
    }

    AudioEngine m_engine;
    Analyzer m_analyzer;
    std::vector<float> m_samples;
    QList<QAudioDevice> m_devices;
    QString m_savedDevice;
    ApiServer *m_api;
    QComboBox *m_deviceCombo;
    QComboBox *m_weightCombo;
    QComboBox *m_avgCombo;
    QCheckBox *m_peakCheck;
    QDoubleSpinBox *m_calSpin;
    QCheckBox *m_apiCheck;
    QSpinBox *m_portSpin;
    QLabel *m_apiUrlLbl;
    QLabel *m_clipLbl;
    SplReadout *m_rFast;
    SplReadout *m_rSlow;
    SplReadout *m_rLeq;
    RtaWidget *m_rta;
    SpectrogramWidget *m_spectro;
    HistoryWidget *m_history;
    QTabWidget *m_tabs;
    int m_clipTicks = 0;
    qint64 m_lastHistPush = 0;
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
    win.resize(1000, 720);
    win.show();
    return app.exec();
}
