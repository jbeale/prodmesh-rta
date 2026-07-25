// Audio capture into a ring buffer via QAudioSource.
//
// All captured channels are kept interleaved in the ring; the collapse to a
// single analysis channel happens at read time. That keeps one code path for
// both app modes (Acoustic reads one channel, Program needs a stereo pair)
// and lets the channel selection change without discarding history.
#pragma once

#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSource>
#include <QIODevice>
#include <QMutex>
#include <QMutexLocker>
#include <QObject>

#include <cstring>
#include <stdexcept>
#include <vector>

#include "dsp.h"

class AudioEngine {
public:
    AudioEngine() { allocate(1); }
    ~AudioEngine() { stop(); }

    void start(const QAudioDevice &dev) {
        stop();
        // Capture at the device's native channel count and pick the wanted
        // channel ourselves — forcing mono here would leave the mixdown (or
        // the choice of channel) up to the driver.
        QAudioFormat fmt = dev.preferredFormat();
        QAudioFormat want = fmt;
        want.setSampleFormat(QAudioFormat::Float);
        if (dev.isFormatSupported(want))
            fmt = want;
        m_format = fmt;
        {
            QMutexLocker lock(&m_mutex);
            allocate(std::max(1, fmt.channelCount()));
            m_cw.design(fmt.sampleRate());
            for (auto &k : m_kw)
                k.design(fmt.sampleRate());
            m_subSamples = std::max(1, fmt.sampleRate() / 10);  // 100 ms
            resetLoudnessAccum();
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
    int channelCount() const { return m_format.channelCount(); }

    // Which capture channel feeds the analyzer: 0-based index, or -1 to
    // average all channels ("Mix"). Out-of-range indexes fall back to Mix.
    void setChannel(int ch) {
        QMutexLocker lock(&m_mutex);
        if (ch == m_channel)
            return;
        m_channel = ch;
        // The ring holds every channel, so the history for the new selection
        // is already correct — only the per-sample filter state and the
        // peak-since-last-read are stale.
        m_peak = 0.0f;
        m_peakC = 0.0f;
        m_cw.reset();
    }
    int channel() const { return m_channel; }

    // Loudness path (Program mode): K-weighting + true peak over one or two
    // channels, accumulated into 100 ms sub-blocks. Off by default so
    // Acoustic mode pays nothing for it.
    void setLoudness(bool on, int chL, int chR) {
        QMutexLocker lock(&m_mutex);
        m_loudOn = on;
        m_reqL = chL;
        m_reqR = chR;
        applyLoudnessRequest();
    }
    bool loudnessEnabled() const { return !m_loudCh.empty(); }

    // Hands over the sub-blocks completed since the last call.
    void takeLoudness(std::vector<LoudnessBlock> &out) {
        QMutexLocker lock(&m_mutex);
        out.swap(m_loudQ);
        m_loudQ.clear();
    }

    // Fills `out` with the most recent n samples of the selected channel (or
    // the mix). Returns false while the ring buffer is still filling.
    // `peakOut` / `peakCOut` are the raw and C-weighted sample peaks since
    // the last call.
    bool latest(int n, std::vector<float> &out, float &peakOut,
                float &peakCOut) {
        QMutexLocker lock(&m_mutex);
        peakOut = m_peak;
        m_peak = 0.0f;
        peakCOut = m_peakC;
        m_peakC = 0.0f;
        const int sel = (m_channel >= 0 && m_channel < m_ch) ? m_channel : -1;
        return copyOut(sel, n, out);
    }

    // Same, for one explicit channel — used by the stereo views.
    bool latest(int ch, int n, std::vector<float> &out) {
        QMutexLocker lock(&m_mutex);
        if (ch < 0 || ch >= m_ch)
            return false;
        return copyOut(ch, n, out);
    }

private:
    void allocate(int channels) {
        m_ch = channels;
        m_frames = FFT_SIZE * 4;
        m_buf.assign(size_t(m_frames) * m_ch, 0.0f);
        m_kw.resize(m_ch);
        m_tp.resize(m_ch);
        m_sumSq.assign(m_ch, 0.0);
        m_pos = 0;
        m_total = 0;
        m_peak = 0.0f;
        m_peakC = 0.0f;
        applyLoudnessRequest();
    }

    // Resolve the requested stereo pair against the channel count the device
    // actually opened with. Caller holds m_mutex.
    void applyLoudnessRequest() {
        const int ch = std::max(1, m_ch);
        std::vector<int> want;
        if (m_loudOn) {
            want.push_back(std::clamp(m_reqL, 0, ch - 1));
            const int r = std::clamp(m_reqR, 0, ch - 1);
            if (r != want.front())
                want.push_back(r);
        }
        if (want == m_loudCh)
            return;
        m_loudCh = want;
        for (auto &k : m_kw)
            k.reset();
        for (auto &t : m_tp)
            t.reset();
        resetLoudnessAccum();
        m_loudQ.clear();
    }

    // Opening a device (or switching the measured pair) hands us a click or
    // an uninitialised first buffer often enough to matter: fed straight into
    // the loudness path it pins the true-peak maximum above full scale and
    // skews integrated loudness for the whole session, since the relative
    // gate then discards the real programme as "quiet". Drop a short window
    // rather than making people hit Reset to get honest numbers.
    static constexpr double kSettleS = 0.3;

    void resetLoudnessAccum() {
        std::fill(m_sumSq.begin(), m_sumSq.end(), 0.0);
        m_subN = 0;
        m_subTp = 0.0;
        m_settle = int(kSettleS * std::max(1, m_format.sampleRate()));
    }

    // Caller holds m_mutex. sel < 0 means mix all channels.
    bool copyOut(int sel, int n, std::vector<float> &out) {
        if (std::min<qint64>(m_total, m_frames) < n)
            return false;
        out.resize(n);
        int start = int((m_pos - n) % m_frames);
        if (start < 0)
            start += m_frames;
        for (int i = 0; i < n; ++i) {
            const int f = start + i < m_frames ? start + i : start + i - m_frames;
            const float *fr = &m_buf[size_t(f) * m_ch];
            if (sel >= 0) {
                out[i] = fr[sel];
            } else {
                float acc = 0.0f;
                for (int c = 0; c < m_ch; ++c)
                    acc += fr[c];
                out[i] = acc / float(m_ch);
            }
        }
        return true;
    }

    void onReady() {
        if (!m_io)
            return;
        const QByteArray data = m_io->readAll();
        const int ch = m_format.channelCount();
        const int bps = m_format.bytesPerSample();
        if (ch <= 0 || bps <= 0 || ch != m_ch)
            return;
        const int frames = int(data.size()) / (ch * bps);
        if (frames <= 0)
            return;
        const char *p = data.constData();
        auto sample = [&](int i, int c) -> float {
            const char *sp = p + (size_t(i) * ch + c) * bps;
            switch (m_format.sampleFormat()) {
            case QAudioFormat::Float: {
                float f;
                std::memcpy(&f, sp, 4);
                return f;
            }
            case QAudioFormat::Int16: {
                qint16 s;
                std::memcpy(&s, sp, 2);
                return float(s / 32768.0);
            }
            case QAudioFormat::Int32: {
                qint32 s;
                std::memcpy(&s, sp, 4);
                return float(s / 2147483648.0);
            }
            case QAudioFormat::UInt8: {
                quint8 s;
                std::memcpy(&s, sp, 1);
                return float((int(s) - 128) / 128.0);
            }
            default:
                return 0.0f;
            }
        };

        QMutexLocker lock(&m_mutex);
        const int sel = (m_channel >= 0 && m_channel < ch) ? m_channel : -1;
        for (int i = 0; i < frames; ++i) {
            float *fr = &m_buf[size_t(m_pos) * m_ch];
            for (int c = 0; c < ch; ++c)
                fr[c] = sample(i, c);
            m_pos = m_pos + 1 < m_frames ? m_pos + 1 : 0;

            // Analysis-channel ballistics (unchanged meaning: the CLIP light
            // and LZpk/LCpk follow whatever channel the RTA is showing).
            float v;
            if (sel >= 0) {
                v = fr[sel];
            } else {
                float acc = 0.0f;
                for (int c = 0; c < ch; ++c)
                    acc += fr[c];
                v = acc / float(ch);
            }
            const float mag = std::fabs(v);
            if (mag > m_peak)
                m_peak = mag;
            const float magC = float(std::fabs(m_cw.step(v)));
            if (magC > m_peakC)
                m_peakC = magC;

            if (!m_loudCh.empty()) {
                // The filters are driven throughout so their state settles,
                // but nothing is measured until the settling window expires.
                for (int c : m_loudCh) {
                    const double k = m_kw[c].step(fr[c]);
                    const double t = m_tp[c].step(fr[c]);
                    if (m_settle > 0)
                        continue;
                    m_sumSq[c] += k * k;
                    m_subTp = std::max(m_subTp, t);
                }
                if (m_settle > 0) {
                    --m_settle;
                } else if (++m_subN >= m_subSamples) {
                    LoudnessBlock b;
                    for (int c : m_loudCh)
                        b.z += m_sumSq[c] / m_subN;
                    b.tpLin = m_subTp;
                    m_loudQ.push_back(b);
                    resetLoudnessAccum();
                }
            }
        }
        m_total += frames;
    }

    QAudioSource *m_source = nullptr;
    QIODevice *m_io = nullptr;
    QAudioFormat m_format;
    QMutex m_mutex;
    std::vector<float> m_buf;  // interleaved, m_frames x m_ch
    int m_ch = 1;
    int m_frames = 0;
    int m_pos = 0;  // next frame to write
    qint64 m_total = 0;
    float m_peak = 0.0f;
    float m_peakC = 0.0f;
    int m_channel = 0;  // 0-based capture channel; -1 = mix of all
    CWeightFilter m_cw;

    // Loudness path. The request is kept separately from the resolved
    // channel list so reopening a device with a different channel count
    // re-clamps it instead of silently switching loudness off.
    bool m_loudOn = false;
    int m_reqL = 0, m_reqR = 1;
    std::vector<int> m_loudCh;  // empty = disabled
    std::vector<KWeightFilter> m_kw;
    std::vector<TruePeakDetector> m_tp;
    std::vector<double> m_sumSq;
    std::vector<LoudnessBlock> m_loudQ;
    int m_subSamples = 4800;
    int m_subN = 0;
    int m_settle = 0;  // frames still to discard after a (re)start
    double m_subTp = 0.0;
};
