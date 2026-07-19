// Minimal read-only HTTP/JSON API so external tools (e.g. a logger on
// another machine) can poll live levels and fetch service-long history.
//
//   GET /api/status            server + measurement configuration
//   GET /api/spl               current Fast/Slow/Leq in dB SPL
//   GET /api/rta               current 1/3-octave band levels
//   GET /api/history           1 Hz SPL samples, up to 6 h
//         ?since_ms=<epoch ms>  only samples newer than this (incremental poll)
//         ?limit=<n>            at most n newest samples
//
// All responses are JSON with Access-Control-Allow-Origin: *. GET only —
// nothing on this server mutates state.
#pragma once

#include <QElapsedTimer>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QObject>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUrl>
#include <QUrlQuery>

#include <deque>

#include "dsp.h"

class ApiServer : public QObject {
public:
    struct Snapshot {
        qint64 timeMs = 0;
        int samplerate = 0;
        QString weighting = "A";
        double cal = 100.0;
        double fast = kNaN, slow = kNaN, leq = kNaN;  // dB SPL (cal applied)
        std::vector<double> bands;                    // dB SPL
        std::vector<double> peaks;                    // empty if peak hold off
    };
    struct HistSample {
        qint64 t = 0;
        double fast = kNaN, slow = kNaN, leq = kNaN;  // dB SPL
    };

    explicit ApiServer(QObject *parent = nullptr) : QObject(parent) {
        m_started.start();
        connect(&m_server, &QTcpServer::newConnection, this, [this] {
            while (QTcpSocket *sock = m_server.nextPendingConnection()) {
                connect(sock, &QTcpSocket::readyRead, sock,
                        [this, sock] { onData(sock); });
                connect(sock, &QTcpSocket::disconnected, sock,
                        &QObject::deleteLater);
            }
        });
    }

    bool listen(quint16 port) {
        close();
        return m_server.listen(QHostAddress::Any, port);
    }
    void close() {
        if (m_server.isListening())
            m_server.close();
    }
    bool isListening() const { return m_server.isListening(); }
    QString errorString() const { return m_server.errorString(); }

    void setSnapshot(const Snapshot &s) { m_snap = s; }

    void pushHistory(const HistSample &h) {
        m_hist.push_back(h);
        while (m_hist.size() > kMaxHist)
            m_hist.pop_front();
    }

    // Best URL to reach this machine from the LAN.
    static QString localUrl(quint16 port) {
        for (const QHostAddress &a : QNetworkInterface::allAddresses())
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback())
                return QString("http://%1:%2").arg(a.toString()).arg(port);
        return QString("http://127.0.0.1:%1").arg(port);
    }

private:
    static QJsonValue jnum(double v) {
        return std::isfinite(v) ? QJsonValue(v) : QJsonValue();
    }
    static QJsonArray jarr(const std::vector<double> &v) {
        QJsonArray a;
        for (double x : v)
            a.append(jnum(x));
        return a;
    }

    void onData(QTcpSocket *sock) {
        QByteArray buf = sock->property("reqbuf").toByteArray() + sock->readAll();
        sock->setProperty("reqbuf", buf);
        const int hdrEnd = buf.indexOf("\r\n\r\n");
        if (hdrEnd < 0) {
            if (buf.size() > 16384)
                sock->close();
            return;
        }
        const QByteArray reqLine = buf.left(buf.indexOf("\r\n"));
        const QList<QByteArray> parts = reqLine.split(' ');
        if (parts.size() < 2) {
            respond(sock, 400, QJsonDocument(QJsonObject{{"error", "bad request"}}));
            return;
        }
        const QByteArray method = parts[0];
        const QString target = QString::fromUtf8(parts[1]);
        if (method == "OPTIONS") {
            respondRaw(sock, "204 No Content", QByteArray());
            return;
        }
        if (method != "GET") {
            respond(sock, 405, QJsonDocument(QJsonObject{{"error", "GET only"}}));
            return;
        }
        bool found = true;
        const QJsonDocument doc = route(target, found);
        respond(sock, found ? 200 : 404, doc);
    }

    QJsonDocument route(const QString &target, bool &found) {
        const QUrl url = QUrl(target);
        const QString path = url.path();
        const QUrlQuery query(url);
        const qint64 now = m_snap.timeMs;

        if (path == "/" || path == "/api" || path == "/api/") {
            return QJsonDocument(QJsonObject{
                {"app", "rta"},
                {"endpoints", QJsonArray{"/api/status", "/api/spl", "/api/rta",
                                         "/api/history?since_ms=&limit="}},
            });
        }
        if (path == "/api/status") {
            return QJsonDocument(QJsonObject{
                {"app", "rta"},
                {"samplerate", m_snap.samplerate},
                {"weighting", m_snap.weighting},
                {"cal_db", m_snap.cal},
                {"fft_size", FFT_SIZE},
                {"update_ms", UPDATE_MS},
                {"uptime_s", double(m_started.elapsed()) / 1000.0},
                {"time_ms", now},
                {"history_len", int(m_hist.size())},
            });
        }
        if (path == "/api/spl") {
            return QJsonDocument(QJsonObject{
                {"time_ms", now},
                {"weighting", m_snap.weighting},
                {"cal_db", m_snap.cal},
                {"fast_db", jnum(m_snap.fast)},
                {"slow_db", jnum(m_snap.slow)},
                {"leq_db", jnum(m_snap.leq)},
            });
        }
        if (path == "/api/rta") {
            QJsonArray centers;
            for (double c : THIRD_OCT_CENTERS)
                centers.append(c);
            QJsonObject o{
                {"time_ms", now},
                {"weighting", m_snap.weighting},
                {"cal_db", m_snap.cal},
                {"centers_hz", centers},
                {"bands_db", jarr(m_snap.bands)},
            };
            o.insert("peaks_db",
                     m_snap.peaks.empty() ? QJsonValue() : jarr(m_snap.peaks));
            return QJsonDocument(o);
        }
        if (path == "/api/history") {
            const qint64 since = query.queryItemValue("since_ms").toLongLong();
            qint64 limit = query.hasQueryItem("limit")
                               ? query.queryItemValue("limit").toLongLong()
                               : qint64(kMaxHist);
            limit = std::clamp<qint64>(limit, 0, qint64(kMaxHist));
            QJsonArray samples;
            qint64 skipped = qint64(m_hist.size());
            for (auto it = m_hist.begin(); it != m_hist.end(); ++it, --skipped) {
                if (it->t <= since || skipped > limit)
                    continue;
                samples.append(QJsonObject{
                    {"t", it->t},
                    {"fast_db", jnum(it->fast)},
                    {"slow_db", jnum(it->slow)},
                    {"leq_db", jnum(it->leq)},
                });
            }
            return QJsonDocument(QJsonObject{
                {"interval_ms", 1000},
                {"count", samples.size()},
                {"samples", samples},
            });
        }
        found = false;
        return QJsonDocument(QJsonObject{{"error", "not found"}});
    }

    void respond(QTcpSocket *sock, int code, const QJsonDocument &doc) {
        const char *phrase = code == 200   ? "200 OK"
                             : code == 400 ? "400 Bad Request"
                             : code == 404 ? "404 Not Found"
                                           : "405 Method Not Allowed";
        respondRaw(sock, phrase, doc.toJson(QJsonDocument::Compact));
    }

    void respondRaw(QTcpSocket *sock, const char *status, const QByteArray &body) {
        QByteArray resp = "HTTP/1.1 ";
        resp += status;
        resp += "\r\nContent-Type: application/json\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "Access-Control-Allow-Methods: GET, OPTIONS\r\n"
                "Connection: close\r\n"
                "Content-Length: " + QByteArray::number(body.size()) +
                "\r\n\r\n";
        resp += body;
        sock->write(resp);
        sock->disconnectFromHost();
    }

    static constexpr size_t kMaxHist = 6 * 3600;  // 6 h at 1 Hz
    QTcpServer m_server;
    Snapshot m_snap;
    std::deque<HistSample> m_hist;
    QElapsedTimer m_started;
};
