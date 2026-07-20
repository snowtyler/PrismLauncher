#pragma once

#include <QScrollArea>
#include <QWidget>
#include <QGridLayout>
#include <QNetworkReply>
#include <QJsonObject>
#include <QList>
#include <QPushButton>
#include <QLabel>
#include "ModpackCard.h"

class ModpackDashboard : public QScrollArea {
    Q_OBJECT
public:
    explicit ModpackDashboard(QWidget* parent = nullptr);
    virtual ~ModpackDashboard() = default;

    void setAdminMode(bool enabled);
    bool isAdminMode() const { return m_adminMode; }
    void refreshDashboard();

signals:
    void launchInstance(const QString& id);
    void editInstance(const QString& id);

private slots:
    void registryFetched();
    void privatePackManifestFetched();
    void onCardActionTriggered(const QString& action, const QString& shortcode);
    void onCardSettingsTriggered(const QString& shortcode);
    void onAddPrivatePackClicked();

private:
    QWidget* m_centralWidget = nullptr;
    QGridLayout* m_gridLayout = nullptr;
    QWidget* m_headerWidget = nullptr;
    QLabel* m_loadingLabel = nullptr;

    bool m_adminMode = false;
    QList<QJsonObject> m_packs;
    QList<ModpackCard*> m_cards;

    QNetworkReply* m_registryReply = nullptr;
    QList<QNetworkReply*> m_privateReplies;

    void fetchRegistry();
    void renderCards();
    void runInstall(const QString& shortcode, const QJsonObject& manifest);
};
