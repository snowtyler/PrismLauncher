#include <QTest>
#include <QMap>
#include "tasks/SyncPlan.h"

struct FakeFileEntry {
    qint64 size = 0;
    QString hash;
};

class FakeSyncFileSource : public ISyncFileSource {
   public:
    QMap<QString, FakeFileEntry> files;
    QSet<QString> dirsWithContent;
    mutable int hashCallCount = 0;

    LocalFileState stat(const QString& relPath) const override
    {
        if (files.contains(relPath)) {
            return {true, files[relPath].size};
        }
        return {false, 0};
    }

    QString hash(const QString& relPath) const override
    {
        hashCallCount++;
        if (files.contains(relPath)) {
            return files[relPath].hash;
        }
        return {};
    }

    QStringList listFiles() const override { return files.keys(); }

    bool dirHasContent(const QString& relPath) const override { return dirsWithContent.contains(relPath); }
};

static SyncManifestFile makeFile(const QString& path, const QString& hash, qint64 size = 100)
{
    return {path, hash, size, true};
}

static SyncManifest baseManifest()
{
    SyncManifest m;
    m.version = "1.0.0";
    m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));
    m.files.append(makeFile("minecraft/config/foo.cfg", "bbb", 200));
    return m;
}

class SyncPlanTest : public QObject {
    Q_OBJECT

   private slots:
    // --- Group A: orphan cleanup ---
    void A1_optionsTxtNotDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/options.txt"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.toDelete.contains("minecraft/options.txt"));
    }

    void A2_serversDatNotDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/servers.dat"] = {50, "xxx"};
        fs.files["minecraft/servers.dat_old"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.toDelete.contains("minecraft/servers.dat"));
        QVERIFY(!plan.toDelete.contains("minecraft/servers.dat_old"));
    }

    void A3_optionsShadersNotDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/optionsshaders.txt"] = {50, "xxx"};
        fs.files["minecraft/optionsof.txt"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.toDelete.contains("minecraft/optionsshaders.txt"));
        QVERIFY(!plan.toDelete.contains("minecraft/optionsof.txt"));
    }

    void A4_instanceCfgNotDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["instance.cfg"] = {50, "xxx"};
        fs.files[".synced_cache.json"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.toDelete.contains("instance.cfg"));
        QVERIFY(!plan.toDelete.contains(".synced_cache.json"));
    }

    void A5_protectedDirsNotDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/saves/world/level.dat"] = {50, "xxx"};
        fs.files["minecraft/screenshots/shot.png"] = {50, "xxx"};
        fs.files["minecraft/logs/latest.log"] = {50, "xxx"};
        fs.files["minecraft/crash-reports/crash.txt"] = {50, "xxx"};
        fs.files["minecraft/backups/backup.zip"] = {50, "xxx"};
        fs.files["minecraft/local/data.dat"] = {50, "xxx"};
        fs.files[".tmp/temp.dat"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        for (const auto& del : plan.toDelete) {
            QVERIFY2(!del.startsWith("minecraft/saves/", Qt::CaseInsensitive), qPrintable(del));
            QVERIFY2(!del.startsWith("minecraft/screenshots/", Qt::CaseInsensitive), qPrintable(del));
            QVERIFY2(!del.startsWith("minecraft/logs/", Qt::CaseInsensitive), qPrintable(del));
            QVERIFY2(!del.startsWith("minecraft/crash-reports/", Qt::CaseInsensitive), qPrintable(del));
            QVERIFY2(!del.startsWith("minecraft/backups/", Qt::CaseInsensitive), qPrintable(del));
            QVERIFY2(!del.startsWith("minecraft/local/", Qt::CaseInsensitive), qPrintable(del));
            QVERIFY2(!del.startsWith(".tmp/", Qt::CaseInsensitive), qPrintable(del));
        }
    }

    void A6_unmanagedDirNotDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/journeymap/data.dat"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.toDelete.contains("minecraft/journeymap/data.dat"));
    }

    void A7_orphanModDeleted()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/mods/orphan.jar"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(plan.toDelete.contains("minecraft/mods/orphan.jar"));
    }

    void A8_orphansInManagedDirs()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/config/orphan.cfg"] = {50, "xxx"};
        fs.files["minecraft/resourcepacks/old.zip"] = {50, "xxx"};
        fs.files["minecraft/shaderpacks/old.zip"] = {50, "xxx"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(plan.toDelete.contains("minecraft/config/orphan.cfg"));
        QVERIFY(plan.toDelete.contains("minecraft/resourcepacks/old.zip"));
        QVERIFY(plan.toDelete.contains("minecraft/shaderpacks/old.zip"));
    }

    void A9_caseInsensitiveKeepSet()
    {
        SyncManifest m;
        m.version = "1.0.0";
        m.files.append(makeFile("minecraft/mods/Foo.jar", "aaa", 100));

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/foo.jar"] = {100, "aaa"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.toDelete.contains("minecraft/mods/foo.jar"));
    }

    // --- Group B: download decisions ---
    void B1_missingFileDownloaded()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "", false, fs);
        bool found = false;
        for (const auto& f : plan.toDownload)
            if (f.path == "minecraft/mods/ModA.jar")
                found = true;
        QVERIFY(found);
    }

    void B2_sizeDiffersDownloaded()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {999, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "", false, fs);
        bool found = false;
        for (const auto& f : plan.toDownload)
            if (f.path == "minecraft/mods/ModA.jar")
                found = true;
        QVERIFY(found);
    }

    void B3_hashDiffersDownloaded()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "zzz"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "", false, fs);
        bool found = false;
        for (const auto& f : plan.toDownload)
            if (f.path == "minecraft/mods/ModA.jar")
                found = true;
        QVERIFY(found);
    }

    void B4_sizeAndHashMatchNotDownloaded()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(plan.toDownload.isEmpty());
    }

    void B5_backslashNormalized()
    {
        SyncManifest m;
        m.version = "1.0.0";
        m.files.append(makeFile("minecraft\\mods\\ModA.jar", "aaa", 100));

        FakeSyncFileSource fs;
        fs.files["minecraft\\mods\\ModA.jar"] = {100, "aaa"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(plan.toDownload.isEmpty());
    }

    void B6_optionsTxtNotDownloadedWithoutForce()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.files.append(makeFile("minecraft/options.txt", "newHash", 100));
        m.forceConfigOverwrite = false;

        FakeSyncFileSource fs;
        fs.files["minecraft/options.txt"] = {100, "oldHash"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.toDownload.isEmpty());
    }

    void B7_optionsTxtDownloadedWithForceAndNewVersion()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.files.append(makeFile("minecraft/options.txt", "newHash", 100));
        m.forceConfigOverwrite = true;

        FakeSyncFileSource fs;
        fs.files["minecraft/options.txt"] = {100, "oldHash"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QCOMPARE(plan.toDownload.size(), 1);
        QCOMPARE(plan.toDownload[0].path, QString("minecraft/options.txt"));
    }

    void B8_optionsTxtNotDownloadedForceButSameVersion()
    {
        SyncManifest m;
        m.version = "1.0.0";
        m.files.append(makeFile("minecraft/options.txt", "newHash", 100));
        m.forceConfigOverwrite = true;

        FakeSyncFileSource fs;
        fs.files["minecraft/options.txt"] = {100, "oldHash"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.toDownload.isEmpty());
    }

    void B9_optionsTxtDownloadedWithForceRepair()
    {
        SyncManifest m;
        m.version = "1.0.0";
        m.files.append(makeFile("minecraft/options.txt", "newHash", 100));
        m.forceConfigOverwrite = true;

        FakeSyncFileSource fs;
        fs.files["minecraft/options.txt"] = {100, "oldHash"};

        auto plan = computeSyncPlan(m, "1.0.0", true, fs);
        QCOMPARE(plan.toDownload.size(), 1);
    }

    // --- Group C: fast path ---
    void C1_fastPathUpToDate()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.upToDate);
        QVERIFY(plan.toDownload.isEmpty());
        QVERIFY(plan.toDelete.isEmpty());
    }

    void C2_fastPathFileMissing()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(!plan.upToDate);
    }

    void C3_fastPathWrongSize()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {999, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(!plan.upToDate);
    }

    void C4_fastPathBypassedByForceRepair()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "1.0.0", true, fs);
        QVERIFY(!plan.upToDate);
    }

    void C5_fastPathEmptyCurrentVersion()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};

        auto plan = computeSyncPlan(m, "", false, fs);
        QVERIFY(!plan.upToDate);
    }

    void C6_fastPathSkipsOrphanCleanup()
    {
        auto m = baseManifest();
        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/config/foo.cfg"] = {200, "bbb"};
        fs.files["minecraft/mods/orphan.jar"] = {50, "xxx"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.upToDate);
        QVERIFY(plan.toDelete.isEmpty());
    }

    // --- Group D: Voxy ---
    void D1_voxyZipSupersedes()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.hasVoxyCacheZip = true;
        m.voxyZipFile = "voxy_cache.zip";
        m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "ccc"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        for (const auto& f : plan.toDownload)
            QVERIFY2(!f.path.contains(".voxy"), qPrintable(f.path));
    }

    void D2_voxySeededNotRedownloaded()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "old"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        for (const auto& f : plan.toDownload)
            QVERIFY2(!f.path.contains(".voxy"), qPrintable(f.path));
    }

    void D3_voxyNotSeededForcesRedownload()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QCOMPARE(plan.toDownload.size(), 1);
    }

    void D4_voxyResetVersionTriggered()
    {
        SyncManifest m;
        m.version = "12.0.5";
        m.voxyResetVersion = "12.0.5";
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "ccc"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "12.0.4", false, fs);
        QCOMPARE(plan.toDownload.size(), 1);
    }

    void D5_voxyResetVersionNotTriggered()
    {
        SyncManifest m;
        m.version = "12.0.7";
        m.voxyResetVersion = "12.0.5";
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "ccc"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "12.0.6", false, fs);
        for (const auto& f : plan.toDownload)
            QVERIFY2(!f.path.contains(".voxy"), qPrintable(f.path));
    }

    void D6_forceVoxyNoResetVersion()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.forceVoxyRedownload = true;
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "ccc"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QCOMPARE(plan.toDownload.size(), 1);
    }

    void D7_forceVoxyNotTriggeredSameVersion()
    {
        SyncManifest m;
        m.version = "1.0.0";
        m.forceVoxyRedownload = true;
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "ccc"};
        fs.dirsWithContent.insert("minecraft/.voxy/saves/cozycreations.modpack.gg");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        for (const auto& f : plan.toDownload)
            QVERIFY2(!f.path.contains(".voxy"), qPrintable(f.path));
    }

    void D8_forceVoxyWithZip()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.hasVoxyCacheZip = true;
        m.voxyZipFile = "voxy_cache.zip";
        m.voxyZipHash = "ziphash";
        m.voxyZipSize = 9999;
        m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.dirsWithContent.insert("minecraft");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(!plan.upToDate);
    }

    void D9_forceVoxyLegacyDirsDeleted()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.hasVoxyCacheZip = false;
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.voxyDirsToDelete.contains("minecraft/.voxy/saves/cozycreations.modpack.gg"));
        QVERIFY(plan.voxyDirsToDelete.contains(".voxy/saves/cozycreations.modpack.gg"));
    }

    void D10_voxyExtractDirMinecraft()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.hasVoxyCacheZip = true;
        m.voxyZipFile = "voxy_cache.zip";
        m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        fs.dirsWithContent.insert("minecraft");

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.downloadVoxyZip);
        QCOMPARE(plan.voxyZipRelPath, QString(".tmp/voxy_cache.zip"));
        QCOMPARE(plan.voxyExtractDir, QString("minecraft/.voxy/saves/cozycreations.modpack.gg"));

        // Without minecraft dir
        FakeSyncFileSource fs2;
        fs2.files["minecraft/mods/ModA.jar"] = {100, "aaa"};
        auto plan2 = computeSyncPlan(m, "1.0.0", false, fs2);
        QCOMPARE(plan2.voxyExtractDir, QString(".voxy/saves/cozycreations.modpack.gg"));
    }

    // --- Group E: hash cache avoidance ---
    void E1_sizeMismatchSkipsHash()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {999, "aaa"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QCOMPARE(fs.hashCallCount, 0);
    }

    void E2_missingFileSkipsHash()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.files.append(makeFile("minecraft/mods/ModA.jar", "aaa", 100));

        FakeSyncFileSource fs;

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QCOMPARE(fs.hashCallCount, 0);
    }

    void E3_voxySkippedNoHash()
    {
        SyncManifest m;
        m.version = "2.0.0";
        m.hasVoxyCacheZip = true;
        m.files.append(makeFile("minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst", "ccc", 300));

        FakeSyncFileSource fs;
        fs.files["minecraft/.voxy/saves/cozycreations.modpack.gg/data.sst"] = {300, "old"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QCOMPARE(fs.hashCallCount, 0);
    }

    // --- Group F: empty manifest guard ---
    void F1_emptyManifestAborts()
    {
        SyncManifest m;
        m.version = "1.0.0";

        FakeSyncFileSource fs;
        fs.files["minecraft/mods/ModA.jar"] = {100, "aaa"};

        auto plan = computeSyncPlan(m, "1.0.0", false, fs);
        QVERIFY(plan.aborted);
        QVERIFY(plan.toDelete.isEmpty());
    }
};

QTEST_GUILESS_MAIN(SyncPlanTest)

#include "SyncPlan_test.moc"
