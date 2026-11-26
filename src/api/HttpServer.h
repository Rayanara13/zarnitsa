// Маленький HTTP/1.1-сервер на QTcpServer: JSON-ответы, поток SSE, по желанию TLS.
#pragma once

#include <QHash>
#include <QHostAddress>
#include <QObject>
#include <QPointer>
#include <QSslCertificate>
#include <QSslKey>
#include <QTcpServer>
#include <QTimer>
#include <QUrlQuery>
#include <functional>

class QTcpSocket;

namespace zr {

struct HttpRequest {
    QByteArray method;
    QString path;
    QUrlQuery query;
    QHash<QByteArray, QByteArray> headers;   // имена в нижнем регистре
    QByteArray body;
    QHostAddress peer;
    QByteArray header(const QByteArray &name) const { return headers.value(name.toLower()); }
};

struct HttpResponse {
    int status = 200;
    QByteArray contentType = "application/json; charset=utf-8";
    QByteArray body;
    QList<QPair<QByteArray, QByteArray>> headers;
};

class HttpConnection : public QObject {
    Q_OBJECT
public:
    // Предел очереди SSE на соединение; уменьшение удобно для детерминированных проверок.
    HttpConnection(QTcpSocket *sock, QObject *parent, qint64 maxStreamBytes = 256 * 1024);
    void respond(const HttpResponse &r);
    void startEventStream();
    void sendEvent(const QByteArray &event, const QByteArray &data);
    void sendComment(const QByteArray &text);
    bool isStream() const { return m_stream; }

signals:
    void request(const zr::HttpRequest &req, zr::HttpConnection *conn);
    void closed(zr::HttpConnection *conn);

private:
    void onReadyRead();
    void fail(int status, const QByteArray &code);
    void closeLater();
    void writeStream(const QByteArray &data);

    QTcpSocket *m_sock;
    const qint64 m_maxStreamBytes;
    QByteArray m_buf;
    bool m_headersDone = false;
    bool m_dispatched = false;
    bool m_stream = false;
    qint64 m_contentLength = 0;
    HttpRequest m_req;
    QTimer m_idle;
};

class HttpServer : public QTcpServer {
    Q_OBJECT
public:
    using Handler = std::function<void(const HttpRequest &, HttpConnection *)>;

    explicit HttpServer(QObject *parent = nullptr);
    void setHandler(Handler h) { m_handler = std::move(h); }
    void setTls(const QSslCertificate &cert, const QSslKey &key);
    bool tls() const { return !m_cert.isNull(); }
    void broadcast(const QByteArray &event, const QByteArray &data);
    int streamCount() const;

    static QByteArray reasonPhrase(int status);

protected:
    void incomingConnection(qintptr fd) override;

private:
    Handler m_handler;
    QSslCertificate m_cert;
    QSslKey m_key;
    QList<QPointer<HttpConnection>> m_conns;
    QTimer m_heartbeat;
};

} // namespace zr

Q_DECLARE_METATYPE(zr::HttpRequest)
