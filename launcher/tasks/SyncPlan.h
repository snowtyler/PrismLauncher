#pragma once

#include <QString>
#include <QStringList>
#include <QList>

struct SyncManifestFile {
    QString path;
    QString hash;
    qint64 size = 0;
    bool hasSize = false;
};

struct SyncManifest {
    QString version;
    QList<SyncManifestFile> files;
    bool forceConfigOverwrite = false;
    bool forceVoxyRedownload = false;
    QString voxyResetVersion;
    bool hasVoxyCacheZip = false;
    QString voxyZipFile;
    QString voxyZipHash;
    qint64 voxyZipSize = 0;
};

struct LocalFileState {
    bool exists = false;
    qint64 size = 0;
};

class ISyncFileSource {
   public:
    virtual ~ISyncFileSource() = default;
    virtual LocalFileState stat(const QString& relPath) const = 0;
    virtual QString hash(const QString& relPath) const = 0;
    virtual QStringList listFiles() const = 0;
    virtual bool dirHasContent(const QString& relPath) const = 0;
};

struct SyncPlan {
    bool upToDate = false;
    bool aborted = false;
    QString abortReason;
    QList<SyncManifestFile> toDownload;
    QStringList toDelete;
    QStringList voxyDirsToDelete;
    bool downloadVoxyZip = false;
    QString voxyZipRelPath;
    QString voxyExtractDir;
};

SyncPlan computeSyncPlan(const SyncManifest& manifest,
                         const QString& currentVersion,
                         bool forceRepair,
                         const ISyncFileSource& local);
