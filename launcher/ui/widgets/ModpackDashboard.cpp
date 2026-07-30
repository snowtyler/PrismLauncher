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

ModpackDashboard::ModpackDashboard(QWidget* parent)
    : QScrollArea(parent)
{
    setWidgetResizable(true);
    setFrameShape(QFrame::NoFrame);
    setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);

    // Apply dashboard dark background
    setStyleSheet(R"(
        QScrollArea {
            background-color: #161822;
        }
        QWidget#centralWidget {
            background-color: #161822;
        }
    )");

    m_centralWidget = new QWidget(this);
    m_centralWidget->setObjectName("centralWidget");
    setWidget(m_centralWidget);

    // Outer layout to center the content container horizontally on large screens
    auto* outerLayout = new QHBoxLayout(m_centralWidget);
    outerLayout->setContentsMargins(15, 60, 15, 40);
    outerLayout->setSpacing(0);

    auto* contentWidget = new QWidget(m_centralWidget);
    contentWidget->setFixedWidth(960); // Fixed container width prevents layout shifts during loading/refreshing
    outerLayout->addWidget(contentWidget, 0, Qt::AlignHCenter | Qt::AlignTop);

    auto* mainLayout = new QVBoxLayout(contentWidget);
    mainLayout->setContentsMargins(0, 0, 0, 0);
    mainLayout->setSpacing(36);

    // Top Navigation Bar (Column 0: Dummy spacer 160px | Column 1: Search + Update Btn Centered | Column 2: Enter Pack Code Btn 160px)
    auto* navLayout = new QGridLayout();
    navLayout->setContentsMargins(0, 0, 0, 0);
    navLayout->setSpacing(0);
    navLayout->setColumnStretch(0, 1);
    navLayout->setColumnStretch(1, 0);
    navLayout->setColumnStretch(2, 1);

    auto* leftSpacer = new QWidget(contentWidget);
    leftSpacer->setFixedWidth(160);
    navLayout->addWidget(leftSpacer, 0, 0, Qt::AlignLeft);

    auto* centerContainer = new QWidget(contentWidget);
    auto* centerGroup = new QHBoxLayout(centerContainer);
    centerGroup->setContentsMargins(0, 0, 0, 0);
    centerGroup->setSpacing(8);
    centerGroup->setAlignment(Qt::AlignCenter);

    m_searchEdit = new QLineEdit(centerContainer);
    m_searchEdit->setPlaceholderText(tr("Search Modpacks..."));
    m_searchEdit->setClearButtonEnabled(true);
    m_searchEdit->setFixedWidth(360);
    m_searchEdit->setStyleSheet(R"(
        QLineEdit {
            background-color: #262936;
            color: #FFFFFF;
            border: 1px solid #373B4D;
            border-radius: 16px;
            padding: 7px 16px;
            font-size: 13px;
        }
        QLineEdit:focus {
            border: 1px solid #4B5263;
        }
    )");
    centerGroup->addWidget(m_searchEdit);

    connect(m_searchEdit, &QLineEdit::textChanged, this, [this](const QString&) {
        applyFilter();
    });

    // Check for Modpack Updates Button
    m_refreshBtn = new QPushButton(centerContainer);
    m_refreshBtn->setFixedSize(36, 36);
    m_refreshBtn->setCursor(Qt::PointingHandCursor);
    m_refreshBtn->setToolTip(tr("Check for modpack updates"));

    QIcon refreshIcon = QIcon::fromTheme("checkupdate");
    if (refreshIcon.isNull()) refreshIcon = QIcon::fromTheme("view-refresh");
    if (refreshIcon.isNull()) refreshIcon = QIcon::fromTheme("refresh");

    if (!refreshIcon.isNull()) {
        m_refreshBtn->setIcon(refreshIcon);
        m_refreshBtn->setIconSize(QSize(18, 18));
    } else {
        m_refreshBtn->setText(QString::fromUtf8("\xE2\x86\xBB"));
    }

    m_refreshBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #262936;
            color: #E2E8F0;
            border: 1px solid #373B4D;
            border-radius: 18px;
            font-size: 15px;
            font-weight: bold;
        }
        QPushButton:hover {
            background-color: #373B4D;
            color: #FFFFFF;
        }
    )");
    centerGroup->addWidget(m_refreshBtn);

    connect(m_refreshBtn, &QPushButton::clicked, this, [this]() {
        emit checkModpackUpdatesRequested();
        refreshDashboard();
    });

    navLayout->addWidget(centerContainer, 0, 1, Qt::AlignCenter);

    // Far Right: Enter Pack Code Button
    m_enterCodeBtn = new QPushButton(tr("[+] Enter Pack Code"), contentWidget);
    m_enterCodeBtn->setCursor(Qt::PointingHandCursor);
    m_enterCodeBtn->setFixedWidth(160);
    m_enterCodeBtn->setStyleSheet(R"(
        QPushButton {
            background-color: #373B4D;
            color: #FFFFFF;
            font-weight: bold;
            border: 1px solid #4B5263;
            border-radius: 16px;
            padding: 7px 16px;
            font-size: 12px;
        }
        QPushButton:hover {
            background-color: #4B5263;
        }
    )");
    navLayout->addWidget(m_enterCodeBtn, 0, 2, Qt::AlignRight);

    connect(m_enterCodeBtn, &QPushButton::clicked, this, &ModpackDashboard::onAddPrivatePackClicked);

    mainLayout->addLayout(navLayout);

    // Grid Layout for Cards (3 columns)
    m_gridLayout = new QGridLayout();
    m_gridLayout->setSpacing(16);
    mainLayout->addLayout(m_gridLayout);

    // Loading indicator
    m_loadingLabel = new QLabel(tr("Checking for modpacks..."), contentWidget);
    m_loadingLabel->setStyleSheet("color: #9CA3AF; font-size: 15px;");
    m_loadingLabel->setAlignment(Qt::AlignCenter);
    mainLayout->addWidget(m_loadingLabel, 1, Qt::AlignCenter);

    fetchRegistry();

    // Periodically update card statuses if instances change
    auto updateCardStatuses = [this]() {
        for (auto* card : m_cards) {
            card->updateStatus();
        }
        applyFilter();
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

    // Clear previous cards & placeholders
    for (auto* card : m_cards) {
        m_gridLayout->removeWidget(card);
        card->deleteLater();
    }
    m_cards.clear();

    for (auto* ph : m_placeholders) {
        m_gridLayout->removeWidget(ph);
        ph->deleteLater();
    }
    m_placeholders.clear();

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

    // Fetch private packs stored locally
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
    // Append local synced instances if not in m_packs
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
                    pack["description"] = tr("Local synced instance.");
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

    // Create cards for all packs
    for (int i = 0; i < m_packs.size(); ++i) {
        auto* card = new ModpackCard(m_packs[i], m_adminMode, m_centralWidget);
        m_cards.append(card);

        connect(card, &ModpackCard::actionTriggered, this, &ModpackDashboard::onCardActionTriggered);
        connect(card, &ModpackCard::settingsTriggered, this, &ModpackDashboard::onCardSettingsTriggered);
    }

    applyFilter();
}

void ModpackDashboard::applyFilter()
{
    // Remove existing grid items from layout
    for (auto* card : m_cards) {
        m_gridLayout->removeWidget(card);
        card->hide();
    }
    for (auto* ph : m_placeholders) {
        m_gridLayout->removeWidget(ph);
        ph->deleteLater();
    }
    m_placeholders.clear();

    QString query = m_searchEdit->text().trimmed().toLower();
    QList<ModpackCard*> visibleCards;

    for (auto* card : m_cards) {
        bool searchMatch = query.isEmpty() ||
                           m_packs[m_cards.indexOf(card)]["name"].toString().toLower().contains(query) ||
                           m_packs[m_cards.indexOf(card)]["description"].toString().toLower().contains(query);

        if (searchMatch) {
            visibleCards.append(card);
        }
    }

    int columns = 3;
    int cardIndex = 0;

    // Place matching pack cards into grid (3 columns)
    for (; cardIndex < visibleCards.size(); ++cardIndex) {
        auto* card = visibleCards[cardIndex];
        m_gridLayout->addWidget(card, cardIndex / columns, cardIndex % columns);
        card->show();
    }

    // If visible cards are less than 3, add placeholder cards to complete at least 3 cards per row
    int minCardsRequired = 3;
    while (cardIndex < minCardsRequired) {
        auto* placeholder = new PlaceholderModpackCard(m_centralWidget);
        m_gridLayout->addWidget(placeholder, cardIndex / columns, cardIndex % columns);
        m_placeholders.append(placeholder);
        cardIndex++;
    }

    m_loadingLabel->setVisible(visibleCards.isEmpty() && m_cards.isEmpty());
}

void ModpackDashboard::onCardActionTriggered(const QString& action, const QString& shortcode)
{
    BaseInstance* inst = nullptr;
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i]->shortcode() == shortcode) {
            inst = m_cards[i]->localInstance();
            break;
        }
    }

    if (action == "play") {
        if (inst) {
            emit launchInstance(inst->id());
        }
    } else if (action == "update" || action == "repair") {
        if (inst) {
            auto updateTask = makeShared<SyncedInstanceUpdateTask>(inst);
            if (action == "repair") {
                updateTask->setForceRepair(true);
            }
            ProgressDialog updateDialog(this);
            updateDialog.execWithTask(updateTask.get());

            if (updateTask->wasSuccessful()) {
                inst->setHasModpackUpdate(false);
                for (auto* card : m_cards) {
                    card->updateStatus();
                }
                applyFilter();
                if (action == "repair") {
                    QMessageBox::information(this, tr("Repair Complete"), tr("The instance has been repaired successfully. All missing or damaged files were restored."));
                }
            } else if (!updateTask->failReason().isEmpty()) {
                QMessageBox::critical(this, action == "repair" ? tr("Repair Failed") : tr("Update Failed"), updateTask->failReason());
            }
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
                refreshDashboard();
            }
        }
    } else if (action == "install") {
        // Fetch manifest & install
        QString publicUrl = APPLICATION->settings()->get("SyncR2PublicUrl").toString();
        if (!publicUrl.endsWith('/')) {
            publicUrl += '/';
        }

        m_loadingLabel->setVisible(true);
        m_loadingLabel->setText(tr("Fetching modpack manifest..."));

        QUrl url(publicUrl + "shortcodes/" + shortcode + ".json");
        QNetworkReply* reply = APPLICATION->network()->get(QNetworkRequest(url));
        connect(reply, &QNetworkReply::finished, this, [this, reply, shortcode]() {
            reply->deleteLater();
            m_loadingLabel->setVisible(false);

            if (reply->error() == QNetworkReply::NoError) {
                QByteArray data = reply->readAll();
                QJsonDocument doc = QJsonDocument::fromJson(data);
                if (!doc.isNull() && doc.isObject()) {
                    runInstall(shortcode, doc.object());
                }
            } else {
                QMessageBox::critical(this, tr("Error"), tr("Failed to fetch modpack manifest: %1").arg(reply->errorString()));
            }
        });
    }
}

void ModpackDashboard::onCardSettingsTriggered(const QString& shortcode)
{
    for (int i = 0; i < m_cards.size(); ++i) {
        if (m_cards[i]->shortcode() == shortcode && m_cards[i]->localInstance()) {
            emit editInstance(m_cards[i]->localInstance()->id());
            break;
        }
    }
}

void ModpackDashboard::onAddPrivatePackClicked()
{
    bool ok;
    QString code = QInputDialog::getText(this, tr("Add Private Modpack"),
                                         tr("Enter private modpack code (e.g. TOO3EX):"), QLineEdit::Normal,
                                         "", &ok);
    if (ok && !code.trimmed().isEmpty()) {
        code = code.trimmed().toUpper();

        QStringList privateCodes = APPLICATION->settings()->get("PrivatePacks").toStringList();
        if (!privateCodes.contains(code)) {
            privateCodes.append(code);
            APPLICATION->settings()->set("PrivatePacks", privateCodes);
        }

        refreshDashboard();
    }
}

class SimpleVersion : public BaseVersion {
    QString m_ver;
public:
    explicit SimpleVersion(QString ver) : m_ver(std::move(ver)) {}
    QString descriptor() const override { return m_ver; }
    QString name() const override { return m_ver; }
    QString typeString() const override { return "Release"; }
};

void ModpackDashboard::runInstall(const QString& shortcode, const QJsonObject& manifest)
{
    QString packName = manifest["name"].toString();
    if (packName.isEmpty()) packName = shortcode;

    QString mcVersion = manifest["mc_version"].toString();
    if (mcVersion.isEmpty()) mcVersion = "1.21.1";

    QString loaderName = manifest["loader"].toString();
    if (loaderName.isEmpty()) loaderName = "NeoForge";

    QString version = manifest["version"].toString();
    if (version.isEmpty()) version = "1.0.0";

    auto mcVer = std::make_shared<SimpleVersion>(mcVersion);
    auto rawTask = new VanillaCreationTask(mcVer);
    rawTask->setName(packName);
    rawTask->setGroup("Synced Modpacks");

    unique_qobject_ptr<Task> task(APPLICATION->instances()->wrapInstanceTask(rawTask));

    ProgressDialog dialog(this);
    dialog.execWithTask(task.get());

    if (!task->wasSuccessful()) {
        return;
    }

    // Post-process created instance
    BaseInstance* inst = nullptr;
    for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
        if (APPLICATION->instances()->at(i)->name() == packName) {
            inst = APPLICATION->instances()->at(i);
            break;
        }
    }

    if (inst) {
        inst->settings()->set("IsSyncedInstance", true);
        inst->settings()->set("SyncShortcode", shortcode);
        inst->saveNow();

        // Perform initial sync
        auto updateTask = makeShared<SyncedInstanceUpdateTask>(inst);
        ProgressDialog syncDialog(this);
        syncDialog.execWithTask(updateTask.get());

        refreshDashboard();
    }
}

