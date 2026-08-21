#pragma once

#include "tasks/Task.h"
#include "BaseInstance.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QMap>
#include "net/NetJob.h"

class SyncedInstanceUpdateTask : public Task {
    Q_OBJECT
public:
    explicit SyncedInstanceUpdateTask(BaseInstance* instance);
    virtual ~SyncedInstanceUpdateTask() = default;

    bool abort() override;

    void setForceRepair(bool force) { m_forceRepair = force; }
    bool isForceRepair() const { return m_forceRepair; }

    QString getOrComputeHash(const QString& relPath, const QString& absPath);

protected:
    void executeTask() override;

private slots:
    void manifestFetched();
    void downloadSucceeded();
    void downloadFailed(QString reason);
    void downloadProgress(qint64 current, qint64 total);

private:
    struct CachedFileInfo {
        qint64 size = 0;
        qint64 mtime = 0;
        QString hash;
    };

    struct FileDownloadItem {
        QString relPath;
        QString hash;
        qint64 size = 0;
    };

    BaseInstance* m_instance;
    QNetworkReply* m_manifestReply = nullptr;
    NetJob::Ptr m_downloadJob;
    QString m_targetVersion;
    bool m_forceRepair = false;

    QMap<QString, CachedFileInfo> m_hashCache;
    bool m_cacheDirty = false;
    QList<FileDownloadItem> m_downloadedFiles;

    qint64 m_totalDownloadBytes = 0;
    qint64 m_completedDownloadBytes = 0;

    bool m_hasVoxyZipDownload = false;
    QString m_voxyZipPath;
    QString m_voxyExtractDir;

    void loadHashCache();
    void saveHashCache();
};

