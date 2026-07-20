#pragma once

#include <QFrame>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QNetworkReply>
#include "BaseInstance.h"

class ModpackCard : public QFrame {
    Q_OBJECT
public:
    explicit ModpackCard(const QJsonObject& packData, bool adminMode, QWidget* parent = nullptr);
    virtual ~ModpackCard() = default;

    void setAdminMode(bool enabled);
    void updateStatus();
    BaseInstance* localInstance() const { return getLocalInstance(); }

signals:
    void actionTriggered(const QString& action, const QString& shortcode);
    void settingsTriggered(const QString& shortcode);

private slots:
    void onActionButtonClicked();
    void onSettingsButtonClicked();
    void onDeleteButtonClicked();
    void bannerDownloaded();

private:
    QJsonObject m_packData;
    bool m_adminMode;
    QString m_shortcode;
    QString m_name;
    QString m_version;
    QString m_bannerUrl;

    QLabel* m_bannerLabel = nullptr;
    QLabel* m_titleLabel = nullptr;
    QLabel* m_versionLabel = nullptr;
    QLabel* m_descLabel = nullptr;
    QPushButton* m_actionButton = nullptr;
    
    QWidget* m_bottomWidget = nullptr;
    QPushButton* m_settingsButton = nullptr;
    QPushButton* m_deleteButton = nullptr;

    QNetworkReply* m_bannerReply = nullptr;

    enum class PackStatus {
        Install,
        Update,
        Play
    } m_status = PackStatus::Install;

    BaseInstance* getLocalInstance() const;
    void fetchBanner();
};
