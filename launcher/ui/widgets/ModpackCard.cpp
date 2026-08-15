#include "ModpackCard.h"
#include "Application.h"
#include "InstanceList.h"
#include "settings/SettingsObject.h"
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QMenu>
#include <QAction>
#include <QStyle>
#include <QPainter>
#include <QPainterPath>
#include <QDebug>

static QPixmap getRoundedPixmap(const QPixmap& src, int radius, int width, int height)
{
    QPixmap scaled = src.scaled(width, height, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation);

    QPixmap cropped(width, height);
    cropped.fill(Qt::transparent);
    {
        QPainter p(&cropped);
        int x = (scaled.width() - width) / 2;
        int y = (scaled.height() - height) / 2;
        p.drawPixmap(0, 0, scaled, x, y, width, height);
    }

    QPixmap rounded(width, height);
    rounded.fill(Qt::transparent);

    QPainter painter(&rounded);
    painter.setRenderHint(QPainter::Antialiasing, true);
    painter.setRenderHint(QPainter::SmoothPixmapTransform, true);

    QPainterPath path;
    path.addRoundedRect(0, 0, width, height, radius, radius);
    painter.setClipPath(path);
    painter.drawPixmap(0, 0, cropped);

    return rounded;
}

static QIcon createMoreOptionsIcon(int size = 24, const QColor& color = QColor("#E2E8F0"))
{
    QPixmap pixmap(size * 2, size * 2);
    pixmap.fill(Qt::transparent);
    pixmap.setDevicePixelRatio(2.0);
    {
        QPainter p(&pixmap);
        p.setRenderHint(QPainter::Antialiasing, true);
        p.setBrush(color);
        p.setPen(Qt::NoPen);

        qreal cx = size / 2.0;
        qreal r = 2.0;
        qreal spacing = 6.0;

        p.drawEllipse(QPointF(cx, cx - spacing), r, r);
        p.drawEllipse(QPointF(cx, cx), r, r);
        p.drawEllipse(QPointF(cx, cx + spacing), r, r);
    }
    return QIcon(pixmap);
}

ModpackCard::ModpackCard(const QJsonObject& packData, bool adminMode, QWidget* parent)
    : QFrame(parent), m_packData(packData), m_adminMode(adminMode)
{
    m_shortcode = packData["shortcode"].toString();
    m_name = packData["name"].toString();
    m_version = packData["version"].toString();
    if (m_version.isEmpty()) m_version = "1.0.0";

    m_bannerUrl = packData["banner_url"].toString();
    m_loader = packData["loader"].toString();
    if (m_loader.isEmpty()) m_loader = "NeoForge";
    m_mcVersion = packData["mc_version"].toString();
    if (m_mcVersion.isEmpty()) m_mcVersion = "1.21.1";

    QString description = packData["description"].toString();
    if (description.isEmpty()) description = tr("A custom synced modpack.");

    setFrameShape(QFrame::NoFrame);
    setFixedSize(290, 380);

    // Card styling
    setStyleSheet(R"(
        ModpackCard {
            background-color: #20232E;
            border: 1px solid #2F3342;
            border-radius: 12px;
        }
        ModpackCard:hover {
            border: 1px solid #4B5263;
            background-color: #252835;
        }
    )");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(10, 10, 10, 10);
    mainLayout->setSpacing(8);

    // Banner Container with top-right overlay badges
    auto* bannerContainer = new QWidget(this);
    bannerContainer->setFixedSize(270, 135);

    m_bannerLabel = new QLabel(bannerContainer);
    m_bannerLabel->setGeometry(0, 0, 270, 135);
    m_bannerLabel->setStyleSheet("background-color: transparent;");
    m_bannerLabel->setAlignment(Qt::AlignCenter);

    // Overlaid Badges Widget
    auto* badgeWidget = new QWidget(bannerContainer);
    auto* badgeLayout = new QHBoxLayout(badgeWidget);
    badgeLayout->setContentsMargins(0, 0, 0, 0);
    badgeLayout->setSpacing(4);

    m_loaderBadge = new QLabel(m_loader, badgeWidget);
    m_loaderBadge->setStyleSheet("background-color: rgba(18, 20, 26, 0.85); color: #E2E8F0; font-size: 11px; font-weight: bold; border-radius: 4px; padding: 2px 6px;");
    badgeLayout->addWidget(m_loaderBadge);

    m_mcVersionBadge = new QLabel(QString("MC %1").arg(m_mcVersion), badgeWidget);
    m_mcVersionBadge->setStyleSheet("background-color: rgba(18, 20, 26, 0.85); color: #E2E8F0; font-size: 11px; font-weight: bold; border-radius: 4px; padding: 2px 6px;");
    badgeLayout->addWidget(m_mcVersionBadge);

    badgeWidget->adjustSize();
    badgeWidget->move(270 - badgeWidget->width() - 8, 8);

    mainLayout->addWidget(bannerContainer);

    // Title & Version Row
    auto* titleLayout = new QHBoxLayout();
    m_titleLabel = new QLabel(m_name, this);
    m_titleLabel->setStyleSheet("color: #FFFFFF; font-size: 15px; font-weight: bold;");
    titleLayout->addWidget(m_titleLabel, 1);

    m_versionLabel = new QLabel(QString("v%1").arg(m_version), this);
    m_versionLabel->setStyleSheet("color: #9CA3AF; font-size: 12px;");
    m_versionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    titleLayout->addWidget(m_versionLabel);

    mainLayout->addLayout(titleLayout);

    // Description
    m_descLabel = new QLabel(description, this);
    m_descLabel->setStyleSheet("color: #9CA3AF; font-size: 12px; line-height: 1.3;");
    m_descLabel->setWordWrap(true);
    m_descLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_descLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    mainLayout->addWidget(m_descLabel, 1);

    // Update Sub-label
    m_updateSubLabel = new QLabel(this);
    m_updateSubLabel->setStyleSheet("color: #E2E8F0; font-size: 12px; font-weight: bold;");
    m_updateSubLabel->setText(tr("An update is available."));
    m_updateSubLabel->hide();
    mainLayout->addWidget(m_updateSubLabel);

    // Action Row (Main Action Button + Settings Menu Button)
    auto* actionLayout = new QHBoxLayout();
    actionLayout->setSpacing(6);

    m_actionButton = new QPushButton(this);
    m_actionButton->setCursor(Qt::PointingHandCursor);
    actionLayout->addWidget(m_actionButton, 1);
    connect(m_actionButton, &QPushButton::clicked, this, &ModpackCard::onActionButtonClicked);

    m_settingsButton = new QPushButton(this);
    m_settingsButton->setIcon(createMoreOptionsIcon(24));
    m_settingsButton->setIconSize(QSize(20, 20));
    m_settingsButton->setFixedSize(36, 36);
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    m_settingsButton->setStyleSheet(R"(
        QPushButton {
            background-color: #333846;
            border: 1px solid #3F4456;
            border-radius: 8px;
        }
        QPushButton:hover {
            background-color: #3F4456;
        }
    )");
    actionLayout->addWidget(m_settingsButton);
    connect(m_settingsButton, &QPushButton::clicked, this, &ModpackCard::onSettingsButtonClicked);

    mainLayout->addLayout(actionLayout);

    updateStatus();
    fetchBanner();
}

bool ModpackCard::hasUpdate() const
{
    BaseInstance* inst = getLocalInstance();
    if (!inst) return false;
    return inst->hasUpdateAvailable();
}

void ModpackCard::setAdminMode(bool enabled)
{
    m_adminMode = enabled;
    updateStatus();
}

BaseInstance* ModpackCard::getLocalInstance() const
{
    for (int i = 0; i < APPLICATION->instances()->count(); ++i) {
        BaseInstance* inst = APPLICATION->instances()->at(i);
        if (inst->settings()->get("IsSyncedInstance").toBool() &&
            inst->settings()->get("SyncShortcode").toString() == m_shortcode) {
            return inst;
        }
    }
    return nullptr;
}

void ModpackCard::updateStatus()
{
    BaseInstance* inst = getLocalInstance();
    if (!inst) {
        QString displayVer = m_version;
        if (!displayVer.isEmpty() && !displayVer.startsWith('v')) {
            displayVer = "v" + displayVer;
        }
        m_versionLabel->setText(displayVer);

        m_status = PackStatus::Install;
        m_actionButton->setText(tr("INSTALL"));
        m_actionButton->setEnabled(true);
        m_actionButton->setStyleSheet(R"(
            QPushButton {
                background-color: #3B82F6;
                color: #FFFFFF;
                font-weight: bold;
                font-size: 13px;
                border: none;
                border-radius: 8px;
                padding: 9px;
            }
            QPushButton:hover { background-color: #2563EB; }
        )");
        m_updateSubLabel->hide();
    } else {
        connect(inst, &BaseInstance::runningStatusChanged, this, &ModpackCard::updateStatus, Qt::UniqueConnection);
        connect(inst, &BaseInstance::propertiesChanged, this, &ModpackCard::updateStatus, Qt::UniqueConnection);

        QString installedVer = inst->settings()->get("SyncVersion").toString();
        if (installedVer.isEmpty()) {
            installedVer = m_version;
        }
        if (!installedVer.isEmpty() && !installedVer.startsWith('v')) {
            installedVer = "v" + installedVer;
        }
        m_versionLabel->setText(installedVer);

        if (inst->isRunning()) {
            m_status = PackStatus::Running;
            m_actionButton->setText(tr("Running"));
            m_actionButton->setEnabled(false);
            m_actionButton->setStyleSheet(R"(
                QPushButton {
                    background-color: #4B5263;
                    color: #9CA3AF;
                    font-weight: bold;
                    font-size: 13px;
                    border: none;
                    border-radius: 8px;
                    padding: 9px;
                }
            )");
            m_updateSubLabel->hide();
        } else if (inst->hasUpdateAvailable()) {
            m_status = PackStatus::Update;
            m_actionButton->setEnabled(true);
            QString targetVer = inst->modpackUpdateVersion();
            if (targetVer.isEmpty()) targetVer = m_version;
            if (targetVer.isEmpty()) targetVer = "1.0.0";
            if (!targetVer.startsWith('v')) targetVer = "v" + targetVer;

            m_actionButton->setText(tr("UPDATE (%1)").arg(targetVer));
            m_actionButton->setStyleSheet(R"(
                QPushButton {
                    background-color: #67E8F9;
                    color: #0F172A;
                    font-weight: bold;
                    font-size: 13px;
                    border: none;
                    border-radius: 8px;
                    padding: 9px;
                }
                QPushButton:hover { background-color: #22D3EE; }
            )");
            m_updateSubLabel->setText(tr("An update is available."));
            m_updateSubLabel->show();
        } else {
            m_status = PackStatus::Play;
            m_actionButton->setEnabled(true);
            m_actionButton->setText(tr("PLAY"));
            m_actionButton->setStyleSheet(R"(
                QPushButton {
                    background-color: #86EFAC;
                    color: #0F172A;
                    font-weight: bold;
                    font-size: 13px;
                    border: none;
                    border-radius: 8px;
                    padding: 9px;
                }
                QPushButton:hover { background-color: #4ADE80; }
            )");
            m_updateSubLabel->hide();
        }
    }
}

void ModpackCard::onActionButtonClicked()
{
    if (m_status == PackStatus::Install) {
        emit actionTriggered("install", m_shortcode);
    } else if (m_status == PackStatus::Update) {
        emit actionTriggered("update", m_shortcode);
    } else if (m_status == PackStatus::Play) {
        emit actionTriggered("play", m_shortcode);
    }
}

void ModpackCard::onSettingsButtonClicked()
{
    QMenu menu(this);
    menu.setStyleSheet(R"(
        QMenu {
            background-color: #242733;
            border: 1px solid #373B4D;
            color: #E2E8F0;
            border-radius: 6px;
            padding: 4px;
        }
        QMenu::item:selected {
            background-color: #373B4D;
        }
    )");

    BaseInstance* inst = getLocalInstance();
    if (inst) {
        QAction* playAction = nullptr;
        if (inst->isRunning()) {
            playAction = menu.addAction(tr("Running"));
            playAction->setEnabled(false);
        } else {
            playAction = menu.addAction(tr("Play Instance"));
            connect(playAction, &QAction::triggered, [this]() { emit actionTriggered("play", m_shortcode); });
        }

        QAction* editAction = menu.addAction(tr("Instance Settings"));
        connect(editAction, &QAction::triggered, [this]() { emit settingsTriggered(m_shortcode); });

        QAction* repairAction = menu.addAction(tr("Repair Instance"));
        connect(repairAction, &QAction::triggered, [this]() { emit actionTriggered("repair", m_shortcode); });

        menu.addSeparator();

        QAction* deleteAction = menu.addAction(tr("Delete Instance"));
        connect(deleteAction, &QAction::triggered, [this]() { emit actionTriggered("delete", m_shortcode); });
    } else {
        QAction* installAction = menu.addAction(tr("Install Modpack"));
        connect(installAction, &QAction::triggered, [this]() { emit actionTriggered("install", m_shortcode); });
    }

    menu.exec(m_settingsButton->mapToGlobal(QPoint(0, m_settingsButton->height())));
}

void ModpackCard::onDeleteButtonClicked()
{
    emit actionTriggered("delete", m_shortcode);
}

void ModpackCard::fetchBanner()
{
    if (m_bannerUrl.isEmpty()) {
        QPixmap letterPixmap(270, 135);
        letterPixmap.fill(QColor("#373B4D"));
        QPainter p(&letterPixmap);
        p.setRenderHint(QPainter::Antialiasing);
        p.setPen(QColor("#E2E8F0"));
        QFont font = p.font();
        font.setPixelSize(36);
        font.setBold(true);
        p.setFont(font);
        p.drawText(letterPixmap.rect(), Qt::AlignCenter, m_name.left(1));
        p.end();

        m_bannerLabel->setPixmap(getRoundedPixmap(letterPixmap, 10, 270, 135));
        return;
    }

    m_bannerReply = APPLICATION->network()->get(QNetworkRequest(QUrl(m_bannerUrl)));
    connect(m_bannerReply, &QNetworkReply::finished, this, &ModpackCard::bannerDownloaded);
}

void ModpackCard::bannerDownloaded()
{
    if (!m_bannerReply) return;
    m_bannerReply->deleteLater();
    auto reply = m_bannerReply;
    m_bannerReply = nullptr;

    if (reply->error() == QNetworkReply::NoError) {
        QPixmap pixmap;
        if (pixmap.loadFromData(reply->readAll())) {
            m_bannerLabel->setPixmap(getRoundedPixmap(pixmap, 10, 270, 135));
            m_bannerLabel->setText("");
            return;
        }
    }

    // Fallback if loading image fails
    QPixmap letterPixmap(270, 135);
    letterPixmap.fill(QColor("#373B4D"));
    QPainter p(&letterPixmap);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QColor("#E2E8F0"));
    QFont font = p.font();
    font.setPixelSize(36);
    font.setBold(true);
    p.setFont(font);
    p.drawText(letterPixmap.rect(), Qt::AlignCenter, m_name.left(1));
    p.end();

    m_bannerLabel->setPixmap(getRoundedPixmap(letterPixmap, 10, 270, 135));
}

// ----------------------------------------------------
// PlaceholderModpackCard Implementation
// ----------------------------------------------------
PlaceholderModpackCard::PlaceholderModpackCard(QWidget* parent)
    : QFrame(parent)
{
    setFrameShape(QFrame::NoFrame);
    setFixedSize(290, 380);

    setStyleSheet(R"(
        PlaceholderModpackCard {
            background-color: rgba(28, 31, 42, 0.4);
            border: 2px dashed #3A3F52;
            border-radius: 12px;
        }
        PlaceholderModpackCard:hover {
            border: 2px dashed #4B5263;
            background-color: rgba(36, 39, 51, 0.6);
        }
    )");

    auto* mainLayout = new QVBoxLayout(this);
    mainLayout->setContentsMargins(15, 15, 15, 15);
    mainLayout->setSpacing(12);

    // Banner Placeholder Area
    auto* bannerBox = new QLabel(tr("[+] Empty Slot"), this);
    bannerBox->setFixedSize(260, 130);
    bannerBox->setAlignment(Qt::AlignCenter);
    bannerBox->setStyleSheet("background-color: rgba(20, 22, 30, 0.6); color: #6B7280; font-size: 14px; font-weight: bold; border-radius: 8px;");
    mainLayout->addWidget(bannerBox);

    // Title
    auto* titleLabel = new QLabel(tr("Modpack Slot"), this);
    titleLabel->setStyleSheet("color: #D1D5DB; font-size: 15px; font-weight: bold;");
    mainLayout->addWidget(titleLabel);

    // Description
    auto* descLabel = new QLabel(tr("No modpack is available for this slot... yet."), this);
    descLabel->setStyleSheet("color: #6B7280; font-size: 12px;");
    descLabel->setWordWrap(true);
    descLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    mainLayout->addWidget(descLabel, 1);
}
