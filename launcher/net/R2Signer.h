#pragma once

#include <QMap>
#include <QByteArray>
#include <QString>
#include <QUrl>

namespace SigV4 {

struct SignedRequest {
    QMap<QByteArray, QByteArray> headers;
};

SignedRequest sign(
    const QString& method,
    const QUrl& url,
    const QByteArray& payload,
    const QString& accessKey,
    const QString& secretKey,
    const QString& region = "auto",
    const QString& service = "s3",
    bool isPrecomputedHash = false
);

} // namespace SigV4
