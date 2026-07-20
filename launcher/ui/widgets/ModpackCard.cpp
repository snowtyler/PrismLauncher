#include "ModpackCard.h"
#include "Application.h"
#include "InstanceList.h"
#include "settings/SettingsObject.h"
#include <QNetworkRequest>
#include <QNetworkAccessManager>
#include <QPixmap>
#include <QMessageBox>

ModpackCard::ModpackCard(const QJsonObject& packData, bool adminMode, QWidget* parent)
    : QFrame(parent), m_packData(packData), m_adminMode(adminMode)
{
    m_shortcode = packData["shortcode"].toString();
    m_name = packData["name"].toString();
    m_version = packData["version"].toString();
    m_bannerUrl = packData["banner_url"].toString();
    QString description = packData["description"].toString();

    setFrameStyle(QFrame::StyledPanel | QFrame::Raised);
    setLineWidth(1);
    setFixedSize(260, 360);

    // Apply main card styles
    setStyleSheet(
        "ModpackCard {"
        "  background-color: #1e1e2e;"
        "  border: 1px solid #313244;"
        "  border-radius: 12px;"
        "}"
        "ModpackCard:hover {"
        "  border: 2px solid #b4befe;"
        "  background-color: #252538;"
        "}"
    );

    auto layout = new QVBoxLayout(this);
    layout->setContentsMargins(10, 10, 10, 10);
    layout->setSpacing(8);

    // Banner placeholder
    m_bannerLabel = new QLabel(this);
    m_bannerLabel->setFixedSize(240, 120);
    m_bannerLabel->setStyleSheet("background-color: #11111b; border-radius: 8px;");
    m_bannerLabel->setAlignment(Qt::AlignCenter);
    m_bannerLabel->setText(tr("Loading image..."));
    layout->addWidget(m_bannerLabel);

    // Title & Version
    auto titleLayout = new QHBoxLayout();
    m_titleLabel = new QLabel(m_name, this);
    m_titleLabel->setStyleSheet("color: #cdd6f4; font-size: 16px; font-weight: bold;");
    titleLayout->addWidget(m_titleLabel);

    m_versionLabel = new QLabel(QString("v%1").arg(m_version), this);
    m_versionLabel->setStyleSheet("color: #a6adc8; font-size: 12px;");
    m_versionLabel->setAlignment(Qt::AlignRight | Qt::AlignVCenter);
    titleLayout->addWidget(m_versionLabel);
    layout->addLayout(titleLayout);

    // Description
    m_descLabel = new QLabel(description, this);
    m_descLabel->setStyleSheet("color: #bac2de; font-size: 12px;");
    m_descLabel->setWordWrap(true);
    m_descLabel->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_descLabel->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
    layout->addWidget(m_descLabel);

    // Main action button (Play / Update / Install)
    m_actionButton = new QPushButton(this);
    m_actionButton->setCursor(Qt::PointingHandCursor);
    layout->addWidget(m_actionButton);
    connect(m_actionButton, &QPushButton::clicked, this, &ModpackCard::onActionButtonClicked);

    // Action Panel (Settings & Delete)
    m_bottomWidget = new QWidget(this);
    auto bottomLayout = new QHBoxLayout(m_bottomWidget);
    bottomLayout->setContentsMargins(0, 0, 0, 0);
    bottomLayout->setSpacing(6);

    m_settingsButton = new QPushButton(tr("Settings"), m_bottomWidget);
    m_settingsButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #313244; color: #f5e0dc;"
        "  border: 1px solid #45475a; border-radius: 6px; padding: 6px;"
        "  width: 100%;"
        "}"
        "QPushButton:hover { background-color: #45475a; }"
    );
    m_settingsButton->setCursor(Qt::PointingHandCursor);
    bottomLayout->addWidget(m_settingsButton);
    connect(m_settingsButton, &QPushButton::clicked, this, &ModpackCard::onSettingsButtonClicked);

    m_deleteButton = new QPushButton(tr("Delete"), m_bottomWidget);
    m_deleteButton->setStyleSheet(
        "QPushButton {"
        "  background-color: #313244; color: #f38ba8;"
        "  border: 1px solid #45475a; border-radius: 6px; padding: 6px;"
        "  width: 100%;"
        "}"
        "QPushButton:hover { background-color: #f38ba8; color: #11111b; }"
    );
    m_deleteButton->setCursor(Qt::PointingHandCursor);
    bottomLayout->addWidget(m_deleteButton);
    connect(m_deleteButton, &QPushButton::clicked, this, &ModpackCard::onDeleteButtonClicked);

    layout->addWidget(m_bottomWidget);
    m_bottomWidget->setVisible(m_adminMode);

    updateStatus();
    fetchBanner();
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
        m_status = PackStatus::Install;
        m_actionButton->setText(tr("INSTALL"));
        m_actionButton->setStyleSheet(
            "QPushButton {"
            "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #89b4fa, stop:1 #74c7ec);"
            "  color: #11111b; font-weight: bold; border-radius: 8px; padding: 10px;"
            "}"
            "QPushButton:hover { background: #b4befe; }"
        );
        m_bottomWidget->setVisible(false);
    } else {
        m_status = PackStatus::Play;
        m_actionButton->setText(tr("PLAY"));
        m_actionButton->setStyleSheet(
            "QPushButton {"
            "  background: qlineargradient(x1:0, y1:0, x2:1, y2:0, stop:0 #a6e3a1, stop:1 #94e2d5);"
            "  color: #11111b; font-weight: bold; border-radius: 8px; padding: 10px;"
            "}"
            "QPushButton:hover { background: #a6e3a1; }"
        );
        m_bottomWidget->setVisible(true);
    }
}

void ModpackCard::onActionButtonClicked()
{
    if (m_status == PackStatus::Install) {
        emit actionTriggered("install", m_shortcode);
    } else {
        emit actionTriggered("play", m_shortcode);
    }
}


void ModpackCard::onSettingsButtonClicked()
{
    emit settingsTriggered(m_shortcode);
}

void ModpackCard::onDeleteButtonClicked()
{
    emit actionTriggered("delete", m_shortcode);
}

void ModpackCard::fetchBanner()
{
    if (m_bannerUrl.isEmpty()) {
        m_bannerLabel->setText(m_name.left(1));
        m_bannerLabel->setStyleSheet("background-color: #45475a; color: #cdd6f4; font-size: 32px; font-weight: bold; border-radius: 8px;");
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
            m_bannerLabel->setPixmap(pixmap.scaled(240, 120, Qt::KeepAspectRatioByExpanding, Qt::SmoothTransformation));
            m_bannerLabel->setText("");
            return;
        }
    }

    // Fallback if loading image fails
    m_bannerLabel->setText(m_name.left(1));
    m_bannerLabel->setStyleSheet("background-color: #45475a; color: #cdd6f4; font-size: 32px; font-weight: bold; border-radius: 8px;");
}
