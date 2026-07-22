#pragma once

#include <QScrollArea>
#include <QWidget>
#include <QGridLayout>
#include <QNetworkReply>
#include <QJsonObject>
#include <QList>
#include <QPushButton>
#include <QLineEdit>
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
    void checkModpackUpdatesRequested();

private slots:
    void registryFetched();
    void privatePackManifestFetched();
    void onCardActionTriggered(const QString& action, const QString& shortcode);
    void onCardSettingsTriggered(const QString& shortcode);
    void onAddPrivatePackClicked();

private:
    QWidget* m_centralWidget = nullptr;
    QGridLayout* m_gridLayout = nullptr;
    QLabel* m_loadingLabel = nullptr;

    QLineEdit* m_searchEdit = nullptr;
    QPushButton* m_refreshBtn = nullptr;
    QPushButton* m_enterCodeBtn = nullptr;

    bool m_adminMode = false;
    QList<QJsonObject> m_packs;
    QList<ModpackCard*> m_cards;
    QList<PlaceholderModpackCard*> m_placeholders;

    QNetworkReply* m_registryReply = nullptr;
    QList<QNetworkReply*> m_privateReplies;

    void fetchRegistry();
    void renderCards();
    void applyFilter();
    void runInstall(const QString& shortcode, const QJsonObject& manifest);
};
