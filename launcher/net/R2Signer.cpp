#include "R2Signer.h"
#include <QMessageAuthenticationCode>
#include <QCryptographicHash>
#include <QDateTime>
#include <QStringList>
#include <QUrlQuery>

namespace SigV4 {

static QByteArray hmacSha256(const QByteArray& key, const QByteArray& data) {
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

SignedRequest sign(
    const QString& method,
    const QUrl& url,
    const QByteArray& payload,
    const QString& accessKey,
    const QString& secretKey,
    const QString& region,
    const QString& service,
    bool isPrecomputedHash
) {
    QDateTime now = QDateTime::currentDateTimeUtc();
    QByteArray dateStr = now.toString("yyyyMMdd").toUtf8();
    QByteArray amzDate = now.toString("yyyyMMddTHHmmssZ").toUtf8();

    QByteArray host = url.host().toUtf8();
    if (url.port() != -1 && url.port() != 80 && url.port() != 443) {
        host += ":" + QByteArray::number(url.port());
    }

    // Canonical URI percent encodes all segments
    QStringList segments = url.path().split('/', Qt::SkipEmptyParts);
    QByteArray canonicalUri;
    for (const QString& seg : segments) {
        canonicalUri += "/" + QUrl::toPercentEncoding(seg);
    }
    if (url.path().endsWith('/')) {
        canonicalUri += "/";
    }
    if (canonicalUri.isEmpty()) {
        canonicalUri = "/";
    }

    QByteArray payloadHash;
    if (isPrecomputedHash) {
        payloadHash = payload;
    } else {
        payloadHash = QCryptographicHash::hash(payload, QCryptographicHash::Sha256).toHex().toLower();
    }

    QMap<QByteArray, QByteArray> canonicalHeaders;
    canonicalHeaders["host"] = host;
    canonicalHeaders["x-amz-content-sha256"] = payloadHash;
    canonicalHeaders["x-amz-date"] = amzDate;

    QByteArray canonicalHeadersStr;
    QByteArray signedHeadersStr;
    for (auto it = canonicalHeaders.begin(); it != canonicalHeaders.end(); ++it) {
        canonicalHeadersStr += it.key() + ":" + it.value() + "\n";
        if (!signedHeadersStr.isEmpty()) signedHeadersStr += ";";
        signedHeadersStr += it.key();
    }

    QByteArray canonicalRequest = method.toUpper().toUtf8() + "\n" +
                                 canonicalUri + "\n" +
                                 QByteArray() + "\n" + // query string is empty
                                 canonicalHeadersStr + "\n" +
                                 signedHeadersStr + "\n" +
                                 payloadHash;

    QByteArray hashedCanonicalRequest = QCryptographicHash::hash(canonicalRequest, QCryptographicHash::Sha256).toHex().toLower();

    QByteArray credentialScope = dateStr + "/" + region.toUtf8() + "/" + service.toUtf8() + "/aws4_request";
    QByteArray stringToSign = QByteArray("AWS4-HMAC-SHA256\n") +
                              amzDate + "\n" +
                              credentialScope + "\n" +
                              hashedCanonicalRequest;

    QByteArray kDate = hmacSha256("AWS4" + secretKey.toUtf8(), dateStr);
    QByteArray kRegion = hmacSha256(kDate, region.toUtf8());
    QByteArray kService = hmacSha256(kRegion, service.toUtf8());
    QByteArray kSigning = hmacSha256(kService, "aws4_request");

    QByteArray signature = hmacSha256(kSigning, stringToSign).toHex().toLower();

    QByteArray authorization = QByteArray("AWS4-HMAC-SHA256 Credential=") + accessKey.toUtf8() + "/" + credentialScope +
                              ", SignedHeaders=" + signedHeadersStr +
                              ", Signature=" + signature;

    SignedRequest res;
    res.headers["Authorization"] = authorization;
    res.headers["x-amz-date"] = amzDate;
    res.headers["x-amz-content-sha256"] = payloadHash;
    res.headers["Host"] = host;

    return res;
}

} // namespace SigV4
