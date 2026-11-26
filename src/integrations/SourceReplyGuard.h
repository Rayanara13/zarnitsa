// Общие границы ответа источника: абсолютный срок и размер распакованного тела.
#pragma once

#include <QNetworkReply>
#include <QTimer>
#include <QVariant>

namespace zr {

inline void guardSourceReply(QNetworkReply *reply, int deadlineMs, qint64 maxBody = 16 * 1024 * 1024)
{
    reply->setReadBufferSize(maxBody + 1);
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    const auto reject = [reply](const QString &error) {
        if (reply->isFinished()) return;
        reply->setProperty("sourceGuardError", error);
        reply->abort();
    };
    QObject::connect(timer, &QTimer::timeout, reply, [reject]() {
        reject(QStringLiteral("Истёк полный срок запроса к источнику"));
    });
    const auto checkSize = [reply, maxBody, reject]() {
        if (reply->bytesAvailable() > maxBody
            || reply->header(QNetworkRequest::ContentLengthHeader).toLongLong() > maxBody)
            reject(QStringLiteral("Ответ источника превышает допустимый размер"));
    };
    QObject::connect(reply, &QNetworkReply::metaDataChanged, reply, checkSize);
    QObject::connect(reply, &QIODevice::readyRead, reply, checkSize);
    QObject::connect(reply, &QNetworkReply::finished, timer, &QTimer::stop);
    timer->start(deadlineMs);
}

inline int sourceHttpStatus(QNetworkReply *reply)
{
    // HTTP 401/404/500 — ответы сервера, но обрыв/таймаут после заголовков не считается ответом.
    const int error = int(reply->error());
    if (!reply->property("sourceGuardError").toString().isEmpty()
        || (error > 0 && error < 200) || (error >= 300 && error < 400))
        return 0;
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

inline QString sourceReplyError(QNetworkReply *reply)
{
    const QString guardError = reply->property("sourceGuardError").toString();
    return guardError.isEmpty() ? reply->errorString() : guardError;
}

} // namespace zr
