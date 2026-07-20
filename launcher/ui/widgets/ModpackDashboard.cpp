#include "ModpackDashboard.h"
#include "Application.h"
#include "InstanceList.h"
#include "settings/SettingsObject.h"
#include "minecraft/VanillaInstanceCreationTask.h"
#include "tasks/SyncedInstanceUpdateTask.h"
#include "tasks/SyncedInstanceUploadTask.h"
#include "ui/dialogs/ProgressDialog.h"
#include "ui/dialogs/UploadConfirmDialog.h"
#include "FileSystem.h"
#include "meta/Index.h"
#include "meta/VersionList.h"
#include "meta/Version.h"
#include <QFileSystemModel>
#include <QTreeView>
#include <QHeaderView>
#include "FastFileIconProvider.h"
#include "FileIgnoreProxy.h"
#include "MMCZip.h"

#include <QNetworkRequest>
#include <QJsonArray>
#include <QJsonDocument>
#include <QInputDialog>
#include <QMessageBox>
#include <QDialog>
#include <QDateTime>
#include <QFormLayout>
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QDir>
#include <QScrollBar>

// Dialog for creating a new synced pack
class CreatePackDialog : public QDialog {
public:
    QLineEdit* shortcodeEdit;
    QLineEdit* nameEdit;
    QLineEdit* mcVersionEdit;
    QComboBox* loaderCombo;
    QLineEdit* loaderVersionEdit;

    explicit CreatePackDialog(QWidget* parent = nullptr) : QDialog(parent) {
        setWindowTitle(tr("Create Synced Instance"));
        auto layout = new QFormLayout(this);

        shortcodeEdit = new QLineEdit(this);
        shortcodeEdit->setPlaceholderText("e.g. TOO3EX");
        layout->addRow(tr("Shortcode (A-Z, 0-9):"), shortcodeEdit);

        nameEdit = new QLineEdit(this);
        nameEdit->setPlaceholderText("e.g. Cozy Creations");
        layout->addRow(tr("Pack Name:"), nameEdit);

        mcVersionEdit = new QLineEdit("1.21.1", this);
        layout->addRow(tr("Minecraft Version:"), mcVersionEdit);

        loaderCombo = new QComboBox(this);
        loaderCombo->addItems({"Fabric", "Forge", "NeoForge", "Quilt", "Vanilla"});
        layout->addRow(tr("Mod Loader:"), loaderCombo);

        loaderVersionEdit = new QLineEdit(this);
        loaderVersionEdit->setPlaceholderText(tr("Leave empty for recommended"));
        layout->addRow(tr("Loader Version:"), loaderVersionEdit);

        auto buttonBox = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
        layout->addRow(buttonBox);

        connect(buttonBox, &QDialogButtonBox::accepted, this, &QDialog::accept);
        connect(buttonBox, &QDialogButtonBox::rejected, this, &QDialog::reject);
    }
};



ModpackDashboard::ModpackDashboard(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Apply dashboard-wide style sheets for a dark, custom look
    setStyleSheet(
        "QScrollArea {"
        "  background-color: #11111b;"
        "}"
        "QWidget#centralWidget {"
        "  background-color: #11111b;"
        "}"
    );

    m_centralWidget = new QWidget(this);
    m_centralWidget->setObjectName("centralWidget");
    setWidget(m_centralWidget);

    auto mainLayout = new QVBoxLayout(m_centralWidget);
    mainLayout->setContentsMargins(20, 10, 20, 20);
    mainLayout->setSpacing(15);

    // Header layout
    m_headerWidget = new QWidget(m_centralWidget);
    auto headerLayout = new QHBoxLayout(m_headerWidget);
    headerLayout->setContentsMargins(0, 0, 0, 0);

    auto brandingLabel = new QLabel(tr("Available Modpacks"), m_headerWidget);
    brandingLabel->setStyleSheet("color: #cdd6f4; font-size: 24px; font-weight: bold;");
    brandingLabel->setAlignment(Qt::AlignCenter);
    headerLayout->addWidget(brandingLabel);

    mainLayout->addWidget(m_headerWidget);

    // Grid layout for cards
    m_gridLayout = new QGridLayout();
    m_gridLayout->setSpacing(15);
    mainLayout->addLayout(m_gridLayout);

    // Loading indicator
    m_loadingLabel = new QLabel(tr("Checking for modpacks..."), m_centralWidget);
    m_loadingLabel->setStyleSheet("color: #a6adc8; font-size: 16px;");
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_loadingLabel, 1, Qt::AlignCenter);

    // Footer actions
    auto footerLayout = new QHBoxLayout();
    auto addPrivateLink = new QPushButton(tr("[+] Add Private Pack Code"), m_centralWidget);
    addPrivateLink->setStyleSheet("QPushButton { color: #f5c2e7; border: none; font-size: 14px; text-decoration: underline; background: transparent; } QPushButton:hover { color: #f38ba8; }");
    addPrivateLink->setCursor(Qt::PointingHandCursor);
    footerLayout->addWidget(addPrivateLink, 0, Qt::AlignLeft);
    connect(addPrivateLink, &QPushButton::clicked, this, &ModpackDashboard::onAddPrivatePackClicked);

    mainLayout->addLayout(footerLayout);

    fetchRegistry();

    // Periodically update card states if instances change
    auto updateCardStatuses = [this]() {
        for (auto* card : m_cards) {
            card->updateStatus();
        }
    };
    connect(APPLICATION->instances(), &InstanceList::dataChanged, this, updateCardStatuses);
    connect(APPLICATION->instances(), &InstanceList::instancesChanged, this, updateCardStatuses);
}

void ModpackDashboard::setAdminMode(bool enabled)
{
    m_adminMode = enabled;
    for (auto* card : m_cards) {
        card->setAdminMode(enabled);
    }
}

void ModpackDashboard::refreshDashboard()
{
    fetchRegistry();
}

void ModpackDashboard::fetchRegistry()
{
    m_loadingLabel->setVisible(true);
    m_loadingLabel->setText(tr("Downloading modpacks list..."));

    // Clear previous cards
    for (auto* card : m_cards) {
        m_gridLayout->removeWidget(card);
        card->deleteLater();
    }
    m_cards.clear();
    m_packs.clear();

    QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    QUrl url(publicUrl + "registry.json?t=" + QString::number(QDateTime::currentMSecsSinceEpoch()));
    m_registryReply = APPLICATION->network()->get(QNetworkRequest(url));
    connect(m_registryReply, &QNetworkReply::finished, this, &ModpackDashboard::registryFetched);
}

void ModpackDashboard::registryFetched()
{
    if (!m_registryReply) return;
    m_registryReply->deleteLater();
    auto reply = m_registryReply;
    m_registryReply = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray registryData = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(registryData);
        if (!doc.isNull() && doc.isObject()) {
            QJsonArray packsArray = doc.object()["packs"].toArray();
            for (int i = 0; i < packsArray.size(); ++i) {
                QJsonObject pack = packsArray[i].toObject();
                if (!pack["is_private"].toBool()) {
                    m_packs.append(pack);
                }
            }
        }
    }

    // Now, fetch any private packs that are stored locally
    QStringList privateCodes = APPLICATION->settings()->get("PrivatePacks").toStringList();
    if (privateCodes.isEmpty()) {
        renderCards();
        return;
    }

    QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
    if (!publicUrl.endsWith('/')) {
        publicUrl += '/';
    }

    m_privateReplies.clear();
    for (const QString& code : privateCodes) {
        // Fetch manifest/details for private packs directly
        QUrl url(publicUrl + "shortcodes/" + code + ".json");
        auto* privateReply = APPLICATION->network()->get(QNetworkRequest(url));
        m_privateReplies.append(privateReply);
        connect(privateReply, &QNetworkReply::finished, this, &ModpackDashboard::privatePackManifestFetched);
    }
}

void ModpackDashboard::privatePackManifestFetched()
{
    auto* reply = qobject_cast<QNetworkReply*>(sender());
    if (!reply) return;

    m_privateReplies.removeOne(reply);
    reply->deleteLater();

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray manifestData = reply->readAll();
        QJsonDocument doc = QJsonDocument::fromJson(manifestData);
        if (!doc.isNull() && doc.isObject()) {
            QJsonObject manifestObj = doc.object();
            // Wrap manifest details into a pack item format
            QJsonObject pack;
            pack["shortcode"] = manifestObj["shortcode"].toString();
            pack["name"] = manifestObj["name"].toString();
            pack["version"] = manifestObj["version"].toString();
            pack["description"] = manifestObj["description"].toString();
            if (pack["description"].toString().isEmpty()) {
                pack["description"] = tr("Private synced modpack.");
            }

            QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
            if (!publicUrl.endsWith('/')) {
                publicUrl += '/';
            }
            pack["banner_url"] = publicUrl + "assets/" + pack["shortcode"].toString() + "-banner.png";
            pack["icon_url"] = publicUrl + "assets/" + pack["shortcode"].toString() + "-icon.png";
            pack["is_private"] = true;

            // Prevent duplicates
            bool exists = false;
            for (const auto& existing : m_packs) {
                if (existing["shortcode"].toString() == pack["shortcode"].toString()) {
                    exists = true;
                    break;
                }
            }
            if (!exists) {
                m_packs.append(pack);
            }
        }
    }

    if (m_privateReplies.isEmpty()) {
        renderCards();
    }
}

void ModpackDashboard::renderCards()
{
    // Dynamically append any locally configured synced instances not already in m_packs
    for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
        BaseInstance* inst = APPLICATION->instances()->at(i);
        if (inst->settings()->get("IsSyncedInstance").toBool()) {
            QString shortcode = inst->settings()->get("SyncShortcode").toString();
            if (shortcode.isEmpty()) continue;

            bool exists = false;
            for (const auto& existing : m_packs) {
                if (existing["shortcode"].toString() == shortcode) {
                    exists = true;
                    break;
                }
            }

            if (!exists) {
                QJsonObject pack;
                pack["shortcode"] = shortcode;
                pack["name"] = inst->name();
                pack["version"] = inst->settings()->get("SyncVersion").toString();
                if (pack["version"].toString().isEmpty()) {
                    pack["version"] = "1.0.0";
                }
                pack["description"] = inst->settings()->get("ExportSummary").toString();
                if (pack["description"].toString().isEmpty()) {
                    pack["description"] = tr("Local synced instance (not yet uploaded to R2).");
                }
                
                QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
                if (!publicUrl.endsWith('/')) {
                    publicUrl += '/';
                }
                pack["banner_url"] = publicUrl + "assets/" + shortcode + "-banner.png";
                pack["icon_url"] = publicUrl + "assets/" + shortcode + "-icon.png";
                pack["is_private"] = inst->settings()->get("SyncIsPrivate").toBool();

                m_packs.append(pack);
            }
        }
    }

    m_loadingLabel->setVisible(false);

    if (m_packs.isEmpty()) {
        m_loadingLabel->setVisible(true);
        m_loadingLabel->setText(tr("No modpacks found. Enter a private pack code below!"));
        return;
    }

    int columns = 3;
    for (int i = 0; i < m_packs.size(); ++i) {
        auto* card = new ModpackCard(m_packs[i], m_adminMode, m_centralWidget);
        m_gridLayout->addWidget(card, i / columns, i % columns);
        m_cards.append(card);

        connect(card, &ModpackCard::actionTriggered, this, &ModpackDashboard::onCardActionTriggered);
        connect(card, &ModpackCard::settingsTriggered, this, &ModpackDashboard::onCardSettingsTriggered);
    }
}

void ModpackDashboard::onCardActionTriggered(const QString& action, const QString& shortcode)
{
    BaseInstance* inst = nullptr;
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i]->localInstance() && m_packs[i]["shortcode"].toString() == shortcode) {
            inst = m_cards[i]->localInstance();
            break;
        }
    }

    if (action == "play") {
        if (inst) {
            emit launchInstance(inst->id());
        }
    } else if (action == "delete") {
        if (inst) {
            if (inst->isRunning()) {
                QMessageBox::warning(this, tr("Cannot Delete Running Instance"),
                                     tr("The modpack is currently running and cannot be deleted. Please close the modpack before attempting to delete it."));
                return;
            }

            auto response = QMessageBox::question(this, tr("Confirm Deletion"),
                                                 tr("Are you sure you want to delete the modpack \"%1\"?\n"
                                                    "This will delete the instance and all of its local data.").arg(inst->name()),
                                                 QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
            if (response == QMessageBox::Yes) {
                if (!APPLICATION->instances()->trashInstance(inst->id())) {
                    APPLICATION->instances()->deleteInstance(inst->id());
                }
            }
        }
    } else if (action == "install") {
        // Fetch the manifest to read minecraft version & loader details
        QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
        if (!publicUrl.endsWith('/')) {
            publicUrl += '/';
        }

        m_loadingLabel->setVisible(true);
        m_loadingLabel->setText(tr("Fetching manifest for installation..."));

        QUrl url(publicUrl + "shortcodes/" + shortcode + ".json");
        auto* manifestReply = APPLICATION->network()->get(QNetworkRequest(url));
        connect(manifestReply, &QNetworkReply::finished, this, [this, shortcode, manifestReply]() {
            manifestReply->deleteLater();
            m_loadingLabel->setVisible(false);

            if (manifestReply->error() != QNetworkReply::NoError) {
                QMessageBox::critical(this, tr("Error"), tr("Failed to fetch installation details: %1").arg(manifestReply->errorString()));
                return;
            }

            QByteArray data = manifestReply->readAll();
            QJsonDocument doc = QJsonDocument::fromJson(data);
            if (doc.isNull() || !doc.isObject()) {
                QMessageBox::critical(this, tr("Error"), tr("Failed to parse installation details."));
                return;
            }

            runInstall(shortcode, doc.object());
        });
    }
}

void ModpackDashboard::runInstall(const QString& shortcode, const QJsonObject& manifest)
{
    QString name = manifest["name"].toString();
    QString mcVersion = manifest["game_version"].toString();
    QString loaderType = manifest["loader"].toString();
    QString loaderVerStr = manifest["loader_version"].toString();

    // Check version metadata
    auto mcList = APPLICATION->metadataIndex()->get("net.minecraft");
    mcList->waitToLoad();
    auto mcVer = mcList->getVersion(mcVersion);
    if (!mcVer) {
        QMessageBox::critical(this, tr("Install Error"), tr("Minecraft version %1 is not supported or not loaded.").arg(mcVersion));
        return;
    }

    BaseVersion::Ptr loaderVer = nullptr;
    QString loaderUid = "";

    if (loaderType != "Vanilla") {
        if (loaderType == "Fabric") {
            loaderUid = "net.fabricmc.fabric-loader";
        } else if (loaderType == "Forge") {
            loaderUid = "net.minecraftforge";
        } else if (loaderType == "NeoForge") {
            loaderUid = "org.neoforged.neoforge";
        } else if (loaderType == "Quilt") {
            loaderUid = "org.quiltmc.quilt-loader";
        }

        auto loaderList = APPLICATION->metadataIndex()->get(loaderUid);
        loaderList->waitToLoad();

        if (loaderVerStr.isEmpty()) {
            loaderVer = loaderList->getRecommended();
        } else {
            loaderVer = loaderList->getVersion(loaderVerStr);
        }

        if (!loaderVer) {
            QMessageBox::critical(this, tr("Install Error"), tr("Loader version %1 is not available.").arg(loaderVerStr));
            return;
        }
    }

    // Run creation task
    InstanceTask* rawTask = nullptr;
    if (loaderType == "Vanilla") {
        rawTask = new VanillaCreationTask(mcVer);
    } else {
        rawTask = new VanillaCreationTask(mcVer, loaderUid, loaderVer);
    }

    rawTask->setName(name);
    rawTask->setIcon("default");

    // Determine target directory for synced instances
    QDir instDirObj(APPLICATION->settings()->get("InstanceDir").toString());
    instDirObj.cdUp();
    QString syncedInstDir = instDirObj.absoluteFilePath("synced_instances");
    QDir().mkpath(syncedInstDir);

    unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(rawTask, syncedInstDir));
    ProgressDialog loadDialog(this);
    loadDialog.execWithTask(task.get());

    if (!task->wasSuccessful()) {
        return;
    }

    // Find the newly created instance
    BaseInstance* inst = nullptr;
    for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
        BaseInstance* candidate = APPLICATION->instances()->at(i);
        QDir cDir(candidate->instanceRoot());
        QDir sDir(syncedInstDir);
        if (cDir.absolutePath().startsWith(sDir.absolutePath()) && candidate->name() == name) {
            inst = candidate;
            break;
        }
    }
    if (!inst) {
        QStringList debugList;
        for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
            BaseInstance* candidate = APPLICATION->instances()->at(i);
            debugList.append(QString("- %1 (%2)").arg(candidate->name(), candidate->instanceRoot()));
        }
        QMessageBox::critical(this, tr("Install Error"),
                              tr("Could not locate the installed instance.\n\nSearching for: '%1'\nSynced dir: '%2'\n\nLoaded instances:\n%3")
                              .arg(name, syncedInstDir, debugList.join("\n")));
        return;
    }

    // Mark as Synced Instance
    inst->settings()->set("IsSyncedInstance", true);
    inst->settings()->set("SyncShortcode", shortcode);
    inst->settings()->set("SyncVersion", ""); // Force updates immediately
    inst->settings()->set("ExportVersion", manifest["version"].toString());
    inst->settings()->set("IntendedVersion", mcVersion);
    inst->saveNow();

    // Trigger update immediately to download mods/configs
    auto updateTask = makeShared<SyncedInstanceUpdateTask>(inst);
    ProgressDialog updateDialog(this);
    updateDialog.execWithTask(updateTask.get());

    // Refresh cards status
    for (auto* card : m_cards) {
        card->updateStatus();
    }
}


void ModpackDashboard::onCardSettingsTriggered(const QString& shortcode)
{
    BaseInstance* inst = nullptr;
    for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
        BaseInstance* candidate = APPLICATION->instances()->at(i);
        if (candidate->settings()->get("IsSyncedInstance").toBool() &&
            candidate->settings()->get("SyncShortcode").toString() == shortcode) {
            inst = candidate;
            break;
        }
    }
    if (!inst) {
        for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
            BaseInstance* candidate = APPLICATION->instances()->at(i);
            if (!candidate->settings()->get("IsSyncedInstance").toBool() &&
                candidate->settings()->get("SyncShortcode").toString() == shortcode) {
                inst = candidate;
                break;
            }
        }
    }
    if (inst) {
        emit editInstance(inst->id());
    }
}

void ModpackDashboard::onAddPrivatePackClicked()
{
    bool ok;
    QString code = QInputDialog::getText(this, tr("Add Private Pack"),
                                         tr("Enter unique modpack shortcode:"),
                                         QLineEdit::Normal, "", &ok);
    if (ok && !code.trimmed().isEmpty()) {
        code = code.trimmed().toUpper();
        QStringList privatePacks = APPLICATION->settings()->get("PrivatePacks").toStringList();
        if (!privatePacks.contains(code)) {
            privatePacks.append(code);
            APPLICATION->settings()->set("PrivatePacks", privatePacks);
            refreshDashboard();
        }
    }
}


