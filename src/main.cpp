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
#include <QDoubleSpinBox>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMainWindow>
#include <QMediaDevices>
#include <QMenu>
#include <QMenuBar>
#include <QMessageBox>
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

#ifndef APP_VERSION
#define APP_VERSION "dev"
#endif

static const char *APP_NAME = "ProdMesh Remote RTA";

// ---------------------------------------------------------------------------

class ApiSettingsDialog : public QDialog {
public:
    ApiSettingsDialog(QWidget *parent, bool enabled, int port, int rateIdx)
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

        rateCombo = new QComboBox;
        rateCombo->addItems({"1 Hz", "5 Hz", "10 Hz", "20 Hz"});
        rateCombo->setCurrentIndex(rateIdx);
        rateCombo->setToolTip("How often /api/stream WebSocket clients "
                              "receive a levels message.");
        form->addRow("Stream rate:", rateCombo);

        auto *hint = new QLabel(
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

    QCheckBox *enableCheck;
    QSpinBox *portSpin;
    QComboBox *rateCombo;
};

// ---------------------------------------------------------------------------

class MainWindow : public QMainWindow {
public:
    MainWindow() {
        setWindowTitle(APP_NAME);

        auto *central = new QWidget;
        setCentralWidget(central);
        central->setStyleSheet("background:#1a1d24; color:#c8cede;");
        auto *root = new QVBoxLayout(central);

        buildMenus();

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
        m_apiLbl = new QLabel;
        m_apiLbl->setTextInteractionFlags(Qt::TextSelectableByMouse);
        statusBar()->addPermanentWidget(m_apiLbl);

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
    void buildMenus() {
        menuBar()->setStyleSheet(
            "QMenuBar { background:#1a1d24; color:#c8cede; }"
            "QMenuBar::item:selected { background:#2a2e39; }"
            "QMenu { background:#232733; color:#c8cede; }"
            "QMenu::item:selected { background:#2a2e39; }");

        QMenu *fileMenu = menuBar()->addMenu("&File");
        QAction *quitAct = fileMenu->addAction("&Quit");
        quitAct->setShortcut(QKeySequence::Quit);
        connect(quitAct, &QAction::triggered, this, &QWidget::close);

        QMenu *settingsMenu = menuBar()->addMenu("&Settings");
        QAction *apiAct = settingsMenu->addAction("&API && Streaming…");
        connect(apiAct, &QAction::triggered, this, [this] { showApiSettings(); });

        QMenu *helpMenu = menuBar()->addMenu("&Help");
        QAction *aboutAct =
            helpMenu->addAction(QString("&About %1").arg(APP_NAME));
        connect(aboutAct, &QAction::triggered, this, [this] { showAbout(); });
        QAction *aboutQtAct = helpMenu->addAction("About &Qt");
        connect(aboutQtAct, &QAction::triggered, qApp, &QApplication::aboutQt);
    }

    void showApiSettings() {
        ApiSettingsDialog dlg(this, m_apiEnabled, m_apiPort, m_streamRateIdx);
        if (dlg.exec() != QDialog::Accepted)
            return;
        m_apiEnabled = dlg.enableCheck->isChecked();
        m_apiPort = dlg.portSpin->value();
        m_streamRateIdx = dlg.rateCombo->currentIndex();
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
        m_streamRateIdx = st.value("streamRate", 2).toInt();  // default 10 Hz
        m_savedDevice = st.value("device").toString();
        const QByteArray geo = st.value("geometry").toByteArray();
        if (!geo.isEmpty())
            restoreGeometry(geo);
    }

    void saveSettings() {
        QSettings st("ProdMesh", "RemoteRTA");
        st.setValue("cal", m_calSpin->value());
        st.setValue("weighting", m_weightCombo->currentText());
        st.setValue("rtaAvg", m_avgCombo->currentIndex());
        st.setValue("peakHold", m_peakCheck->isChecked());
        st.setValue("apiEnabled", m_apiEnabled);
        st.setValue("apiPort", m_apiPort);
        st.setValue("streamRate", m_streamRateIdx);
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
        if (m_apiEnabled) {
            const quint16 port = quint16(m_apiPort);
            if (m_api->listen(port)) {
                m_apiLbl->setStyleSheet("color:#8a92a6;");
                m_apiLbl->setText(
                    QString("API %1/api").arg(ApiServer::localUrl(port)));
            } else {
                m_apiLbl->setStyleSheet("color:#e05c5c;");
                m_apiLbl->setText(
                    QString("API error: %1").arg(m_api->errorString()));
            }
        } else {
            m_api->close();
            m_apiLbl->clear();
        }
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
        // -UPDATE_MS/2: the check runs on the tick grid, so without slack a
        // 100 ms interval lands on alternating 100/150 ms ticks (~8 Hz).
        if (now - m_lastStream >= streamIntervalMs() - UPDATE_MS / 2) {
            m_lastStream = now;
            m_api->broadcastSnapshot();
        }
    }

    AudioEngine m_engine;
    Analyzer m_analyzer;
    std::vector<float> m_samples;
    QList<QAudioDevice> m_devices;
    QString m_savedDevice;
    ApiServer *m_api;
    bool m_apiEnabled = false;
    int m_apiPort = 8517;
    int m_streamRateIdx = 2;  // 10 Hz
    QComboBox *m_deviceCombo;
    QComboBox *m_weightCombo;
    QComboBox *m_avgCombo;
    QCheckBox *m_peakCheck;
    QDoubleSpinBox *m_calSpin;
    QLabel *m_apiLbl;
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
    qint64 m_lastStream = 0;
    qint64 m_lastTickMs = 0;
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
    app.setApplicationName("ProdMesh Remote RTA");
    app.setOrganizationName("ProdMesh");
    app.setApplicationVersion(APP_VERSION);
    MainWindow win;
    win.resize(1000, 720);
    win.show();
    return app.exec();
}
