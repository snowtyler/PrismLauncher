#pragma once

#include "tasks/Task.h"
#include "BaseInstance.h"
#include <QNetworkReply>
#include <QList>

class ModpackUpdateCheckTask : public Task {
    Q_OBJECT
public:
    explicit ModpackUpdateCheckTask(QList<BaseInstance*> instances);
    virtual ~ModpackUpdateCheckTask() = default;

    bool abort() override;

    QList<BaseInstance*> updatedInstances() const { return m_updatedInstances; }

protected:
    void executeTask() override;

private:
    void checkNextOrFinish();
    void checkInstance(BaseInstance* instance);

private:
    QList<BaseInstance*> m_instancesToCheck;
    QList<BaseInstance*> m_updatedInstances;
    int m_pendingCount = 0;
    QList<QNetworkReply*> m_replies;
};
