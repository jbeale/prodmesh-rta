// Audio capture into a ring buffer via QAudioSource.
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
            m_peakC = 0.0f;
            m_cw.design(fmt.sampleRate());
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
    // ring buffer is still filling. `peakOut` / `peakCOut` are the raw and
    // C-weighted sample peaks since the last call.
    bool latest(int n, std::vector<float> &out, float &peakOut,
                float &peakCOut) {
        QMutexLocker lock(&m_mutex);
        peakOut = m_peak;
        m_peak = 0.0f;
        peakCOut = m_peakC;
        m_peakC = 0.0f;
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
            const float magC = float(std::fabs(m_cw.step(m_conv[i])));
            if (magC > m_peakC)
                m_peakC = magC;
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
    float m_peakC = 0.0f;
    CWeightFilter m_cw;
};
