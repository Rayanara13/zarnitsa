#include "HttpServer.h"

#include <QSslSocket>
#include <QTcpSocket>
#include <QUrl>

namespace zr {

static constexpr int kMaxHeader = 16 * 1024;
static constexpr int kMaxBody = 1024 * 1024;
static constexpr int kMaxConnections = 128;
static constexpr int kIdleMs = 15000;

static bool isHeaderName(const QByteArray &name)
{
    if (name.isEmpty()) return false;
    static const QByteArray punctuation("!#$%&'*+-.^_`|~");
    for (char c : name) {
        if ((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || punctuation.contains(c))
            continue;
        return false;
    }
    return true;
}

QByteArray HttpServer::reasonPhrase(int s)
{
    switch (s) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 503: return "Service Unavailable";
    default: return "Status";
    }
}

HttpConnection::HttpConnection(QTcpSocket *sock, QObject *parent, qint64 maxStreamBytes)
    : QObject(parent)
    , m_sock(sock)
    , m_maxStreamBytes(qMax<qint64>(1, maxStreamBytes))
{
    m_sock->setParent(this);
    m_req.peer = sock->peerAddress();
    connect(m_sock, &QTcpSocket::readyRead, this, &HttpConnection::onReadyRead);
    connect(m_sock, &QTcpSocket::disconnected, this, [this]() {
        m_stream = false;
        m_idle.stop();
        deleteLater();
        emit closed(this);
    });
    m_idle.setSingleShot(true);
    connect(&m_idle, &QTimer::timeout, this, [this]() {
        if (!m_stream) m_sock->abort();
    });
    m_idle.start(kIdleMs);
}

void HttpConnection::closeLater()
{
    m_sock->disconnectFromHost();
}

void HttpConnection::fail(int status, const QByteArray &code)
{
    HttpResponse r;
    r.status = status;
    r.body = "{\"error\":\"" + code + "\"}";
    respond(r);
}

void HttpConnection::onReadyRead()
{
    if (m_dispatched) {
        m_sock->readAll();
        return;
    }
    m_buf += m_sock->readAll();
    if (!m_headersDone) {
        const int end = m_buf.indexOf("\r\n\r\n");
        if (end < 0) {
            if (m_buf.size() > kMaxHeader) fail(413, "request_too_large");
            return;
        }
        if (end + 4 > kMaxHeader) {
            fail(413, "request_too_large");
            return;
        }
        const QList<QByteArray> lines = m_buf.left(end).split('\n');
        m_buf.remove(0, end + 4);
        if (lines.size() > 1 && !lines.first().endsWith('\r')) {
            fail(400, "invalid_request");
            return;
        }
        const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
        if (first.size() < 3 || !first[2].startsWith("HTTP/1.")) {
            fail(400, "invalid_request");
            return;
        }
        m_req.method = first[0].toUpper();
        const QUrl url(QString::fromLatin1(first[1]));
        m_req.path = url.path();
        m_req.query = QUrlQuery(url.query());
        for (int i = 1; i < lines.size(); ++i) {
            QByteArray l = lines[i];
            if (i + 1 < lines.size()) {
                if (!l.endsWith('\r')) {
                    fail(400, "invalid_request");
                    return;
                }
                l.chop(1);
            }
            const int colon = l.indexOf(':');
            const QByteArray name = l.left(colon).toLower();
            if (colon <= 0 || !isHeaderName(name)) {
                fail(400, "invalid_request");
                return;
            }
            const QByteArray rawValue = l.mid(colon + 1);
            for (char c : rawValue) {
                if ((uchar(c) < 0x20 && c != '\t') || uchar(c) == 0x7f) {
                    fail(400, "invalid_request");
                    return;
                }
            }
            // Chunked не поддерживается; неоднозначное обрамление не передаём обработчику.
            if (name == "transfer-encoding") {
                fail(400, "unsupported_transfer_encoding");
                return;
            }
            if (name == "content-length" && m_req.headers.contains(name)) {
                fail(400, "invalid_request");
                return;
            }
            m_req.headers.insert(name, rawValue.trimmed());
        }
        const QByteArray length = m_req.headers.value("content-length", "0");
        bool ok = !length.isEmpty();
        for (char c : length)
            if (c < '0' || c > '9') ok = false;
        if (ok) m_contentLength = length.toLongLong(&ok);
        if (!ok) {
            fail(400, "invalid_request");
            return;
        }
        if (m_contentLength > kMaxBody) {
            fail(413, "request_too_large");
            return;
        }
        m_headersDone = true;
    }
    if (m_buf.size() < m_contentLength)
        return;
    m_req.body = m_buf.left(int(m_contentLength));
    m_buf.clear();
    m_dispatched = true;
    m_idle.stop();
    emit request(m_req, this);
}

void HttpConnection::respond(const HttpResponse &r)
{
    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + ' ' + HttpServer::reasonPhrase(r.status) + "\r\n";
    if (r.status != 204) {
        out += "Content-Type: " + r.contentType + "\r\n";
        out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
    }
    out += "Cache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n";
    for (const auto &h : r.headers)
        out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    if (r.status != 204 && m_req.method != "HEAD")
        out += r.body;
    m_dispatched = true;
    m_sock->write(out);
    closeLater();
}

void HttpConnection::startEventStream()
{
    if (m_stream) return;
    m_dispatched = true;
    m_stream = true;
    m_idle.stop();
    writeStream("HTTP/1.1 200 OK\r\nContent-Type: text/event-stream; charset=utf-8\r\n"
                "Cache-Control: no-store\r\nX-Accel-Buffering: no\r\nConnection: keep-alive\r\n\r\n");
}

void HttpConnection::writeStream(const QByteArray &data)
{
    if (!m_stream) return;
    if (m_sock->state() != QAbstractSocket::ConnectedState) {
        m_stream = false;
        return;
    }
    qint64 pending = m_sock->bytesToWrite();
    // У TLS уже зашифрованные данные находятся в отдельной очереди сокета.
    if (auto *ssl = qobject_cast<QSslSocket *>(m_sock))
        pending += ssl->encryptedBytesToWrite();
    if (data.size() > m_maxStreamBytes || pending > m_maxStreamBytes - data.size()
        || m_sock->write(data) != data.size()) {
        // disconnectFromHost() ждал бы опустошения очереди медленного клиента.
        m_stream = false;
        m_sock->abort();
    }
}

void HttpConnection::sendEvent(const QByteArray &event, const QByteArray &data)
{
    if (!m_stream) return;
    QByteArray out = "event: " + event + "\n";
    for (const QByteArray &line : data.split('\n'))
        out += "data: " + line + "\n";
    out += "\n";
    writeStream(out);
}

void HttpConnection::sendComment(const QByteArray &text)
{
    if (m_stream) writeStream(": " + text + "\n\n");
}

HttpServer::HttpServer(QObject *parent)
    : QTcpServer(parent)
{
    qRegisterMetaType<zr::HttpRequest>("zr::HttpRequest");
    m_heartbeat.setInterval(15000);
    connect(&m_heartbeat, &QTimer::timeout, this, [this]() {
        for (const auto &c : m_conns)
            if (c && c->isStream()) c->sendComment("ping");
    });
    m_heartbeat.start();
}

void HttpServer::setTls(const QSslCertificate &cert, const QSslKey &key)
{
    m_cert = cert;
    m_key = key;
}

int HttpServer::streamCount() const
{
    int n = 0;
    for (const auto &c : m_conns)
        if (c && c->isStream()) ++n;
    return n;
}

void HttpServer::broadcast(const QByteArray &event, const QByteArray &data)
{
    for (const auto &c : m_conns)
        if (c && c->isStream()) c->sendEvent(event, data);
}

void HttpServer::incomingConnection(qintptr fd)
{
    m_conns.removeAll(QPointer<HttpConnection>());
    QTcpSocket *sock = nullptr;
    if (tls()) {
        auto *ssl = new QSslSocket(this);
        if (!ssl->setSocketDescriptor(fd)) { delete ssl; return; }
        ssl->setLocalCertificate(m_cert);
        ssl->setPrivateKey(m_key);
        ssl->setProtocol(QSsl::TlsV1_2OrLater);
        ssl->startServerEncryption();
        sock = ssl;
    } else {
        sock = new QTcpSocket(this);
        if (!sock->setSocketDescriptor(fd)) { delete sock; return; }
    }
    if (m_conns.size() >= kMaxConnections) {
        sock->write("HTTP/1.1 503 Service Unavailable\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        sock->disconnectFromHost();
        connect(sock, &QTcpSocket::disconnected, sock, &QObject::deleteLater);
        return;
    }
    auto *conn = new HttpConnection(sock, this);
    m_conns.append(conn);
    connect(conn, &HttpConnection::request, this, [this](const HttpRequest &req, HttpConnection *c) {
        if (m_handler) m_handler(req, c);
    });
}

} // namespace zr
