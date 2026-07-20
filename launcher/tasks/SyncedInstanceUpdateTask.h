#pragma once

#include "tasks/Task.h"
#include "BaseInstance.h"
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include "net/NetJob.h"

class SyncedInstanceUpdateTask : public Task {
    Q_OBJECT
public:
    explicit SyncedInstanceUpdateTask(BaseInstance* instance);
    virtual ~SyncedInstanceUpdateTask() = default;

    bool abort() override;

protected:
    void executeTask() override;

private slots:
    void manifestFetched();
    void downloadSucceeded();
    void downloadFailed(QString reason);
    void downloadProgress(qint64 current, qint64 total);

private:
    BaseInstance* m_instance;
    QNetworkReply* m_manifestReply = nullptr;
    NetJob::Ptr m_downloadJob;
    QString m_targetVersion;

    void deleteOrphanedFiles(const QStringList& filesToKeep);
};
