// HTTP/JSON + WebSocket API so external tools (e.g. ProdMesh on another
// machine) can poll or stream live levels and fetch service-long history.
//
//   GET /                      live browser dashboard (webui.h)
//   GET /api/status            server + measurement configuration
//   GET /api/spl               current Fast/Slow/Leq in dB SPL
//   GET /api/rta               current 1/3-octave band levels
//   GET /api/history           1 Hz SPL samples, up to 6 h
//         ?since_ms=<epoch ms>  only samples newer than this (incremental poll)
//         ?limit=<n>            at most n newest samples
//   WS  /api/stream            pushes the /api/spl+/api/rta payload at the
//                              configured stream rate (RFC 6455 text frames)
//
// JSON responses carry Access-Control-Allow-Origin: *. GET only — nothing on
// this server mutates state. The WebSocket side is hand-rolled on QTcpServer
// (server->client push only) so no extra Qt modules are required.
#pragma once

#include <QCryptographicHash>
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

#include <algorithm>
#include <deque>
#include <vector>

#include "dsp.h"
#include "webui.h"

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
        QString micCorr;                              // cal file name, or empty
        // Derived SPL metrics (id -> value); see the Metrics dialog for ids.
        std::vector<std::pair<QString, double>> metrics;
        // Traffic-light alarm on one watched metric.
        bool alarmEnabled = false;
        QString alarmMetric;
        int alarmState = 0;  // 0 ok, 1 warning, 2 alert
        double alarmWarn = 0.0, alarmAlert = 0.0;
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
                connect(sock, &QTcpSocket::disconnected, this,
                        [this, sock] { removeStream(sock); });
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
        for (QTcpSocket *s : m_streams)
            s->disconnectFromHost();
        m_streams.clear();
    }

    bool isListening() const { return m_server.isListening(); }
    QString errorString() const { return m_server.errorString(); }
    int streamClientCount() const { return int(m_streams.size()); }

    void setSnapshot(const Snapshot &s) { m_snap = s; }

    void pushHistory(const HistSample &h) {
        m_hist.push_back(h);
        while (m_hist.size() > kMaxHist)
            m_hist.pop_front();
    }

    // Push the current snapshot to every /api/stream WebSocket client.
    void broadcastSnapshot() {
        if (m_streams.empty())
            return;
        const QByteArray payload =
            QJsonDocument(levelsJson()).toJson(QJsonDocument::Compact);
        for (QTcpSocket *s : m_streams)
            sendFrame(s, 0x1, payload);
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

    QJsonObject levelsJson() const {
        QJsonArray centers;
        for (double c : THIRD_OCT_CENTERS)
            centers.append(c);
        QJsonObject o{
            {"type", "levels"},
            {"time_ms", m_snap.timeMs},
            {"weighting", m_snap.weighting},
            {"cal_db", m_snap.cal},
            {"fast_db", jnum(m_snap.fast)},
            {"slow_db", jnum(m_snap.slow)},
            {"leq_db", jnum(m_snap.leq)},
            {"centers_hz", centers},
            {"bands_db", jarr(m_snap.bands)},
        };
        o.insert("peaks_db",
                 m_snap.peaks.empty() ? QJsonValue() : jarr(m_snap.peaks));
        o.insert("metrics", metricsJson());
        o.insert("alarm", alarmJson());
        return o;
    }

    QJsonObject metricsJson() const {
        QJsonObject met;
        for (const auto &m : m_snap.metrics)
            met.insert(m.first, jnum(m.second));
        return met;
    }

    QJsonObject alarmJson() const {
        return QJsonObject{
            {"enabled", m_snap.alarmEnabled},
            {"metric", m_snap.alarmMetric},
            {"state", m_snap.alarmState},
            {"warn_db", m_snap.alarmWarn},
            {"alert_db", m_snap.alarmAlert},
        };
    }

    void onData(QTcpSocket *sock) {
        if (sock->property("ws").toBool()) {
            onWsData(sock);
            return;
        }
        QByteArray buf = sock->property("reqbuf").toByteArray() + sock->readAll();
        sock->setProperty("reqbuf", buf);
        const int hdrEnd = buf.indexOf("\r\n\r\n");
        if (hdrEnd < 0) {
            if (buf.size() > 16384)
                sock->close();
            return;
        }
        const QByteArray header = buf.left(hdrEnd);
        const QByteArray reqLine = header.left(header.indexOf("\r\n") >= 0
                                                   ? header.indexOf("\r\n")
                                                   : header.size());
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
        const QString path = QUrl(target).path();
        if (path == "/api/stream") {
            upgradeToWs(sock, header);
            return;
        }
        if (path == "/" || path == "/index.html") {
            respondRaw(sock, "200 OK", QByteArray(kDashboardHtml),
                       "text/html; charset=utf-8");
            return;
        }
        bool found = true;
        const QJsonDocument doc = route(target, found);
        respond(sock, found ? 200 : 404, doc);
    }

    // --- WebSocket (RFC 6455, server push only) ---

    void upgradeToWs(QTcpSocket *sock, const QByteArray &header) {
        QByteArray key;
        for (const QByteArray &line : header.split('\r')) {
            const QByteArray l = line.startsWith('\n') ? line.mid(1) : line;
            static const QByteArray kKeyHeader = "sec-websocket-key:";
            if (l.toLower().startsWith(kKeyHeader))
                key = l.mid(kKeyHeader.size()).trimmed();
        }
        if (key.isEmpty()) {
            respond(sock, 400,
                    QJsonDocument(QJsonObject{
                        {"error", "WebSocket endpoint — connect with ws://"}}));
            return;
        }
        const QByteArray accept =
            QCryptographicHash::hash(
                key + "258EAFA5-E914-47DA-95CA-C5AB0DC85B11",
                QCryptographicHash::Sha1)
                .toBase64();
        sock->write("HTTP/1.1 101 Switching Protocols\r\n"
                    "Upgrade: websocket\r\n"
                    "Connection: Upgrade\r\n"
                    "Sec-WebSocket-Accept: " + accept + "\r\n\r\n");
        sock->setProperty("ws", true);
        sock->setProperty("reqbuf", QByteArray());
        m_streams.push_back(sock);
        // Greet with the current snapshot so clients render immediately.
        sendFrame(sock, 0x1,
                  QJsonDocument(levelsJson()).toJson(QJsonDocument::Compact));
    }

    void onWsData(QTcpSocket *sock) {
        QByteArray buf = sock->property("wsbuf").toByteArray() + sock->readAll();
        while (buf.size() >= 2) {
            const quint8 b0 = quint8(buf[0]);
            const quint8 b1 = quint8(buf[1]);
            const quint8 opcode = b0 & 0x0F;
            const bool masked = b1 & 0x80;
            qint64 len = b1 & 0x7F;
            int pos = 2;
            if (len == 126) {
                if (buf.size() < 4)
                    break;
                len = (quint8(buf[2]) << 8) | quint8(buf[3]);
                pos = 4;
            } else if (len == 127) {
                if (buf.size() < 10)
                    break;
                len = 0;
                for (int i = 0; i < 8; ++i)
                    len = (len << 8) | quint8(buf[2 + i]);
                pos = 10;
            }
            const int maskPos = pos;
            if (masked)
                pos += 4;
            if (buf.size() < pos + len)
                break;
            QByteArray payload = buf.mid(pos, int(len));
            if (masked)
                for (int i = 0; i < payload.size(); ++i)
                    payload[i] = payload[i] ^ buf[maskPos + (i & 3)];
            if (opcode == 0x8) {  // close
                sendFrame(sock, 0x8, payload);
                sock->disconnectFromHost();
            } else if (opcode == 0x9) {  // ping -> pong
                sendFrame(sock, 0xA, payload);
            }
            buf.remove(0, int(pos + len));
        }
        sock->setProperty("wsbuf", buf);
    }

    static void sendFrame(QTcpSocket *sock, quint8 opcode,
                          const QByteArray &payload) {
        QByteArray f;
        f.append(char(0x80 | opcode));
        const qint64 n = payload.size();
        if (n < 126) {
            f.append(char(n));
        } else if (n < 65536) {
            f.append(char(126));
            f.append(char((n >> 8) & 0xFF));
            f.append(char(n & 0xFF));
        } else {
            f.append(char(127));
            for (int i = 7; i >= 0; --i)
                f.append(char((n >> (8 * i)) & 0xFF));
        }
        f.append(payload);
        sock->write(f);
    }

    void removeStream(QTcpSocket *sock) {
        m_streams.erase(std::remove(m_streams.begin(), m_streams.end(), sock),
                        m_streams.end());
    }

    // --- HTTP routing ---

    QJsonDocument route(const QString &target, bool &found) {
        const QUrl url = QUrl(target);
        const QString path = url.path();
        const QUrlQuery query(url);
        const qint64 now = m_snap.timeMs;

        if (path == "/api" || path == "/api/") {
            return QJsonDocument(QJsonObject{
                {"app", "prodmesh-remote-rta"},
                {"dashboard", "/"},
                {"endpoints",
                 QJsonArray{"/api/status", "/api/spl", "/api/rta",
                            "/api/history?since_ms=&limit=",
                            "ws: /api/stream"}},
            });
        }
        if (path == "/api/status") {
            return QJsonDocument(QJsonObject{
                {"app", "prodmesh-remote-rta"},
                {"samplerate", m_snap.samplerate},
                {"weighting", m_snap.weighting},
                {"cal_db", m_snap.cal},
                {"fft_size", FFT_SIZE},
                {"update_ms", UPDATE_MS},
                {"uptime_s", double(m_started.elapsed()) / 1000.0},
                {"time_ms", now},
                {"history_len", int(m_hist.size())},
                {"stream_clients", int(m_streams.size())},
                {"mic_correction", m_snap.micCorr.isEmpty()
                                       ? QJsonValue()
                                       : QJsonValue(m_snap.micCorr)},
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
                {"metrics", metricsJson()},
                {"alarm", alarmJson()},
            });
        }
        if (path == "/api/rta") {
            QJsonObject o = levelsJson();
            o.remove("type");
            return QJsonDocument(o);
        }
        if (path == "/api/history") {
            const qint64 since = query.queryItemValue("since_ms").toLongLong();
            qint64 limit = query.hasQueryItem("limit")
                               ? query.queryItemValue("limit").toLongLong()
                               : qint64(kMaxHist);
            limit = std::clamp<qint64>(limit, 0, qint64(kMaxHist));
            QJsonArray samples;
            qint64 remaining = qint64(m_hist.size());
            for (auto it = m_hist.begin(); it != m_hist.end(); ++it, --remaining) {
                if (it->t <= since || remaining > limit)
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

    void respondRaw(QTcpSocket *sock, const char *status, const QByteArray &body,
                    const char *contentType = "application/json") {
        QByteArray resp = "HTTP/1.1 ";
        resp += status;
        resp += "\r\nContent-Type: ";
        resp += contentType;
        resp += "\r\nAccess-Control-Allow-Origin: *\r\n"
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
    std::vector<QTcpSocket *> m_streams;
    QElapsedTimer m_started;
};
